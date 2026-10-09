/*
 * SadieDate - a minimal, non-invasive date-spoofing launcher.
 *
 * PURPOSE
 *   Runs a legacy program (e.g. a SADiE workstation whose time-limited build
 *   refuses to start on the real date) while the system clock temporarily
 *   reads an earlier date. Unlike tools such as RunAsDate, this launcher does
 *   NOT inject a DLL or hook any API inside the target process. The target
 *   runs completely unmodified, so its output (audio in particular) is
 *   bit-for-bit identical to a normal run.
 *
 * KEEPING CORRECT FILE DATES
 *   Because the whole system clock is rolled back while the target runs, any
 *   file the target writes is stamped with the back-date. To keep your real
 *   dates, this launcher offers two mechanisms (see SadieDate.ini):
 *
 *   (A) Timestamp correction (default). When the target exits, the launcher
 *       walks the folders you list in WatchDirs and corrects the dates of the
 *       files that were written during the session, setting them to the real
 *       moment they were written. This only changes date metadata on files
 *       that are already closed -- it never touches their contents.
 *
 *   (B) Early restore (RestoreAfterSeconds > 0). If your build only checks the
 *       date when it starts, the launcher can restore the real clock a few
 *       seconds after launch, while the program keeps running. Everything you
 *       then save is stamped with the correct real date automatically.
 *
 * HOW IT WORKS
 *   1. Save the current local time and a monotonic uptime counter.
 *   2. Roll the system clock back to the configured target date.
 *   3. Launch the target program.
 *   4. (Optional) After RestoreAfterSeconds, restore the real clock.
 *   5. Wait for the program to exit.
 *   6. Restore the real clock if not already restored.
 *   7. (Optional) Correct the dates of files written during the session.
 *   The clock is also restored if this launcher is closed, Ctrl-C'd, or the
 *   machine logs off / shuts down while the target is running.
 *
 * COMPATIBILITY
 *   Builds as a single 32-bit executable that runs on Windows XP, 7 and 10,
 *   on both 32-bit and 64-bit editions (via WOW64 on 64-bit).
 *
 * REQUIRES
 *   Administrator rights (changing the system clock needs the SE_SYSTEMTIME
 *   privilege). The bundled manifest requests elevation on Vista and newer;
 *   on XP, run as an administrator account.
 *
 * USAGE
 *   Configure SadieDate.ini (next to the .exe) and double-click, or:
 *     SadieDate.exe "YYYY-MM-DD" "C:\Path\To\SADiE.exe" [extra args...]
 *     SadieDate.exe "YYYY-MM-DD HH:MM:SS" "C:\Path\To\SADiE.exe"
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <stdio.h>
#include <wchar.h>

#define ONE_SECOND_100NS  10000000ULL   /* 1 s in 100-ns units */
#define WINDOW_SLACK_100NS (5ULL * ONE_SECOND_100NS)

/* ------------------------------------------------------------------ */
/* Global state shared with the console control handler and fix-up.    */
/* ------------------------------------------------------------------ */
static SYSTEMTIME g_savedLocalTime;   /* real local time at launch        */
static ULONGLONG  g_startTickMs;      /* monotonic uptime (ms) at launch  */
static int        g_haveTick64 = 0;   /* GetTickCount64 available?         */
static volatile LONG g_restored = 0;  /* ensures we restore exactly once   */
static ULONGLONG  g_spoofElapsedMs = 0; /* how long the clock was spoofed  */

/* Window (LOCAL-basis FILETIME, 100-ns units) during which the clock was
 * rolled back, plus the constant correction offset. Set once the clock is
 * changed; used by the timestamp fix-up after the session. */
static ULONGLONG  g_windowStartLocal = 0;
static LONGLONG   g_offset100ns = 0;

typedef ULONGLONG (WINAPI *PFN_GetTickCount64)(void);
static PFN_GetTickCount64 g_pGetTickCount64 = NULL;

/* ------------------------------------------------------------------ */
/* Small time helpers                                                  */
/* ------------------------------------------------------------------ */
static ULONGLONG ft_to_ull(const FILETIME *ft)
{
    return ((ULONGLONG)ft->dwHighDateTime << 32) | (ULONGLONG)ft->dwLowDateTime;
}
static void ull_to_ft(ULONGLONG v, FILETIME *ft)
{
    ft->dwLowDateTime  = (DWORD)(v & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

/* Monotonic millisecond counter, unaffected by changing the clock.
 * Uses GetTickCount64 on Vista+, falls back to 32-bit GetTickCount on XP. */
static ULONGLONG now_ms(void)
{
    if (g_haveTick64 && g_pGetTickCount64)
        return g_pGetTickCount64();
    return (ULONGLONG)GetTickCount();
}

/* Elapsed ms since launch. DWORD subtraction tolerates a single 32-bit
 * wraparound (~49.7 days) on XP; GetTickCount64 never wraps in practice. */
static ULONGLONG elapsed_ms(void)
{
    ULONGLONG now = now_ms();
    if (g_haveTick64)
        return now - g_startTickMs;
    return (ULONGLONG)((DWORD)now - (DWORD)g_startTickMs);
}

/* ------------------------------------------------------------------ */
/* Privilege handling                                                  */
/* ------------------------------------------------------------------ */
static BOOL enable_time_privilege(void)
{
    HANDLE hToken = NULL;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    BOOL ok = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return FALSE;

    if (LookupPrivilegeValueW(NULL, SE_SYSTEMTIME_NAME, &luid)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL)
            && GetLastError() == ERROR_SUCCESS)
            ok = TRUE;
    }
    CloseHandle(hToken);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Clock set / restore                                                 */
/* ------------------------------------------------------------------ */

/* Restore the real clock exactly once: saved time + elapsed uptime.
 * Records how long the clock was spoofed (for the fix-up window). */
static void restore_clock(void)
{
    FILETIME ft;
    ULONGLONG t;
    SYSTEMTIME st;
    ULONGLONG elapsed;

    /* Run only on the first caller (handler vs. normal path race). */
    if (InterlockedExchange(&g_restored, 1) != 0)
        return;

    elapsed = elapsed_ms();
    g_spoofElapsedMs = elapsed;

    if (!SystemTimeToFileTime(&g_savedLocalTime, &ft))
        return;

    t  = ft_to_ull(&ft);
    t += elapsed * 10000ULL;                /* ms -> 100-ns units */
    ull_to_ft(t, &ft);

    if (FileTimeToSystemTime(&ft, &st))
        SetLocalTime(&st);
}

/* Console/logoff/shutdown/Ctrl-C handler: never leave the clock wrong. */
static BOOL WINAPI ctrl_handler(DWORD ctrlType)
{
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        restore_clock();
        break;
    default:
        break;
    }
    return FALSE; /* let default handling proceed */
}

/* ------------------------------------------------------------------ */
/* Timestamp fix-up                                                    */
/* ------------------------------------------------------------------ */

/* Is a file's (UTC) timestamp inside the spoofed window? Compared in
 * local basis so it lines up with the local target date the user set. */
static BOOL in_spoof_window(const FILETIME *utcFt)
{
    FILETIME lf;
    ULONGLONG v, lo, hi;

    if (utcFt->dwLowDateTime == 0 && utcFt->dwHighDateTime == 0)
        return FALSE;                       /* unset timestamp */
    if (!FileTimeToLocalFileTime(utcFt, &lf))
        return FALSE;

    v  = ft_to_ull(&lf);
    lo = (g_windowStartLocal > WINDOW_SLACK_100NS)
         ? g_windowStartLocal - WINDOW_SLACK_100NS : 0;
    hi = g_windowStartLocal
         + g_spoofElapsedMs * 10000ULL
         + WINDOW_SLACK_100NS;
    return (v >= lo && v <= hi);
}

/* Shift one timestamp forward by the correction offset if it falls inside
 * the spoofed window. Returns 1 if it changed. */
static int adjust_time(FILETIME *ft)
{
    if (in_spoof_window(ft)) {
        ULONGLONG v = ft_to_ull(ft);
        v = (ULONGLONG)((LONGLONG)v + g_offset100ns);
        ull_to_ft(v, ft);
        return 1;
    }
    return 0;
}

/* Correct the create/access/write times of one filesystem entry. */
static void fix_entry(const wchar_t *path, unsigned *pCount)
{
    HANDLE f;
    FILETIME c, a, w;
    int changed = 0;

    f = CreateFileW(path,
                    FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, /* lets us open directories too */
                    NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;

    if (GetFileTime(f, &c, &a, &w)) {
        changed |= adjust_time(&c);
        changed |= adjust_time(&a);
        changed |= adjust_time(&w);
        if (changed && SetFileTime(f, &c, &a, &w))
            (*pCount)++;
    }
    CloseHandle(f);
}

/* Recursively correct timestamps under one directory. */
static void fix_tree(const wchar_t *dir, int depth, unsigned *pCount)
{
    wchar_t pattern[MAX_PATH];
    wchar_t full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (depth > 64)                         /* guard against loops */
        return;

    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    pattern[MAX_PATH - 1] = L'\0';

    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0)
            continue;

        if (_snwprintf(full, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) < 0)
            continue;                       /* path too long - skip safely */
        full[MAX_PATH - 1] = L'\0';

        fix_entry(full, pCount);            /* file or directory */

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            fix_tree(full, depth + 1, pCount);
    } while (FindNextFileW(h, &fd));

    FindClose(h);
}

/* Walk a semicolon-separated list of folders and correct timestamps. */
static void fix_watch_dirs(const wchar_t *dirs)
{
    wchar_t buf[2048];
    wchar_t *p, *start;
    unsigned count = 0;

    if (g_offset100ns <= 0 || dirs == NULL || dirs[0] == L'\0')
        return;

    wcsncpy(buf, dirs, 2047);
    buf[2047] = L'\0';

    start = buf;
    for (p = buf; ; ++p) {
        if (*p == L';' || *p == L'\0') {
            wchar_t saved = *p;
            *p = L'\0';
            /* trim leading spaces */
            while (*start == L' ') ++start;
            if (*start != L'\0') {
                if (GetFileAttributesW(start) != INVALID_FILE_ATTRIBUTES) {
                    wprintf(L"  Correcting dates in: %ls\n", start);
                    fix_tree(start, 0, &count);
                } else {
                    wprintf(L"  (skipped, not found: %ls)\n", start);
                }
            }
            if (saved == L'\0')
                break;
            start = p + 1;
        }
    }
    wprintf(L"  Done. Corrected dates on %u item(s).\n", count);
}

/* ------------------------------------------------------------------ */
/* Misc helpers                                                        */
/* ------------------------------------------------------------------ */

/* Parse "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS" (also accepts / or .).
 * Returns the number of fields parsed (>=3 on success), 0 on failure. */
static int parse_target(const wchar_t *s,
                        int *y, int *mo, int *d, int *h, int *mi, int *sec)
{
    int n;
    *h = *mi = *sec = -1;
    n = swscanf(s, L"%d%*[-/.]%d%*[-/.]%d %d:%d:%d", y, mo, d, h, mi, sec);
    if (n < 3)
        return 0;
    if (*y < 1601 || *y > 30827) return 0;  /* FILETIME valid range */
    if (*mo < 1 || *mo > 12)     return 0;
    if (*d  < 1 || *d  > 31)     return 0;
    return n;
}

/* Copy the directory part of a path into dir (no trailing slash). */
static BOOL dir_of_path(const wchar_t *path, wchar_t *dir, size_t dirCount)
{
    const wchar_t *slash = NULL, *p;
    size_t len;
    for (p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/')
            slash = p;
    if (!slash)
        return FALSE;
    len = (size_t)(slash - path);
    if (len >= dirCount)
        len = dirCount - 1;
    wcsncpy(dir, path, len);
    dir[len] = L'\0';
    return TRUE;
}

static void press_enter_to_exit(void)
{
    fputws(L"\nPress Enter to close...", stdout);
    fflush(stdout);
    (void)getwchar();
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int wmain(int argc, wchar_t **argv)
{
    wchar_t exePath[MAX_PATH];
    wchar_t iniPath[MAX_PATH];
    wchar_t program[MAX_PATH]   = {0};
    wchar_t workdir[MAX_PATH]   = {0};
    wchar_t targetStr[128]      = {0};
    wchar_t extraArgs[1024]     = {0};
    wchar_t watchDirs[2048]     = {0};
    wchar_t cmdline[MAX_PATH + 1024];
    const wchar_t *workdirPtr = NULL;
    int pauseOnExit = 0;
    int fixFileDates = 1;
    int restoreAfterSeconds = 0;
    int fromArgs = 0;

    int y, mo, d, h, mi, sec, nfields;
    FILETIME ftSaved, ftTarget;
    SYSTEMTIME stTarget;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HMODULE hKernel;

    /* Locate GetTickCount64 (absent on XP). */
    hKernel = GetModuleHandleW(L"kernel32.dll");
    if (hKernel) {
        FARPROC proc = GetProcAddress(hKernel, "GetTickCount64");
        g_pGetTickCount64 = (PFN_GetTickCount64)(void (*)(void))proc;
        g_haveTick64 = (g_pGetTickCount64 != NULL);
    }

    /* Build path to SadieDate.ini next to this executable. */
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wcscpy(iniPath, exePath);
    {
        wchar_t *slash = wcsrchr(iniPath, L'\\');
        if (slash)
            wcscpy(slash + 1, L"SadieDate.ini");
        else
            wcscpy(iniPath, L"SadieDate.ini");
    }

    wprintf(L"SadieDate - non-invasive date launcher\n");
    wprintf(L"--------------------------------------\n");

    /* ---- Gather configuration: command line overrides INI ---- */
    if (argc >= 3) {
        fromArgs = 1;
        wcsncpy(targetStr, argv[1], 127);
        wcsncpy(program,   argv[2], MAX_PATH - 1);
        {
            int i;
            extraArgs[0] = L'\0';
            for (i = 3; i < argc; ++i) {
                if (extraArgs[0] != L'\0')
                    wcsncat(extraArgs, L" ", 1023 - wcslen(extraArgs));
                wcsncat(extraArgs, argv[i], 1023 - wcslen(extraArgs));
            }
        }
    } else if (argc == 2 &&
               (wcscmp(argv[1], L"/?") == 0 ||
                wcscmp(argv[1], L"-h") == 0 ||
                wcscmp(argv[1], L"--help") == 0)) {
        wprintf(L"\nUsage:\n");
        wprintf(L"  Configure SadieDate.ini next to this .exe, then double-click it.\n");
        wprintf(L"  Or from a command prompt:\n");
        wprintf(L"    SadieDate.exe \"YYYY-MM-DD\" \"C:\\Path\\To\\SADiE.exe\" [args]\n");
        wprintf(L"    SadieDate.exe \"YYYY-MM-DD HH:MM:SS\" \"C:\\Path\\To\\SADiE.exe\"\n");
        return 0;
    } else {
        GetPrivateProfileStringW(L"SadieDate", L"TargetDate", L"",
                                 targetStr, 128, iniPath);
        GetPrivateProfileStringW(L"SadieDate", L"Program", L"",
                                 program, MAX_PATH, iniPath);
        GetPrivateProfileStringW(L"SadieDate", L"WorkingDir", L"",
                                 workdir, MAX_PATH, iniPath);
        GetPrivateProfileStringW(L"SadieDate", L"Args", L"",
                                 extraArgs, 1024, iniPath);
        GetPrivateProfileStringW(L"SadieDate", L"WatchDirs", L"",
                                 watchDirs, 2048, iniPath);
        pauseOnExit = (int)GetPrivateProfileIntW(L"SadieDate",
                                 L"PauseOnExit", 0, iniPath);
        fixFileDates = (int)GetPrivateProfileIntW(L"SadieDate",
                                 L"FixFileDates", 1, iniPath);
        restoreAfterSeconds = (int)GetPrivateProfileIntW(L"SadieDate",
                                 L"RestoreAfterSeconds", 0, iniPath);
    }

    if (targetStr[0] == L'\0' || program[0] == L'\0') {
        wprintf(L"\nERROR: No configuration found.\n");
        if (!fromArgs) {
            wprintf(L"Edit SadieDate.ini (expected at:\n  %ls\n)\n", iniPath);
            wprintf(L"and set TargetDate and Program, or pass them on the command line.\n");
        }
        press_enter_to_exit();
        return 2;
    }

    nfields = parse_target(targetStr, &y, &mo, &d, &h, &mi, &sec);
    if (!nfields) {
        wprintf(L"\nERROR: Could not understand TargetDate \"%ls\".\n", targetStr);
        wprintf(L"Use YYYY-MM-DD or YYYY-MM-DD HH:MM:SS.\n");
        press_enter_to_exit();
        return 2;
    }

    /* Validate the target program exists BEFORE touching the clock. */
    if (GetFileAttributesW(program) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"\nERROR: Program not found:\n  %ls\n", program);
        wprintf(L"Check the Program path in SadieDate.ini.\n");
        press_enter_to_exit();
        return 3;
    }

    if (!enable_time_privilege()) {
        wprintf(L"\nERROR: Could not acquire permission to change the clock.\n");
        wprintf(L"Right-click SadieDate.exe and choose \"Run as administrator\"\n");
        wprintf(L"(on Windows XP, use an administrator account).\n");
        press_enter_to_exit();
        return 4;
    }

    /* Capture the real time + uptime reference (order matters). */
    GetLocalTime(&g_savedLocalTime);
    g_startTickMs = now_ms();

    /* Build the spoofed SYSTEMTIME: keep time-of-day unless user pinned one. */
    stTarget = g_savedLocalTime;
    stTarget.wYear      = (WORD)y;
    stTarget.wMonth     = (WORD)mo;
    stTarget.wDay       = (WORD)d;
    stTarget.wDayOfWeek = 0;
    if (h   >= 0) stTarget.wHour   = (WORD)h;
    if (mi  >= 0) stTarget.wMinute = (WORD)mi;
    if (sec >= 0) { stTarget.wSecond = (WORD)sec; stTarget.wMilliseconds = 0; }

    /* Pre-compute the correction offset and window start (local basis). */
    if (SystemTimeToFileTime(&g_savedLocalTime, &ftSaved) &&
        SystemTimeToFileTime(&stTarget, &ftTarget)) {
        g_windowStartLocal = ft_to_ull(&ftTarget);
        g_offset100ns = (LONGLONG)ft_to_ull(&ftSaved)
                      - (LONGLONG)ft_to_ull(&ftTarget);
    }

    /* Install the restore handler BEFORE changing the clock. */
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    if (!SetLocalTime(&stTarget)) {
        wprintf(L"\nERROR: Failed to set the system clock (error %lu).\n",
                (unsigned long)GetLastError());
        press_enter_to_exit();
        return 5;
    }

    wprintf(L"\nReal time saved:  %04d-%02d-%02d %02d:%02d:%02d\n",
            g_savedLocalTime.wYear, g_savedLocalTime.wMonth,
            g_savedLocalTime.wDay, g_savedLocalTime.wHour,
            g_savedLocalTime.wMinute, g_savedLocalTime.wSecond);
    wprintf(L"Clock set to:     %04d-%02d-%02d %02d:%02d:%02d\n",
            stTarget.wYear, stTarget.wMonth, stTarget.wDay,
            stTarget.wHour, stTarget.wMinute, stTarget.wSecond);
    wprintf(L"Launching:        %ls\n\n", program);

    /* Working directory: explicit, else the program's own folder. */
    if (workdir[0] != L'\0') {
        workdirPtr = workdir;
    } else if (dir_of_path(program, workdir, MAX_PATH)) {
        workdirPtr = workdir;
    }

    /* Build command line: "program" [args] */
    if (extraArgs[0] != L'\0')
        _snwprintf(cmdline, MAX_PATH + 1024, L"\"%ls\" %ls", program, extraArgs);
    else
        _snwprintf(cmdline, MAX_PATH + 1024, L"\"%ls\"", program);
    cmdline[MAX_PATH + 1023] = L'\0';

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(program, cmdline, NULL, NULL, FALSE,
                        0, NULL, workdirPtr, &si, &pi)) {
        wprintf(L"ERROR: Failed to launch the program (error %lu).\n",
                (unsigned long)GetLastError());
        restore_clock();
        press_enter_to_exit();
        return 6;
    }

    /* Mode B: restore the real clock a few seconds after launch, while the
     * program keeps running (good when the date is only checked at startup). */
    if (restoreAfterSeconds > 0) {
        DWORD waited = WaitForSingleObject(pi.hProcess,
                                           (DWORD)restoreAfterSeconds * 1000);
        if (waited == WAIT_TIMEOUT) {       /* still running -> restore now */
            restore_clock();
            wprintf(L">>> Real clock restored; the program is still running.\n");
            wprintf(L">>> Files you save from now on get the correct real date.\n\n");
        }
    } else {
        wprintf(L">>> The clock will be restored when the program closes.\n");
        wprintf(L">>> Keep this window open while you work.\n\n");
    }

    /* Wait for the target to exit. */
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* Restore the real clock if not already restored. */
    restore_clock();

    {
        SYSTEMTIME nowLocal;
        GetLocalTime(&nowLocal);
        wprintf(L"\nProgram closed. Clock restored to: %04d-%02d-%02d %02d:%02d:%02d\n",
                nowLocal.wYear, nowLocal.wMonth, nowLocal.wDay,
                nowLocal.wHour, nowLocal.wMinute, nowLocal.wSecond);
    }

    /* Correct the dates of files written during the spoofed window. */
    if (fixFileDates && g_offset100ns > 0 && watchDirs[0] != L'\0') {
        wprintf(L"\nCorrecting file dates written during the session...\n");
        fix_watch_dirs(watchDirs);
    } else if (fixFileDates && watchDirs[0] == L'\0' && restoreAfterSeconds == 0) {
        wprintf(L"\nNOTE: Files written while the program ran were stamped with the\n");
        wprintf(L"      back-date. Set WatchDirs in SadieDate.ini (your audio/project\n");
        wprintf(L"      output folders) so their dates are corrected automatically.\n");
    }

    if (pauseOnExit)
        press_enter_to_exit();

    return 0;
}

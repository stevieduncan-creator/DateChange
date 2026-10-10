/*
 * SadieDate.exe  -  launcher/injector for the date-only shim.
 *
 * Launches your program suspended, injects SadieHook{32,64}.dll (which makes
 * only that process + its plugins see an earlier date), then resumes it. The
 * real system clock is never changed, so files keep the correct date and drive
 * mounts / network auth are unaffected. Needs NO administrator rights -- it
 * only touches a process it created itself.
 *
 * Two executables are built: SadieDate.exe (32-bit) and SadieDate64.exe
 * (64-bit). Run either one; if the target program is the other bitness, it
 * automatically hands off to its sibling. So you can always just run
 * SadieDate.exe.
 *
 * CONFIGURATION (SadieDate.ini, in the same folder)
 *   Single program:
 *       [SadieDate]
 *       TargetDate=2008-01-01
 *       Program=C:\Program Files\SADiE\SADiE.exe
 *   Several versions (different folders / names / expiry dates):
 *       [Programs]
 *       1=SADiE 5 | C:\Program Files\SADiE 5\Sadie5.exe | 2008-01-01
 *       2=SADiE 6 | D:\Audio\SADiE 6\Sadie6.exe         | 2010-06-01
 *   With more than one program listed, SadieDate shows a menu. You can also
 *   pick one directly:   SadieDate.exe 2        (by number)
 *                        SadieDate.exe "SADiE 6" (by label)
 *
 * Command line (overrides the INI):
 *     SadieDate.exe "YYYY-MM-DD" "C:\Path\To\SADiE.exe" [extra args...]
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
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>

#define SADIEDATE_BUILD "2026-10-10f"

/* ---- optional diagnostics (INI: [SadieDate] Debug=1) --------------------- */
static int     g_debug = 0;
static wchar_t g_logpathW[MAX_PATH] = {0};
static void jlog(const char *fmt, ...)
{
    char buf[512], line[520];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';
    if (g_debug)
        wprintf(L"  [debug] %hs\n", buf);
    if (g_logpathW[0]) {
        HANDLE f = CreateFileW(g_logpathW, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wr;
            int m = _snprintf(line, sizeof(line) - 1, "%s\r\n", buf);
            if (m < 0) m = (int)sizeof(line) - 1;
            SetFilePointer(f, 0, NULL, FILE_END);
            WriteFile(f, line, (DWORD)m, &wr, NULL);
            CloseHandle(f);
        }
    }
}

/* Last-resort crash logger: records where the launcher died, then lets it end. */
static LONG WINAPI launcher_crashfilter(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord)
        jlog("launcher: *** CRASH code=0x%08lx addr=%p ***",
             (unsigned long)ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    else
        jlog("launcher: *** CRASH (no info) ***");
    return EXCEPTION_EXECUTE_HANDLER;
}

#ifdef _WIN64
#  define SELF_IS_64   1
#  define HOOK_DLL_W   L"SadieHook64.dll"
#  define SIBLING_W    L"SadieDate.exe"      /* the 32-bit build */
#else
#  define SELF_IS_64   0
#  define HOOK_DLL_W   L"SadieHook32.dll"
#  define SIBLING_W    L"SadieDate64.exe"    /* the 64-bit build */
#endif

#define MAX_PROGRAMS 64

typedef struct {
    wchar_t label[80];
    wchar_t path[MAX_PATH];
    wchar_t date[64];           /* empty = use the global TargetDate */
} Program;

static void press_enter_to_exit(void)
{
    fputws(L"\nPress Enter to close...", stdout);
    fflush(stdout);
    (void)getwchar();
}

/* Trim leading/trailing spaces and tabs in place. */
static void trimw(wchar_t *s)
{
    wchar_t *p = s, *e;
    while (*p == L' ' || *p == L'\t') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t' ||
                     e[-1] == L'\r' || e[-1] == L'\n')) *--e = L'\0';
}

/* Map a HookLevel name (or number) to the function bitmask. Bits:
 * 1 GetLocalTime, 2 GetSystemTime, 4 GetSystemTimeAsFileTime,
 * 8 GetSystemTimePreciseAsFileTime, 16 NtQuerySystemTime. */
static unsigned map_hooklevel(const wchar_t *s)
{
    wchar_t t[32];
    wchar_t *end = NULL;
    unsigned long v;

    wcsncpy(t, s ? s : L"", 31); t[31] = L'\0';
    trimw(t); _wcslwr(t);

    if (!t[0] || !wcscmp(t, L"full"))     return 0x1F;  /* everything */
    if (!wcscmp(t, L"safe"))              return 0x07;  /* no NtQuery, no Precise */
    if (!wcscmp(t, L"minimal"))           return 0x03;  /* GetLocalTime+GetSystemTime */
    if (!wcscmp(t, L"calendar"))          return 0x01;  /* GetLocalTime only */
    v = wcstoul(t, &end, 0);
    if (end && *end == L'\0' && v != 0)   return (unsigned)(v & 0x1F);
    return 0x1F;
}

/* Parse "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS" (also accepts / or .). */
static int parse_target(const wchar_t *s,
                        int *y, int *mo, int *d, int *h, int *mi, int *sec)
{
    int n;
    *h = *mi = *sec = -1;
    n = swscanf(s, L"%d%*[-/.]%d%*[-/.]%d %d:%d:%d", y, mo, d, h, mi, sec);
    if (n < 3) return 0;
    if (*y < 1601 || *y > 30827) return 0;
    if (*mo < 1 || *mo > 12) return 0;
    if (*d < 1 || *d > 31) return 0;
    return n;
}

static BOOL dir_of_path(const wchar_t *path, wchar_t *dir, size_t n)
{
    const wchar_t *slash = NULL, *p;
    size_t len;
    for (p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') slash = p;
    if (!slash) return FALSE;
    len = (size_t)(slash - path);
    if (len >= n) len = n - 1;
    wcsncpy(dir, path, len);
    dir[len] = L'\0';
    return TRUE;
}

/* PE machine type of an on-disk executable, or 0 on error. */
static WORD read_machine(const wchar_t *path)
{
    HANDLE f;
    IMAGE_DOS_HEADER dos;
    DWORD rd, sig;
    IMAGE_FILE_HEADER fh;
    WORD machine = 0;

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (ReadFile(f, &dos, sizeof dos, &rd, NULL) && rd == sizeof dos &&
        dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(f, dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(f, &sig, 4, &rd, NULL) && rd == 4 && sig == IMAGE_NT_SIGNATURE &&
        ReadFile(f, &fh, sizeof fh, &rd, NULL) && rd == sizeof fh) {
        machine = fh.Machine;
    }
    CloseHandle(f);
    return machine;
}

/* Read the [Programs] list from the INI. Returns the count found. */
static int gather_programs(const wchar_t *iniPath, Program *out, int maxN)
{
    int i, n = 0;
    for (i = 1; i <= 99 && n < maxN; i++) {
        wchar_t key[8], line[600];
        wchar_t *bar1, *bar2;
        swprintf(key, 8, L"%d", i);
        GetPrivateProfileStringW(L"Programs", key, L"", line, 600, iniPath);
        trimw(line);
        if (!line[0])
            continue;                       /* allow gaps in numbering */

        /* Split "Label | Path | Date" (Date optional). */
        bar1 = wcschr(line, L'|');
        if (!bar1) {                        /* no label given: path only */
            out[n].label[0] = L'\0';
            wcsncpy(out[n].path, line, MAX_PATH - 1);
            out[n].path[MAX_PATH - 1] = L'\0';
            out[n].date[0] = L'\0';
        } else {
            *bar1 = L'\0';
            wcsncpy(out[n].label, line, 79); out[n].label[79] = L'\0';
            bar2 = wcschr(bar1 + 1, L'|');
            if (bar2) {
                *bar2 = L'\0';
                wcsncpy(out[n].date, bar2 + 1, 63); out[n].date[63] = L'\0';
            } else {
                out[n].date[0] = L'\0';
            }
            wcsncpy(out[n].path, bar1 + 1, MAX_PATH - 1);
            out[n].path[MAX_PATH - 1] = L'\0';
        }
        trimw(out[n].label);
        trimw(out[n].path);
        trimw(out[n].date);
        if (!out[n].label[0]) {             /* default label = exe name */
            const wchar_t *base = out[n].path, *p;
            for (p = out[n].path; *p; ++p)
                if (*p == L'\\' || *p == L'/') base = p + 1;
            wcsncpy(out[n].label, base, 79); out[n].label[79] = L'\0';
        }
        if (out[n].path[0])
            n++;
    }
    return n;
}

/* Pick a program: by index (1-based), by label substring, or interactively. */
static int choose_program(Program *progs, int nprog, const wchar_t *selector)
{
    int choice = -1;

    if (nprog <= 0) return -1;
    if (nprog == 1 && !selector) return 0;

    if (selector && selector[0]) {
        /* numeric selector? */
        wchar_t *end = NULL;
        long v = wcstol(selector, &end, 10);
        if (end && *end == L'\0' && v >= 1 && v <= nprog)
            return (int)v - 1;
        /* else case-insensitive label substring match (first hit) */
        {
            int i, hit = -1, hits = 0;
            for (i = 0; i < nprog; i++) {
                wchar_t lab[80], sel[80];
                wcsncpy(lab, progs[i].label, 79); lab[79] = 0;
                wcsncpy(sel, selector, 79); sel[79] = 0;
                _wcslwr(lab); _wcslwr(sel);
                if (wcsstr(lab, sel)) { hit = i; hits++; }
            }
            if (hits == 1) return hit;
            if (hits > 1)
                wprintf(L"\n\"%ls\" matches more than one program; please choose:\n",
                        selector);
            else
                wprintf(L"\n\"%ls\" matched no program; please choose:\n", selector);
        }
    }

    /* Interactive menu. */
    for (;;) {
        wchar_t buf[64];
        int i;
        long v;
        wchar_t *end = NULL;

        wprintf(L"\nWhich program would you like to run?\n");
        for (i = 0; i < nprog; i++)
            wprintf(L"  %d) %ls\n      %ls\n", i + 1, progs[i].label, progs[i].path);
        wprintf(L"Enter 1-%d (or Q to quit): ", nprog);
        fflush(stdout);

        if (!fgetws(buf, 64, stdin))
            return -1;
        trimw(buf);
        if (buf[0] == L'q' || buf[0] == L'Q')
            return -1;
        v = wcstol(buf, &end, 10);
        if (end && *end == L'\0' && v >= 1 && v <= nprog) {
            choice = (int)v - 1;
            break;
        }
        wprintf(L"  (not a valid choice)\n");
    }
    return choice;
}

/* Hand off to the other-bitness sibling with resolved values (no re-prompt). */
static int dispatch_to_sibling(const wchar_t *selfDir, const wchar_t *date,
                               const wchar_t *program, const wchar_t *extra)
{
    wchar_t sibling[MAX_PATH];
    wchar_t cmd[MAX_PATH * 2 + 1152];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    _snwprintf(sibling, MAX_PATH, L"%ls%ls", selfDir, SIBLING_W);
    sibling[MAX_PATH - 1] = L'\0';
    if (GetFileAttributesW(sibling) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"\nERROR: This SADiE is the other bitness, and its helper\n"
                L"       (%ls) is missing from:\n  %ls\n", SIBLING_W, selfDir);
        press_enter_to_exit();
        return 10;
    }

    if (extra && extra[0])
        _snwprintf(cmd, MAX_PATH * 2 + 1152, L"\"%ls\" \"%ls\" \"%ls\" %ls",
                   sibling, date, program, extra);
    else
        _snwprintf(cmd, MAX_PATH * 2 + 1152, L"\"%ls\" \"%ls\" \"%ls\"",
                   sibling, date, program);
    cmd[MAX_PATH * 2 + 1151] = L'\0';

    wprintf(L"Target is %ls-bit; handing off to %ls ...\n",
            SELF_IS_64 ? L"32" : L"64", SIBLING_W);
    jlog("launcher: handing off to sibling %ls for %ls", SIBLING_W, program);

    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(sibling, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        jlog("launcher: sibling launch FAILED err=%lu", (unsigned long)GetLastError());
        wprintf(L"ERROR: Could not start %ls (error %lu).\n",
                SIBLING_W, (unsigned long)GetLastError());
        press_enter_to_exit();
        return 12;
    }
    jlog("launcher: sibling launched OK");
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

/* Inject HOOK_DLL_W into a suspended process. Returns TRUE on success. */
static BOOL inject(HANDLE hProc, const wchar_t *dllPath)
{
    SIZE_T bytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remote;
    HMODULE k32;
    FARPROC pLoadLib;
    HANDLE hThread;
    DWORD exitCode = 0;
    BOOL ok = FALSE;

    k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return FALSE;
    pLoadLib = GetProcAddress(k32, "LoadLibraryW");
    if (!pLoadLib) { jlog("inject: no LoadLibraryW?!"); return FALSE; }

    remote = VirtualAllocEx(hProc, NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
    if (!remote) {
        jlog("inject: VirtualAllocEx failed, err=%lu", (unsigned long)GetLastError());
        return FALSE;
    }

    if (!WriteProcessMemory(hProc, remote, dllPath, bytes, NULL)) {
        jlog("inject: WriteProcessMemory failed, err=%lu", (unsigned long)GetLastError());
    } else {
        hThread = CreateRemoteThread(hProc, NULL, 0,
                      (LPTHREAD_START_ROUTINE)pLoadLib, remote, 0, NULL);
        if (!hThread) {
            jlog("inject: CreateRemoteThread failed, err=%lu",
                 (unsigned long)GetLastError());
        } else {
            DWORD w = WaitForSingleObject(hThread, 15000);
            GetExitCodeThread(hThread, &exitCode);  /* nonzero HMODULE = loaded */
            ok = (exitCode != 0);
            jlog("inject: remote thread wait=%lu LoadLibrary returned HMODULE=0x%lx (%s)",
                 (unsigned long)w, (unsigned long)exitCode,
                 ok ? "loaded" : "FAILED to load DLL");
            CloseHandle(hThread);
        }
    }
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    return ok;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t exePath[MAX_PATH], selfDir[MAX_PATH], iniPath[MAX_PATH];
    wchar_t program[MAX_PATH] = {0}, workdir[MAX_PATH] = {0};
    wchar_t targetStr[128] = {0}, extraArgs[1024] = {0};
    wchar_t dllPath[MAX_PATH], cmdline[MAX_PATH + 1024], offBuf[64];
    const wchar_t *workdirPtr = NULL;
    int y, mo, d, h, mi, sec;
    WORD machine;
    SYSTEMTIME realLocal, stTarget;
    FILETIME ftReal, ftTarget;
    LONGLONG offset;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wcscpy(selfDir, exePath);
    { wchar_t *s = wcsrchr(selfDir, L'\\'); if (s) s[1] = L'\0'; else selfDir[0] = L'\0'; }
    _snwprintf(iniPath, MAX_PATH, L"%lsSadieDate.ini", selfDir);

    wprintf(L"SadieDate - date-only shim launcher (%ls-bit)\n",
            SELF_IS_64 ? L"64" : L"32");
    wprintf(L"---------------------------------------------\n");

    /* Which date functions the shim should hook. Set it once here so it is
     * inherited by SADiE (and by the sibling launcher across a hand-off). */
    {
        wchar_t hookLevel[32], hbuf[16];
        GetPrivateProfileStringW(L"SadieDate", L"HookLevel", L"full",
                                 hookLevel, 32, iniPath);
        _snwprintf(hbuf, 16, L"%u", map_hooklevel(hookLevel));
        SetEnvironmentVariableW(L"SADIEDATE_HOOKS", hbuf);
    }
    /* Hook GetProcAddress/LoadLibrary too? (default yes; 0 = less invasive) */
    {
        int dyn = (int)GetPrivateProfileIntW(L"SadieDate", L"HookDynamic", 1, iniPath);
        SetEnvironmentVariableW(L"SADIEDATE_DYN", dyn ? L"1" : L"0");
    }

    /* Diagnostics. Debug=1 in the INI, or SADIEDATE_LOG already set, turns on a
     * log file next to this exe and step-by-step injection reporting. */
    g_debug = (int)GetPrivateProfileIntW(L"SadieDate", L"Debug", 0, iniPath);
    if (GetEnvironmentVariableW(L"SADIEDATE_LOG", g_logpathW, MAX_PATH) == 0) {
        if (g_debug) {
            _snwprintf(g_logpathW, MAX_PATH, L"%lsSadieDate-log.txt", selfDir);
            g_logpathW[MAX_PATH - 1] = L'\0';
            DeleteFileW(g_logpathW);                 /* start fresh */
            SetEnvironmentVariableW(L"SADIEDATE_LOG", g_logpathW); /* child logs too */
        }
    } else {
        g_debug = 1;   /* a log path was provided by a parent launcher */
    }
    if (g_debug)
        wprintf(L"(debug on; log: %ls)\n", g_logpathW);
    SetUnhandledExceptionFilter(launcher_crashfilter);
    jlog("launcher: start BUILD=%s (%d-bit) argc=%d",
         SADIEDATE_BUILD, SELF_IS_64 ? 64 : 32, argc);
    { int i; for (i = 0; i < argc; ++i) jlog("launcher: argv[%d]=[%ls]", i, argv[i]); }

    if (argc == 2 && (!wcscmp(argv[1], L"/?") || !wcscmp(argv[1], L"-h") ||
                      !wcscmp(argv[1], L"--help"))) {
        wprintf(L"\nConfigure SadieDate.ini and double-click, or:\n"
                L"  SadieDate.exe \"YYYY-MM-DD\" \"C:\\Path\\To\\SADiE.exe\" [args]\n"
                L"  SadieDate.exe <number|name>   (pick from the [Programs] list)\n");
        return 0;
    }

    if (argc >= 3) {
        /* Explicit date + program on the command line. */
        wcsncpy(targetStr, argv[1], 127);
        wcsncpy(program, argv[2], MAX_PATH - 1);
        extraArgs[0] = L'\0';
        { int i; for (i = 3; i < argc; ++i) {
            if (extraArgs[0]) wcsncat(extraArgs, L" ", 1023 - wcslen(extraArgs));
            wcsncat(extraArgs, argv[i], 1023 - wcslen(extraArgs)); } }
    } else {
        /* INI mode. Build the program list; pick one (menu or selector). */
        Program progs[MAX_PROGRAMS];
        int nprog, idx;
        const wchar_t *selector = (argc == 2) ? argv[1] : NULL;
        wchar_t globalDate[128];

        GetPrivateProfileStringW(L"SadieDate", L"TargetDate", L"",
                                 globalDate, 128, iniPath);
        GetPrivateProfileStringW(L"SadieDate", L"WorkingDir", L"",
                                 workdir, MAX_PATH, iniPath);
        GetPrivateProfileStringW(L"SadieDate", L"Args", L"",
                                 extraArgs, 1024, iniPath);

        nprog = gather_programs(iniPath, progs, MAX_PROGRAMS);
        jlog("launcher: INI mode, nprog=%d selector=%ls", nprog,
             selector ? selector : L"(none)");
        if (nprog == 0) {
            /* Fall back to the single Program= line. */
            GetPrivateProfileStringW(L"SadieDate", L"Program", L"",
                                     program, MAX_PATH, iniPath);
            wcsncpy(targetStr, globalDate, 127);
        } else {
            idx = choose_program(progs, nprog, selector);
            jlog("launcher: menu choice idx=%d", idx);
            if (idx < 0) { wprintf(L"\nCancelled.\n"); return 0; }
            wcsncpy(program, progs[idx].path, MAX_PATH - 1);
            program[MAX_PATH - 1] = L'\0';
            wcsncpy(targetStr, progs[idx].date[0] ? progs[idx].date : globalDate, 127);
            targetStr[127] = L'\0';
        }
    }

    jlog("launcher: resolved program=[%ls] date=[%ls]", program, targetStr);

    if (!targetStr[0] || !program[0]) {
        jlog("launcher: no program/date configured");
        wprintf(L"\nERROR: No configuration found.\n"
                L"Edit SadieDate.ini (at %ls): set TargetDate and either a\n"
                L"Program= line or a [Programs] list.\n", iniPath);
        press_enter_to_exit();
        return 2;
    }
    if (!parse_target(targetStr, &y, &mo, &d, &h, &mi, &sec)) {
        wprintf(L"\nERROR: Bad TargetDate \"%ls\". Use YYYY-MM-DD or YYYY-MM-DD HH:MM:SS.\n",
                targetStr);
        press_enter_to_exit();
        return 2;
    }
    if (GetFileAttributesW(program) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"\nERROR: Program not found:\n  %ls\n"
                L"Check the path in SadieDate.ini.\n", program);
        press_enter_to_exit();
        return 3;
    }

    /* If the target is the other bitness, hand off to the sibling build. */
    machine = read_machine(program);
    jlog("launcher: target machine=0x%x (self is %d-bit)", machine, SELF_IS_64 ? 64 : 32);
    if (machine == IMAGE_FILE_MACHINE_AMD64 && !SELF_IS_64)
        return dispatch_to_sibling(selfDir, targetStr, program, extraArgs);
    if (machine == IMAGE_FILE_MACHINE_I386 && SELF_IS_64)
        return dispatch_to_sibling(selfDir, targetStr, program, extraArgs);

    /* Locate the matching hook DLL next to us. */
    _snwprintf(dllPath, MAX_PATH, L"%ls%ls", selfDir, HOOK_DLL_W);
    dllPath[MAX_PATH - 1] = L'\0';
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"\nERROR: Hook library missing:\n  %ls\n"
                L"Keep %ls in the same folder as this program.\n",
                dllPath, HOOK_DLL_W);
        press_enter_to_exit();
        return 4;
    }

    /* Constant offset = real_now - target (a duration, in 100ns). */
    GetLocalTime(&realLocal);
    stTarget = realLocal;
    stTarget.wYear = (WORD)y; stTarget.wMonth = (WORD)mo; stTarget.wDay = (WORD)d;
    stTarget.wDayOfWeek = 0;
    if (h >= 0) stTarget.wHour = (WORD)h;
    if (mi >= 0) stTarget.wMinute = (WORD)mi;
    if (sec >= 0) { stTarget.wSecond = (WORD)sec; stTarget.wMilliseconds = 0; }

    if (!SystemTimeToFileTime(&realLocal, &ftReal) ||
        !SystemTimeToFileTime(&stTarget, &ftTarget)) {
        wprintf(L"\nERROR: Could not compute the date offset.\n");
        press_enter_to_exit();
        return 5;
    }
    offset = (LONGLONG)(((ULONGLONG)ftReal.dwHighDateTime << 32 | ftReal.dwLowDateTime))
           - (LONGLONG)(((ULONGLONG)ftTarget.dwHighDateTime << 32 | ftTarget.dwLowDateTime));
    if (offset <= 0) {
        wprintf(L"\nERROR: TargetDate must be BEFORE the real current date.\n");
        press_enter_to_exit();
        return 5;
    }

    _snwprintf(offBuf, 64, L"%I64d", (__int64)offset);     /* %I64d: XP msvcrt-safe */
    SetEnvironmentVariableW(L"SADIEDATE_OFFSET", offBuf);   /* child inherits this */

    wprintf(L"\nReal date (unchanged):  %04d-%02d-%02d %02d:%02d\n",
            realLocal.wYear, realLocal.wMonth, realLocal.wDay,
            realLocal.wHour, realLocal.wMinute);
    wprintf(L"Program will see:       %04d-%02d-%02d %02d:%02d\n",
            stTarget.wYear, stTarget.wMonth, stTarget.wDay,
            stTarget.wHour, stTarget.wMinute);
    wprintf(L"Launching:              %ls\n", program);

    if (workdir[0]) workdirPtr = workdir;
    else if (dir_of_path(program, workdir, MAX_PATH)) workdirPtr = workdir;

    if (extraArgs[0])
        _snwprintf(cmdline, MAX_PATH + 1024, L"\"%ls\" %ls", program, extraArgs);
    else
        _snwprintf(cmdline, MAX_PATH + 1024, L"\"%ls\"", program);
    cmdline[MAX_PATH + 1023] = L'\0';

    jlog("launcher: CreateProcess (suspended): %ls", program);
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(program, cmdline, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, workdirPtr, &si, &pi)) {
        jlog("launcher: CreateProcess FAILED err=%lu", (unsigned long)GetLastError());
        wprintf(L"\nERROR: Failed to launch the program (error %lu).\n",
                (unsigned long)GetLastError());
        press_enter_to_exit();
        return 6;
    }
    jlog("launcher: process created, pid=%lu", (unsigned long)pi.dwProcessId);

    jlog("launcher: target=%ls machine=0x%x offsetSec=%lu dll=%ls",
         program, machine, (unsigned long)(offset / 10000000), dllPath);

    if (inject(pi.hProcess, dllPath)) {
        wprintf(L"\nDate shim installed. Resuming the program.\n");
        jlog("launcher: inject OK");
    } else {
        wprintf(L"\n*** WARNING: Could not install the date shim. The program will\n"
                L"    start with the REAL date (it may hit its date check). ***\n");
        jlog("launcher: inject FAILED");
    }

    ResumeThread(pi.hThread);
    jlog("launcher: resumed target, launcher exiting normally");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

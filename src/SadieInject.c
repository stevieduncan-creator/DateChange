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
 * Usage:
 *   Configure SadieDate.ini and double-click, or:
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
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#ifdef _WIN64
#  define SELF_IS_64   1
#  define HOOK_DLL_W   L"SadieHook64.dll"
#  define SIBLING_W    L"SadieDate.exe"      /* the 32-bit build */
#else
#  define SELF_IS_64   0
#  define HOOK_DLL_W   L"SadieHook32.dll"
#  define SIBLING_W    L"SadieDate64.exe"    /* the 64-bit build */
#endif

static void press_enter_to_exit(void)
{
    fputws(L"\nPress Enter to close...", stdout);
    fflush(stdout);
    (void)getwchar();
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

/* Hand off to the other-bitness sibling, passing our original arguments. */
static int dispatch_to_sibling(const wchar_t *selfDir)
{
    wchar_t sibling[MAX_PATH];
    wchar_t *cmd;
    int argcW, i;
    LPWSTR *argvW;
    size_t cap;
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

    argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
    cap = MAX_PATH + 8;
    for (i = 1; argvW && i < argcW; i++)
        cap += wcslen(argvW[i]) + 4;
    cmd = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!cmd) return 11;

    _snwprintf(cmd, cap, L"\"%ls\"", sibling);
    for (i = 1; argvW && i < argcW; i++) {
        wcsncat(cmd, L" \"", cap - wcslen(cmd) - 1);
        wcsncat(cmd, argvW[i], cap - wcslen(cmd) - 1);
        wcsncat(cmd, L"\"", cap - wcslen(cmd) - 1);
    }

    wprintf(L"Target is %ls-bit; handing off to %ls ...\n",
            SELF_IS_64 ? L"32" : L"64", SIBLING_W);

    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(sibling, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        wprintf(L"ERROR: Could not start %ls (error %lu).\n",
                SIBLING_W, (unsigned long)GetLastError());
        free(cmd);
        press_enter_to_exit();
        return 12;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    free(cmd);
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
    if (!pLoadLib) return FALSE;

    remote = VirtualAllocEx(hProc, NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
    if (!remote) return FALSE;

    if (WriteProcessMemory(hProc, remote, dllPath, bytes, NULL)) {
        hThread = CreateRemoteThread(hProc, NULL, 0,
                      (LPTHREAD_START_ROUTINE)pLoadLib, remote, 0, NULL);
        if (hThread) {
            WaitForSingleObject(hThread, 15000);
            GetExitCodeThread(hThread, &exitCode);  /* nonzero HMODULE = loaded */
            ok = (exitCode != 0);
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
    int fromArgs = 0, y, mo, d, h, mi, sec;
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

    if (argc >= 3) {
        fromArgs = 1;
        wcsncpy(targetStr, argv[1], 127);
        wcsncpy(program, argv[2], MAX_PATH - 1);
        extraArgs[0] = L'\0';
        { int i; for (i = 3; i < argc; ++i) {
            if (extraArgs[0]) wcsncat(extraArgs, L" ", 1023 - wcslen(extraArgs));
            wcsncat(extraArgs, argv[i], 1023 - wcslen(extraArgs)); } }
    } else if (argc == 2 && (!wcscmp(argv[1], L"/?") || !wcscmp(argv[1], L"-h") ||
                             !wcscmp(argv[1], L"--help"))) {
        wprintf(L"\nConfigure SadieDate.ini and double-click, or:\n"
                L"  SadieDate.exe \"YYYY-MM-DD\" \"C:\\Path\\To\\SADiE.exe\" [args]\n");
        return 0;
    } else {
        GetPrivateProfileStringW(L"SadieDate", L"TargetDate", L"", targetStr, 128, iniPath);
        GetPrivateProfileStringW(L"SadieDate", L"Program", L"", program, MAX_PATH, iniPath);
        GetPrivateProfileStringW(L"SadieDate", L"WorkingDir", L"", workdir, MAX_PATH, iniPath);
        GetPrivateProfileStringW(L"SadieDate", L"Args", L"", extraArgs, 1024, iniPath);
    }

    if (!targetStr[0] || !program[0]) {
        wprintf(L"\nERROR: No configuration found.\n");
        if (!fromArgs)
            wprintf(L"Edit SadieDate.ini (at %ls) and set TargetDate and Program.\n", iniPath);
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
        wprintf(L"\nERROR: Program not found:\n  %ls\n", program);
        press_enter_to_exit();
        return 3;
    }

    /* If the target is the other bitness, hand off to the sibling build. */
    machine = read_machine(program);
    if (machine == IMAGE_FILE_MACHINE_AMD64 && !SELF_IS_64)
        return dispatch_to_sibling(selfDir);
    if (machine == IMAGE_FILE_MACHINE_I386 && SELF_IS_64)
        return dispatch_to_sibling(selfDir);

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

    /* Compute the constant offset = real_now - target (a duration, in 100ns). */
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

    _snwprintf(offBuf, 64, L"%lld", (long long)offset);
    SetEnvironmentVariableW(L"SADIEDATE_OFFSET", offBuf);   /* child inherits this */

    wprintf(L"\nReal date (unchanged):  %04d-%02d-%02d %02d:%02d\n",
            realLocal.wYear, realLocal.wMonth, realLocal.wDay,
            realLocal.wHour, realLocal.wMinute);
    wprintf(L"Program will see:       %04d-%02d-%02d %02d:%02d\n",
            stTarget.wYear, stTarget.wMonth, stTarget.wDay,
            stTarget.wHour, stTarget.wMinute);
    wprintf(L"Launching:              %ls\n", program);

    /* Working directory: explicit, else the program's own folder. */
    if (workdir[0]) workdirPtr = workdir;
    else if (dir_of_path(program, workdir, MAX_PATH)) workdirPtr = workdir;

    if (extraArgs[0])
        _snwprintf(cmdline, MAX_PATH + 1024, L"\"%ls\" %ls", program, extraArgs);
    else
        _snwprintf(cmdline, MAX_PATH + 1024, L"\"%ls\"", program);
    cmdline[MAX_PATH + 1023] = L'\0';

    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(program, cmdline, NULL, NULL, FALSE,
                        CREATE_SUSPENDED, NULL, workdirPtr, &si, &pi)) {
        wprintf(L"\nERROR: Failed to launch the program (error %lu).\n",
                (unsigned long)GetLastError());
        press_enter_to_exit();
        return 6;
    }

    if (inject(pi.hProcess, dllPath)) {
        wprintf(L"\nDate shim installed. Resuming the program.\n");
    } else {
        wprintf(L"\nWARNING: Could not install the date shim; the program will\n"
                L"         start with the REAL date (it may hit its date check).\n");
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* Nothing to clean up: the real clock was never touched. We can exit now;
     * the shim lives inside the target for as long as it runs. */
    return 0;
}

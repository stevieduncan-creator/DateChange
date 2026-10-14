/*
 * SadieHook.dll  -  a surgical, date-ONLY time shim (inline-hook version).
 *
 * Injected into a target process (e.g. SADiE) by SadieDate.exe. It makes the
 * process -- and EVERY DLL/plugin loaded into it, now or later -- see a date
 * shifted into the past by a CONSTANT offset, while the real system clock is
 * left completely alone.
 *
 * HOW IT HOOKS
 *   It installs a short JMP at the very start of each calendar-date function in
 *   kernel32 / ntdll, redirecting every caller to our replacement. Because all
 *   modules ultimately call those same functions, ONE patch per function covers
 *   the whole process -- including plugins loaded dynamically after injection.
 *   There is no import-table patching, and no GetProcAddress / LoadLibrary
 *   hooking (those broad hooks destabilised SADiE).
 *
 *   The replacements never call the original (so no trampoline / instruction
 *   decoding is needed): they read the REAL time directly from the fixed,
 *   unhooked kernel shared-data page (KUSER_SHARED_DATA) and subtract the
 *   constant offset. They touch ONLY calendar-date functions -- never
 *   GetTickCount / timeGetTime / QueryPerformanceCounter -- so audio timing is
 *   untouched, and the offset is constant so every interval is preserved.
 *
 * FAIL-SAFE
 *   If the offset is missing/zero, or a function can't be resolved, it is simply
 *   not hooked and the target runs normally. The code never calls the functions
 *   it patches, so there is no recursion.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SADIEDATE_BUILD "2026-10-14b"

/* ---- optional diagnostic log (path from env SADIEDATE_LOG) ---------------- */
static char g_logpath[MAX_PATH] = {0};
static void logline(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    HANDLE f;
    DWORD wr;
    int n;
    if (!g_logpath[0]) return;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - 2;
    buf[n++] = '\r'; buf[n++] = '\n';
    f = CreateFileA(g_logpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        SetFilePointer(f, 0, NULL, FILE_END);
        WriteFile(f, buf, (DWORD)n, &wr, NULL);
        FlushFileBuffers(f);
        CloseHandle(f);
    }
}

/* ---- crash reporter: on the first access violation, name the faulting
 * address and the module it lives in, so we can tell whether a crash is in our
 * shim or in the host program. Shown as a popup (log files are unreliable on the
 * XP test box) and also written to the log if one is configured. -------------*/
static LONG g_vehCount = 0;
static void module_for_addr(void *addr, wchar_t *out, int cap)
{
    HMODULE h = NULL;
    out[0] = L'\0';
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &h) && h)
        GetModuleFileNameW(h, out, cap);
    if (!out[0]) wcsncpy(out, L"(unknown module)", cap);
}
static LONG CALLBACK veh_logger(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        LONG count = InterlockedIncrement(&g_vehCount);
        if (count <= 6)
            logline("hook: ACCESS VIOLATION at %p", addr);
        if (count == 1) {
            wchar_t mod[MAX_PATH], msg[MAX_PATH + 200];
            module_for_addr(addr, mod, MAX_PATH);
            _snwprintf(msg, MAX_PATH + 200,
                L"A crash occurred. Please screenshot this and send it to Claude.\r\n\r\n"
                L"Fault address: %p\r\nIn module:\r\n%ls",
                addr, mod);
            msg[MAX_PATH + 199] = L'\0';
            MessageBoxW(NULL, msg, L"SadieDate - crash location",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;   /* let normal crash handling proceed */
}

/* Which date functions to hook (bitmask from SADIEDATE_HOOKS). */
#define HOOK_LOCALTIME        0x01   /* GetLocalTime                      */
#define HOOK_SYSTEMTIME       0x02   /* GetSystemTime                     */
#define HOOK_SYSTEMTIME_FT    0x04   /* GetSystemTimeAsFileTime           */
#define HOOK_SYSTEMTIME_PREC  0x08   /* GetSystemTimePreciseAsFileTime    */
#define HOOK_NTQUERY          0x10   /* NtQuerySystemTime                 */
#define HOOK_ALL              0x1F
static unsigned  g_mask = HOOK_ALL;
static LONGLONG  g_offset = 0;        /* 100-ns units to subtract          */
static unsigned  g_hookCount = 0;

/* ---- read the REAL system time from the kernel shared-data page ----------- *
 * KUSER_SHARED_DATA is mapped read-only at a fixed address (0x7FFE0000) in
 * every process on x86 and x64, on every Windows from XP onward (and Wine).
 * SystemTime (UTC, 100-ns units since 1601) is at offset 0x14, stored as
 * {LowPart, High1Time, High2Time}; the loop guards against a torn read.      */
static ULONGLONG read_real_utc_100ns(void)
{
    volatile LONG *p = (volatile LONG *)(ULONG_PTR)(0x7FFE0000 + 0x14);
    LONG high1, high2;
    ULONG low;
    do {
        high1 = p[1];
        low   = (ULONG)p[0];
        high2 = p[2];
    } while (high1 != high2);
    return ((ULONGLONG)(ULONG)high1 << 32) | (ULONGLONG)low;
}
static ULONGLONG spoofed_utc_100ns(void)
{
    return (ULONGLONG)((LONGLONG)read_real_utc_100ns() - g_offset);
}

/* ---- replacement functions (self-contained: no call to the original) ----- */
static void WINAPI my_GetSystemTimeAsFileTime(LPFILETIME ft)
{
    ULONGLONG v = spoofed_utc_100ns();
    if (ft) { ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32); }
}
static void WINAPI my_GetSystemTime(LPSYSTEMTIME st)
{
    ULONGLONG v = spoofed_utc_100ns();
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)v; ft.dwHighDateTime = (DWORD)(v >> 32);
    if (st) FileTimeToSystemTime(&ft, st);
}
static void WINAPI my_GetLocalTime(LPSYSTEMTIME st)
{
    ULONGLONG v = spoofed_utc_100ns();
    FILETIME utc, local;
    utc.dwLowDateTime = (DWORD)v; utc.dwHighDateTime = (DWORD)(v >> 32);
    if (st && FileTimeToLocalFileTime(&utc, &local))
        FileTimeToSystemTime(&local, st);
}
static void WINAPI my_GetSystemTimePreciseAsFileTime(LPFILETIME ft)
{
    my_GetSystemTimeAsFileTime(ft);   /* system-time resolution is fine here */
}
static NTSTATUS NTAPI my_NtQuerySystemTime(PLARGE_INTEGER t)
{
    if (t) t->QuadPart = (LONGLONG)spoofed_utc_100ns();
    return 0; /* STATUS_SUCCESS */
}

/* ---- install a permanent JMP at the start of a function ------------------ */
static int install_inline_hook(void *target, void *stub)
{
    DWORD old;
#ifdef _WIN64
    unsigned char patch[14];
    SIZE_T n = 14;
    patch[0] = 0xFF; patch[1] = 0x25;          /* JMP [RIP+0]                */
    *(DWORD *)(patch + 2) = 0;
    *(void **)(patch + 6) = stub;              /* 64-bit absolute target     */
#else
    unsigned char patch[5];
    SIZE_T n = 5;
    patch[0] = 0xE9;                            /* JMP rel32                  */
    *(LONG *)(patch + 1) = (LONG)((char *)stub - ((char *)target + 5));
#endif
    if (!target)
        return 0;
    if (!VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(target, patch, n);
    VirtualProtect(target, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, n);
    return 1;
}

/* ---- init ---------------------------------------------------------------- */
static BOOL init_shim(void)
{
    HMODULE k32, ntdll;
    char buf[64];
    DWORD n;
    FARPROC pGST, pGLT, pGSTAFT, pGSTPAFT, pNtQ;

    GetEnvironmentVariableA("SADIEDATE_LOG", g_logpath, sizeof(g_logpath));
    logline("hook: DllMain attach, BUILD=%s pid=%lu (inline)",
            SADIEDATE_BUILD, (unsigned long)GetCurrentProcessId());
    AddVectoredExceptionHandler(1, veh_logger);   /* report a crash's location */

    n = GetEnvironmentVariableA("SADIEDATE_OFFSET", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        logline("hook: SADIEDATE_OFFSET not set -> shim disabled");
        return FALSE;
    }
    g_offset = _strtoi64(buf, NULL, 10);
    if (g_offset == 0) {
        logline("hook: offset is 0 -> shim disabled");
        return FALSE;
    }
    n = GetEnvironmentVariableA("SADIEDATE_HOOKS", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        unsigned m = (unsigned)strtoul(buf, NULL, 10);
        if (m != 0) g_mask = m & HOOK_ALL;
    }

    k32 = GetModuleHandleW(L"kernel32.dll");
    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!k32) {
        logline("hook: no kernel32?! -> shim disabled");
        return FALSE;
    }

    /* GetProcAddress resolves forwarders (e.g. kernel32 -> kernelbase), so we
     * patch the real implementation that every caller ends up at. */
    pGLT     = GetProcAddress(k32, "GetLocalTime");
    pGST     = GetProcAddress(k32, "GetSystemTime");
    pGSTAFT  = GetProcAddress(k32, "GetSystemTimeAsFileTime");
    pGSTPAFT = GetProcAddress(k32, "GetSystemTimePreciseAsFileTime"); /* Win8+ */
    pNtQ     = ntdll ? GetProcAddress(ntdll, "NtQuerySystemTime") : NULL;

    if (g_mask & HOOK_LOCALTIME)
        g_hookCount += install_inline_hook((void *)pGLT,     (void *)my_GetLocalTime);
    if (g_mask & HOOK_SYSTEMTIME)
        g_hookCount += install_inline_hook((void *)pGST,     (void *)my_GetSystemTime);
    if (g_mask & HOOK_SYSTEMTIME_FT)
        g_hookCount += install_inline_hook((void *)pGSTAFT,  (void *)my_GetSystemTimeAsFileTime);
    if (g_mask & HOOK_SYSTEMTIME_PREC)
        g_hookCount += install_inline_hook((void *)pGSTPAFT, (void *)my_GetSystemTimePreciseAsFileTime);
    if (g_mask & HOOK_NTQUERY)
        g_hookCount += install_inline_hook((void *)pNtQ,     (void *)my_NtQuerySystemTime);

    logline("hook: offsetSec=%lu mask=0x%02x GLT=%p GST=%p GSTAFT=%p NtQ=%p "
            "-> inline-hooked %u function(s) ACTIVE",
            (unsigned long)(g_offset / 10000000), g_mask,
            (void *)pGLT, (void *)pGST, (void *)pGSTAFT, (void *)pNtQ, g_hookCount);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        init_shim();          /* failure just means "run normally" */
    }
    return TRUE;
}

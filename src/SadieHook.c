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

#define SADIEDATE_BUILD "2026-10-14c"

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

/* ========================================================================== *
 *  FILE / REGISTRY TIMESTAMP SHIFTING (32-bit only)
 *
 *  SADiE cross-checks the timestamps of its saved license/settings against the
 *  date it thinks it is. Those files carry the REAL date (the clock keeps
 *  running), so with the shim they look "from the future" and SADiE faults.
 *  We shift the timestamps SADiE READS back by the same offset, so it sees a
 *  consistent past -- while the files on disk keep their real dates.
 *
 *  These functions must call the original to get the real timestamp, so each is
 *  detoured with a small trampoline (copy the first whole instructions, append
 *  a jump back) built with a minimal length decoder. Anything the decoder is
 *  unsure about is simply not hooked (safe). x86 only; x64 skips this.
 * ========================================================================== */
#ifndef _WIN64

static void shift_ft(FILETIME *ft)
{
    if (ft && (ft->dwLowDateTime || ft->dwHighDateTime)) {
        ULONGLONG v = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
        v = (ULONGLONG)((LONGLONG)v - g_offset);
        ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32);
    }
}

/* per-thread guard: some of these functions call each other internally, so only
 * the OUTERMOST call shifts (otherwise a timestamp would be shifted twice). */
static DWORD g_ftls = TLS_OUT_OF_INDEXES;
static int ft_enter(void)
{
    if (g_ftls == TLS_OUT_OF_INDEXES) return 1;
    if (TlsGetValue(g_ftls)) return 0;
    TlsSetValue(g_ftls, (LPVOID)1);
    return 1;
}
static void ft_leave(int outer)
{
    if (outer && g_ftls != TLS_OUT_OF_INDEXES) TlsSetValue(g_ftls, (LPVOID)0);
}

/* ---- minimal x86 length decoder (prologue instructions only) ------------- */
static int modrm_len(const unsigned char *p, int imm)
{
    unsigned char m = p[0];
    int mod = m >> 6, rm = m & 7, len = 1;
    if (mod != 3) {
        if (rm == 4) {                 /* SIB */
            unsigned char sib = p[1]; len++;
            if (mod == 0 && (sib & 7) == 5) len += 4;
            else if (mod == 1) len += 1;
            else if (mod == 2) len += 4;
        } else {
            if (mod == 0 && rm == 5) len += 4;
            else if (mod == 1) len += 1;
            else if (mod == 2) len += 4;
        }
    }
    return len + imm;
}
static int insn_len(const unsigned char *p)
{
    int i = 0;
    unsigned char op;
    for (;;) {                         /* skip prefixes */
        unsigned char b = p[i];
        if (b==0x66||b==0x67||b==0xF0||b==0xF2||b==0xF3||
            b==0x2E||b==0x36||b==0x3E||b==0x26||b==0x64||b==0x65) { i++; continue; }
        break;
    }
    op = p[i++];
    if ((op>=0x50 && op<=0x5F) || op==0x90 || op==0xC3 || op==0xC9) return i;
    if (op>=0xB8 && op<=0xBF) return i + 4;        /* mov reg,imm32 */
    if (op>=0xB0 && op<=0xB7) return i + 1;        /* mov reg8,imm8 */
    if (op==0x6A) return i + 1;                    /* push imm8 */
    if (op==0x68) return i + 4;                    /* push imm32 */
    if (op==0xA1||op==0xA3) return i + 4;          /* mov eax,[moffs32] */
    if (op==0x88||op==0x89||op==0x8A||op==0x8B||op==0x8D||
        op==0x01||op==0x03||op==0x09||op==0x0B||op==0x21||op==0x23||
        op==0x29||op==0x2B||op==0x31||op==0x33||op==0x39||op==0x3B||
        op==0x84||op==0x85||op==0x87||op==0xFF||op==0x8F)
        return i + modrm_len(p + i, 0);
    if (op==0x80||op==0x83||op==0xC6) return i + modrm_len(p + i, 1);
    if (op==0x81||op==0xC7)           return i + modrm_len(p + i, 4);
    return 0;                          /* unknown / not relocatable -> abort */
}

/* ---- trampoline allocator + detour installer ----------------------------- */
static unsigned char *g_tramp = NULL;
static SIZE_T g_tramp_used = 0;
static void *install_detour(void *target, void *stub)
{
    unsigned char *t = (unsigned char *)target, *tr;
    int copied = 0, len;
    DWORD old;
    if (!t) return NULL;
    while (copied < 5) {               /* copy whole instructions >= 5 bytes */
        len = insn_len(t + copied);
        if (len <= 0 || copied + len > 20) return NULL;
        copied += len;
    }
    if (!g_tramp) {
        g_tramp = (unsigned char *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                                                PAGE_EXECUTE_READWRITE);
        g_tramp_used = 0;
    }
    if (!g_tramp || g_tramp_used + (SIZE_T)copied + 5 > 4096) return NULL;
    tr = g_tramp + g_tramp_used;
    g_tramp_used += (copied + 5 + 15) & ~15;

    memcpy(tr, t, copied);             /* original prologue ... */
    tr[copied] = 0xE9;                 /* ... then JMP back to target+copied   */
    *(LONG *)(tr + copied + 1) = (LONG)((t + copied) - (tr + copied + 5));

    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old)) return NULL;
    t[0] = 0xE9;                        /* patch target: JMP stub               */
    *(LONG *)(t + 1) = (LONG)((unsigned char *)stub - (t + 5));
    VirtualProtect(t, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    return tr;                          /* call this to run the original        */
}

/* ---- originals (trampolines) and stubs ----------------------------------- */
typedef BOOL (WINAPI *GetFileTime_t)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME);
typedef BOOL (WINAPI *GetFileAttributesExW_t)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
typedef BOOL (WINAPI *GetFileAttributesExA_t)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
typedef HANDLE (WINAPI *FindFirstFileW_t)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef HANDLE (WINAPI *FindFirstFileA_t)(LPCSTR, LPWIN32_FIND_DATAA);
typedef BOOL (WINAPI *FindNextFileW_t)(HANDLE, LPWIN32_FIND_DATAW);
typedef BOOL (WINAPI *FindNextFileA_t)(HANDLE, LPWIN32_FIND_DATAA);
typedef BOOL (WINAPI *GetFileInformationByHandle_t)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
typedef LONG (WINAPI *RegQueryInfoKeyW_t)(HKEY, LPWSTR, LPDWORD, LPDWORD, LPDWORD,
    LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);
typedef LONG (WINAPI *RegQueryInfoKeyA_t)(HKEY, LPSTR, LPDWORD, LPDWORD, LPDWORD,
    LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, LPDWORD, PFILETIME);

static GetFileTime_t                  o_GetFileTime;
static GetFileAttributesExW_t         o_GetFileAttributesExW;
static GetFileAttributesExA_t         o_GetFileAttributesExA;
static FindFirstFileW_t               o_FindFirstFileW;
static FindFirstFileA_t               o_FindFirstFileA;
static FindNextFileW_t                o_FindNextFileW;
static FindNextFileA_t                o_FindNextFileA;
static GetFileInformationByHandle_t   o_GetFileInformationByHandle;
static RegQueryInfoKeyW_t             o_RegQueryInfoKeyW;
static RegQueryInfoKeyA_t             o_RegQueryInfoKeyA;

static BOOL WINAPI my_GetFileTime(HANDLE h, LPFILETIME c, LPFILETIME a, LPFILETIME w)
{
    int outer = ft_enter();
    BOOL r = o_GetFileTime(h, c, a, w);
    if (r && outer) { shift_ft(c); shift_ft(a); shift_ft(w); }
    ft_leave(outer);
    return r;
}
static BOOL WINAPI my_GetFileAttributesExW(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID p)
{
    int outer = ft_enter();
    BOOL r = o_GetFileAttributesExW(n, l, p);
    if (r && outer && p) {
        WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)p;
        shift_ft(&d->ftCreationTime); shift_ft(&d->ftLastAccessTime); shift_ft(&d->ftLastWriteTime);
    }
    ft_leave(outer);
    return r;
}
static BOOL WINAPI my_GetFileAttributesExA(LPCSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID p)
{
    int outer = ft_enter();
    BOOL r = o_GetFileAttributesExA(n, l, p);
    if (r && outer && p) {
        WIN32_FILE_ATTRIBUTE_DATA *d = (WIN32_FILE_ATTRIBUTE_DATA *)p;
        shift_ft(&d->ftCreationTime); shift_ft(&d->ftLastAccessTime); shift_ft(&d->ftLastWriteTime);
    }
    ft_leave(outer);
    return r;
}
static HANDLE WINAPI my_FindFirstFileW(LPCWSTR n, LPWIN32_FIND_DATAW d)
{
    int outer = ft_enter();
    HANDLE r = o_FindFirstFileW(n, d);
    if (r != INVALID_HANDLE_VALUE && outer && d) {
        shift_ft(&d->ftCreationTime); shift_ft(&d->ftLastAccessTime); shift_ft(&d->ftLastWriteTime);
    }
    ft_leave(outer);
    return r;
}
static HANDLE WINAPI my_FindFirstFileA(LPCSTR n, LPWIN32_FIND_DATAA d)
{
    int outer = ft_enter();
    HANDLE r = o_FindFirstFileA(n, d);
    if (r != INVALID_HANDLE_VALUE && outer && d) {
        shift_ft(&d->ftCreationTime); shift_ft(&d->ftLastAccessTime); shift_ft(&d->ftLastWriteTime);
    }
    ft_leave(outer);
    return r;
}
static BOOL WINAPI my_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW d)
{
    int outer = ft_enter();
    BOOL r = o_FindNextFileW(h, d);
    if (r && outer && d) {
        shift_ft(&d->ftCreationTime); shift_ft(&d->ftLastAccessTime); shift_ft(&d->ftLastWriteTime);
    }
    ft_leave(outer);
    return r;
}
static BOOL WINAPI my_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA d)
{
    int outer = ft_enter();
    BOOL r = o_FindNextFileA(h, d);
    if (r && outer && d) {
        shift_ft(&d->ftCreationTime); shift_ft(&d->ftLastAccessTime); shift_ft(&d->ftLastWriteTime);
    }
    ft_leave(outer);
    return r;
}
static BOOL WINAPI my_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION d)
{
    int outer = ft_enter();
    BOOL r = o_GetFileInformationByHandle(h, d);
    if (r && outer && d) {
        shift_ft(&d->ftCreationTime); shift_ft(&d->ftLastAccessTime); shift_ft(&d->ftLastWriteTime);
    }
    ft_leave(outer);
    return r;
}
static LONG WINAPI my_RegQueryInfoKeyW(HKEY k, LPWSTR cn, LPDWORD ccn, LPDWORD rv,
    LPDWORD sk, LPDWORD mskl, LPDWORD mcl, LPDWORD vc, LPDWORD mvnl, LPDWORD mvdl,
    LPDWORD sd, PFILETIME ft)
{
    int outer = ft_enter();
    LONG r = o_RegQueryInfoKeyW(k, cn, ccn, rv, sk, mskl, mcl, vc, mvnl, mvdl, sd, ft);
    if (r == ERROR_SUCCESS && outer) shift_ft(ft);
    ft_leave(outer);
    return r;
}
static LONG WINAPI my_RegQueryInfoKeyA(HKEY k, LPSTR cn, LPDWORD ccn, LPDWORD rv,
    LPDWORD sk, LPDWORD mskl, LPDWORD mcl, LPDWORD vc, LPDWORD mvnl, LPDWORD mvdl,
    LPDWORD sd, PFILETIME ft)
{
    int outer = ft_enter();
    LONG r = o_RegQueryInfoKeyA(k, cn, ccn, rv, sk, mskl, mcl, vc, mvnl, mvdl, sd, ft);
    if (r == ERROR_SUCCESS && outer) shift_ft(ft);
    ft_leave(outer);
    return r;
}

/* Detour one function; record its trampoline (as the "original"). */
#define DETOUR(mod, name, var) do { \
    FARPROC p = (mod) ? GetProcAddress((mod), #name) : NULL; \
    void *tr = install_detour((void *)p, (void *)my_##name); \
    if (tr) { (var) = tr; g_ftHooks++; } \
} while (0)

static unsigned g_ftHooks = 0;
static void install_filetime_hooks(void)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    if (!adv) adv = LoadLibraryW(L"advapi32.dll");
    g_ftls = TlsAlloc();

    DETOUR(k32, GetFileTime,                  o_GetFileTime);
    DETOUR(k32, GetFileAttributesExW,         o_GetFileAttributesExW);
    DETOUR(k32, GetFileAttributesExA,         o_GetFileAttributesExA);
    DETOUR(k32, FindFirstFileW,               o_FindFirstFileW);
    DETOUR(k32, FindFirstFileA,               o_FindFirstFileA);
    DETOUR(k32, FindNextFileW,                o_FindNextFileW);
    DETOUR(k32, FindNextFileA,                o_FindNextFileA);
    DETOUR(k32, GetFileInformationByHandle,   o_GetFileInformationByHandle);
    DETOUR(adv, RegQueryInfoKeyW,             o_RegQueryInfoKeyW);
    DETOUR(adv, RegQueryInfoKeyA,             o_RegQueryInfoKeyA);
    logline("hook: file/registry timestamp detours installed: %u", g_ftHooks);
}
#endif /* !_WIN64 */

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

#ifndef _WIN64
    /* Also shift the file/registry timestamps the target reads, so its saved
     * data looks consistent with the faked date (default on; ShiftFileTimes=0
     * in the INI turns it off). */
    n = GetEnvironmentVariableA("SADIEDATE_FILETIME", buf, sizeof(buf));
    if (!(n > 0 && n < sizeof(buf) && strtoul(buf, NULL, 10) == 0))
        install_filetime_hooks();
    else
        logline("hook: file/registry timestamp shifting disabled by config");
#endif
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

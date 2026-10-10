/*
 * SadieHook.dll  -  a surgical, date-ONLY time shim.
 *
 * Injected into a target process (e.g. SADiE) by SadieDate.exe. It makes the
 * process -- and every plugin DLL loaded into it -- see a date shifted into the
 * past by a CONSTANT offset, while the real system clock is left completely
 * alone (so files the process writes still get the correct real date, and drive
 * mounts / network auth keep working).
 *
 * WHY THIS DOES NOT ALTER AUDIO
 *   - It hooks ONLY calendar-date functions (SYSTEMTIME / FILETIME based).
 *     It NEVER touches GetTickCount, timeGetTime, QueryPerformanceCounter or
 *     any other monotonic / high-resolution timer. Those drive the audio
 *     engine's buffering and sample timing, and they are left bit-for-bit
 *     untouched.
 *   - The offset is a CONSTANT that still advances at real-time rate, so the
 *     interval between any two time reads is preserved exactly. Even code that
 *     happened to read a date function sees unchanged deltas.
 *   (By contrast, RunAsDate-style tools also offset GetTickCount -- a 32-bit
 *    millisecond counter -- which overflows when shifted by years, feeding the
 *    audio path garbage. That is the likely cause of the A/B mismatch.)
 *
 * HOW IT HOOKS (no code patching)
 *   It swaps function POINTERS in each module's Import Address Table (IAT) --
 *   it never overwrites executable code, so it cannot corrupt instructions or
 *   crash from a bad disassembly. To catch calls that bypass the IAT it also
 *   hooks GetProcAddress (dynamic resolution) and LoadLibrary* (so plugins
 *   loaded later get their IATs patched too).
 *
 * FAIL-SAFE
 *   If the offset is missing/zero, or anything cannot be resolved, the DLL does
 *   nothing and the target runs exactly as normal. It is built to never take
 *   the host process down.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#include <stdlib.h>
#include <stdio.h>

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
static unsigned g_modCount = 0, g_slotCount = 0;   /* diagnostics */
#define SADIEDATE_BUILD "2026-10-10g"

/* Log the first few access violations anywhere in the process, with the faulting
 * address, so we can tell whether a crash is inside this shim or in the host. */
static LONG g_vehCount = 0;
static LONG CALLBACK veh_logger(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        if (InterlockedIncrement(&g_vehCount) <= 6)
            logline("hook: ACCESS VIOLATION at %p (code 0x%08lx)",
                    ep->ExceptionRecord->ExceptionAddress,
                    (unsigned long)ep->ExceptionRecord->ExceptionCode);
    }
    return EXCEPTION_CONTINUE_SEARCH;   /* never interfere with normal handling */
}

/* The constant offset, in 100-ns units, to SUBTRACT from every date read.
 * Positive => the process sees an earlier date. 0 => shim disabled. */
static LONGLONG g_offset = 0;
static HMODULE  g_self = NULL;
static BYTE    *g_k32base = NULL;     /* never patch these two: our stubs call */
static BYTE    *g_ntdllbase = NULL;   /* their real funcs; keep them un-offset */

/* Which date functions to hook (bitmask from SADIEDATE_HOOKS). Lets us fool a
 * license/date check without touching functions a hardware driver's I/O path
 * may rely on (e.g. GetSystemTimeAsFileTime / NtQuerySystemTime). */
#define HOOK_LOCALTIME        0x01   /* GetLocalTime                      */
#define HOOK_SYSTEMTIME       0x02   /* GetSystemTime                     */
#define HOOK_SYSTEMTIME_FT    0x04   /* GetSystemTimeAsFileTime           */
#define HOOK_SYSTEMTIME_PREC  0x08   /* GetSystemTimePreciseAsFileTime    */
#define HOOK_NTQUERY          0x10   /* NtQuerySystemTime                 */
#define HOOK_ALL              0x1F
static unsigned g_mask = HOOK_ALL;

/* Hook GetProcAddress + LoadLibrary* too (covers dynamically-resolved calls and
 * plugins loaded later)? This is the most invasive part; SADIEDATE_DYN=0 turns
 * it off, leaving only the already-loaded modules' date imports patched. */
static int g_dyn = 1;

/* ---- real function pointers (the genuine implementations) ---------------- */
typedef void (WINAPI *GetSystemTime_t)(LPSYSTEMTIME);
typedef void (WINAPI *GetLocalTime_t)(LPSYSTEMTIME);
typedef void (WINAPI *GetSystemTimeAsFileTime_t)(LPFILETIME);
typedef void (WINAPI *GetSystemTimePreciseAsFileTime_t)(LPFILETIME);
typedef NTSTATUS (NTAPI *NtQuerySystemTime_t)(PLARGE_INTEGER);
typedef FARPROC  (WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
typedef HMODULE  (WINAPI *LoadLibraryA_t)(LPCSTR);
typedef HMODULE  (WINAPI *LoadLibraryW_t)(LPCWSTR);
typedef HMODULE  (WINAPI *LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
typedef HMODULE  (WINAPI *LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);

static GetSystemTime_t                    real_GetSystemTime;
static GetLocalTime_t                     real_GetLocalTime;
static GetSystemTimeAsFileTime_t          real_GetSystemTimeAsFileTime;
static GetSystemTimePreciseAsFileTime_t   real_GetSystemTimePreciseAsFileTime;
static NtQuerySystemTime_t                real_NtQuerySystemTime;
static GetProcAddress_t                   real_GetProcAddress;
static LoadLibraryA_t                     real_LoadLibraryA;
static LoadLibraryW_t                     real_LoadLibraryW;
static LoadLibraryExA_t                   real_LoadLibraryExA;
static LoadLibraryExW_t                   real_LoadLibraryExW;

/* ---- offset helpers ------------------------------------------------------ */
static ULONGLONG ft_to_ull(const FILETIME *ft)
{
    return ((ULONGLONG)ft->dwHighDateTime << 32) | (ULONGLONG)ft->dwLowDateTime;
}
static void ull_to_ft(ULONGLONG v, FILETIME *ft)
{
    ft->dwLowDateTime  = (DWORD)(v & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(v >> 32);
}
static void shift_filetime(FILETIME *ft)
{
    ULONGLONG v = ft_to_ull(ft);
    ull_to_ft((ULONGLONG)((LONGLONG)v - g_offset), ft);
}
static void shift_systemtime(LPSYSTEMTIME st)
{
    FILETIME ft;
    if (SystemTimeToFileTime(st, &ft)) {       /* pure field conversion */
        shift_filetime(&ft);
        FileTimeToSystemTime(&ft, st);
    }
}

/* ---- replacement functions (stubs) --------------------------------------- */
static void WINAPI my_GetSystemTime(LPSYSTEMTIME st)
{
    real_GetSystemTime(st);
    if (st) shift_systemtime(st);
}
static void WINAPI my_GetLocalTime(LPSYSTEMTIME st)
{
    real_GetLocalTime(st);
    if (st) shift_systemtime(st);              /* offset is a tz-independent span */
}
static void WINAPI my_GetSystemTimeAsFileTime(LPFILETIME ft)
{
    real_GetSystemTimeAsFileTime(ft);
    if (ft) shift_filetime(ft);
}
static void WINAPI my_GetSystemTimePreciseAsFileTime(LPFILETIME ft)
{
    real_GetSystemTimePreciseAsFileTime(ft);
    if (ft) shift_filetime(ft);
}
static NTSTATUS NTAPI my_NtQuerySystemTime(PLARGE_INTEGER t)
{
    NTSTATUS s = real_NtQuerySystemTime(t);
    if (s == 0 && t) t->QuadPart -= g_offset;
    return s;
}

/* forward decls */
static void patch_module(BYTE *base);

static FARPROC WINAPI my_GetProcAddress(HMODULE mod, LPCSTR name)
{
    FARPROC r = real_GetProcAddress(mod, name);
    /* Only intercept by-name lookups (ordinals have a zero HIWORD). */
    if (name && ((ULONG_PTR)name >> 16) != 0) {
        if ((g_mask & HOOK_SYSTEMTIME) && r == (FARPROC)real_GetSystemTime)
            return (FARPROC)my_GetSystemTime;
        if ((g_mask & HOOK_LOCALTIME) && r == (FARPROC)real_GetLocalTime)
            return (FARPROC)my_GetLocalTime;
        if ((g_mask & HOOK_SYSTEMTIME_FT) && r == (FARPROC)real_GetSystemTimeAsFileTime)
            return (FARPROC)my_GetSystemTimeAsFileTime;
        if ((g_mask & HOOK_SYSTEMTIME_PREC) && real_GetSystemTimePreciseAsFileTime &&
            r == (FARPROC)real_GetSystemTimePreciseAsFileTime)
            return (FARPROC)my_GetSystemTimePreciseAsFileTime;
        if ((g_mask & HOOK_NTQUERY) && real_NtQuerySystemTime &&
            r == (FARPROC)real_NtQuerySystemTime)
            return (FARPROC)my_NtQuerySystemTime;
    }
    return r;
}
static HMODULE WINAPI my_LoadLibraryA(LPCSTR n)
{
    HMODULE h = real_LoadLibraryA(n);
    if (h) patch_module((BYTE *)h);
    return h;
}
static HMODULE WINAPI my_LoadLibraryW(LPCWSTR n)
{
    HMODULE h = real_LoadLibraryW(n);
    if (h) patch_module((BYTE *)h);
    return h;
}
static HMODULE WINAPI my_LoadLibraryExA(LPCSTR n, HANDLE f, DWORD fl)
{
    HMODULE h = real_LoadLibraryExA(n, f, fl);
    if (h) patch_module((BYTE *)h);
    return h;
}
static HMODULE WINAPI my_LoadLibraryExW(LPCWSTR n, HANDLE f, DWORD fl)
{
    HMODULE h = real_LoadLibraryExW(n, f, fl);
    if (h) patch_module((BYTE *)h);
    return h;
}

/* ---- the real->replacement table ----------------------------------------- */
typedef struct { void *real; void *stub; } HookPair;
static HookPair g_pairs[16];
static int      g_npairs = 0;

static void add_pair(void *real, void *stub)
{
    if (real && g_npairs < 16) {
        g_pairs[g_npairs].real = real;
        g_pairs[g_npairs].stub = stub;
        g_npairs++;
    }
}
static void *lookup_stub(void *cur)
{
    int i;
    for (i = 0; i < g_npairs; i++)
        if (g_pairs[i].real == cur)
            return g_pairs[i].stub;
    return NULL;
}

/* ---- IAT patcher --------------------------------------------------------- */
static void patch_slot(void **slot, void *rep)
{
    DWORD old;
    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *slot = rep;
        VirtualProtect(slot, sizeof(void *), old, &old);
        g_slotCount++;
    }
}

static void patch_module(BYTE *base)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD rva;

    if (!base || base == (BYTE *)g_self)        /* never patch ourselves */
        return;
    if (base == g_k32base || base == g_ntdllbase)
        return;   /* leave kernel32/ntdll internals calling the real funcs */

    dos = (IMAGE_DOS_HEADER *)base;
    if (IsBadReadPtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
              .VirtualAddress;
    if (!rva || rva >= nt->OptionalHeader.SizeOfImage)
        return;

    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva);
    for (; !IsBadReadPtr(imp, sizeof(*imp)) && imp->Name; imp++) {
        IMAGE_THUNK_DATA *thunk;
        if (imp->FirstThunk == 0 || imp->FirstThunk >= nt->OptionalHeader.SizeOfImage)
            continue;
        thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; !IsBadReadPtr(thunk, sizeof(*thunk)) && thunk->u1.Function; thunk++) {
            void **slot = (void **)&thunk->u1.Function;
            void  *rep  = lookup_stub(*slot);
            if (rep)
                patch_slot(slot, rep);
        }
    }
}

/* Walk the loader's in-memory module list and patch every module. Reading the
 * PEB loader list is safe to do under the loader lock (no API re-entry). */
static void patch_all_modules(void)
{
    PPEB peb;
    PPEB_LDR_DATA ldr;
    LIST_ENTRY *head, *it;

    /* NtCurrentTeb() reads a segment register (fs:[0x30] / gs:[0x60]). GCC's
     * -Warray-bounds misreads that as a deref of address 0; it is not. */
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    peb = NtCurrentTeb()->ProcessEnvironmentBlock;
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

    if (!peb || !peb->Ldr)
        return;
    ldr  = peb->Ldr;
    head = &ldr->InMemoryOrderModuleList;
    for (it = head->Flink; it && it != head; it = it->Flink) {
        LDR_DATA_TABLE_ENTRY *e =
            CONTAINING_RECORD(it, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (e->DllBase) {
            g_modCount++;
            patch_module((BYTE *)e->DllBase);
        }
    }
}

/* ---- init ---------------------------------------------------------------- */
static BOOL init_shim(void)
{
    HMODULE k32, ntdll;
    char buf[64];
    DWORD n;

    /* Diagnostic log path (optional). Read first so we can log everything. */
    GetEnvironmentVariableA("SADIEDATE_LOG", g_logpath, sizeof(g_logpath));
    logline("hook: DllMain attach, BUILD=%s pid=%lu",
            SADIEDATE_BUILD, (unsigned long)GetCurrentProcessId());
    if (g_logpath[0])
        AddVectoredExceptionHandler(1, veh_logger);   /* only when logging */

    /* Read the offset the launcher passed us. Absent/zero => stay disabled. */
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

    /* Optional: which functions to hook. Absent => hook everything. */
    n = GetEnvironmentVariableA("SADIEDATE_HOOKS", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        unsigned m = (unsigned)strtoul(buf, NULL, 10);
        if (m != 0)
            g_mask = m & HOOK_ALL;
    }
    /* Optional: hook GetProcAddress/LoadLibrary too (default yes). */
    n = GetEnvironmentVariableA("SADIEDATE_DYN", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf))
        g_dyn = (strtoul(buf, NULL, 10) != 0);

    k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32)
        return FALSE;
    ntdll = GetModuleHandleW(L"ntdll.dll");
    g_k32base = (BYTE *)k32;
    g_ntdllbase = (BYTE *)ntdll;

    /* Use the genuine GetProcAddress to resolve the genuine targets. */
    real_GetProcAddress = (GetProcAddress_t)GetProcAddress(k32, "GetProcAddress");
    if (!real_GetProcAddress)
        return FALSE;

    real_GetSystemTime = (GetSystemTime_t)
        real_GetProcAddress(k32, "GetSystemTime");
    real_GetLocalTime = (GetLocalTime_t)
        real_GetProcAddress(k32, "GetLocalTime");
    real_GetSystemTimeAsFileTime = (GetSystemTimeAsFileTime_t)
        real_GetProcAddress(k32, "GetSystemTimeAsFileTime");
    real_GetSystemTimePreciseAsFileTime = (GetSystemTimePreciseAsFileTime_t)
        real_GetProcAddress(k32, "GetSystemTimePreciseAsFileTime"); /* Win8+ */
    real_LoadLibraryA  = (LoadLibraryA_t)  real_GetProcAddress(k32, "LoadLibraryA");
    real_LoadLibraryW  = (LoadLibraryW_t)  real_GetProcAddress(k32, "LoadLibraryW");
    real_LoadLibraryExA = (LoadLibraryExA_t)real_GetProcAddress(k32, "LoadLibraryExA");
    real_LoadLibraryExW = (LoadLibraryExW_t)real_GetProcAddress(k32, "LoadLibraryExW");
    if (ntdll)
        real_NtQuerySystemTime = (NtQuerySystemTime_t)
            real_GetProcAddress(ntdll, "NtQuerySystemTime");

    /* Build the real->stub table (NULLs are skipped by add_pair). Only the
     * date functions enabled by the mask are hooked; GetProcAddress and
     * LoadLibrary* are always hooked so the enabled ones stay fully covered. */
    if (g_mask & HOOK_SYSTEMTIME)
        add_pair((void *)real_GetSystemTime,                  (void *)my_GetSystemTime);
    if (g_mask & HOOK_LOCALTIME)
        add_pair((void *)real_GetLocalTime,                   (void *)my_GetLocalTime);
    if (g_mask & HOOK_SYSTEMTIME_FT)
        add_pair((void *)real_GetSystemTimeAsFileTime,        (void *)my_GetSystemTimeAsFileTime);
    if (g_mask & HOOK_SYSTEMTIME_PREC)
        add_pair((void *)real_GetSystemTimePreciseAsFileTime, (void *)my_GetSystemTimePreciseAsFileTime);
    if (g_mask & HOOK_NTQUERY)
        add_pair((void *)real_NtQuerySystemTime,              (void *)my_NtQuerySystemTime);
    if (g_dyn) {
        add_pair((void *)real_GetProcAddress,             (void *)my_GetProcAddress);
        add_pair((void *)real_LoadLibraryA,               (void *)my_LoadLibraryA);
        add_pair((void *)real_LoadLibraryW,               (void *)my_LoadLibraryW);
        add_pair((void *)real_LoadLibraryExA,             (void *)my_LoadLibraryExA);
        add_pair((void *)real_LoadLibraryExW,             (void *)my_LoadLibraryExW);
    }

    logline("hook: offsetSec=%lu mask=0x%02x dyn=%d GetLocalTime=%p GetSystemTime=%p "
            "GSTAFT=%p NtQuery=%p pairs=%d",
            (unsigned long)(g_offset / 10000000), g_mask, g_dyn,
            (void *)real_GetLocalTime, (void *)real_GetSystemTime,
            (void *)real_GetSystemTimeAsFileTime, (void *)real_NtQuerySystemTime,
            g_npairs);

    patch_all_modules();

    logline("hook: patched %u IAT slot(s) across %u module(s) -> shim ACTIVE",
            g_slotCount, g_modCount);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = (HMODULE)hinst;
        DisableThreadLibraryCalls(hinst);
        init_shim();          /* failure just means "run normally" */
    }
    return TRUE;
}

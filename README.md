# SadieDate

A tiny, **surgical date shim** for legacy software — such as a SADiE
workstation whose time-limited build (and whose plugins) refuse to run on
today's date. You validly own the software; it is obsolete and unsupported, and
the only thing blocking it is an internal date check.

SadieDate makes **only SADiE and its plugins** see an earlier date, continuously,
while the **real Windows clock is left untouched** — so files you save keep the
correct date, drives mount/dismount normally, and network logins don't break on
clock skew. And it does it **without altering your audio**.

---

## What it does (and doesn't)

- ✅ Only SADiE's process (and every plugin loaded into it) sees the past date.
- ✅ The real system clock is **never changed** → saved files get the correct
  real date automatically; no clean-up step, no "fix the dates later".
- ✅ Works for the **whole session**, however long — plugins that re-check the
  date when opened keep passing.
- ✅ Audio comes out **bit-for-bit identical** to a normal run (see below).
- ✅ One double-click. **No administrator rights** required.
- ✅ Covers **32-bit and 64-bit** SADiE on **Windows XP, 7 and 10**.

---

## Why this does NOT alter your audio

Tools like RunAsDate hook a **broad** set of time functions — including
monotonic timers such as `GetTickCount`. `GetTickCount` is a 32-bit millisecond
counter; shifting it back by ~18 years overflows it and feeds the audio engine
garbage. That is the likely reason your processed files stopped A/B-matching.

SadieDate is deliberately **narrow**:

1. **Date functions only.** It intercepts just the calendar-date calls
   (`GetLocalTime`, `GetSystemTime`, `GetSystemTimeAsFileTime`,
   `GetSystemTimePreciseAsFileTime`, `NtQuerySystemTime`). It **never** touches
   `GetTickCount`, `timeGetTime`, or `QueryPerformanceCounter` — the timers the
   audio engine actually uses. They pass through completely unmodified.
2. **Constant offset.** It subtracts a fixed amount that still advances at
   real-time rate, so the **interval** between any two time reads is preserved
   exactly. Even code that reads a date sees unchanged deltas.

**It swaps function pointers in the Import Address Table — it never rewrites
machine code**, so it can't corrupt instructions. And it's fail-safe: if
anything can't be set up, SADiE simply runs as normal.

> ⚠️ Because this is in-process hooking (the same *category* as RunAsDate, but
> stripped to the minimum), **prove it on your rig before trusting it.** See
> "Verify" below. Test on a non-critical machine first.

---

## What's in the box

| File               | What it is                                             |
|--------------------|--------------------------------------------------------|
| `SadieDate.exe`    | The launcher (32-bit). Run this one.                   |
| `SadieDate64.exe`  | 64-bit launcher. `SadieDate.exe` hands off to it automatically for 64-bit SADiE. |
| `SadieHook32.dll`  | The date shim injected into 32-bit programs.           |
| `SadieHook64.dll`  | The date shim injected into 64-bit programs.           |
| `ShowDate.exe` / `ShowDate64.exe` | Harmless test tool to confirm the shim works. |
| `SadieDate.ini`    | Your configuration.                                    |

**Keep all of these in the same folder.** `SadieDate.exe` finds the DLLs and the
64-bit launcher next to itself.

---

## Quick start

1. Put every file above in one folder.
2. Edit `SadieDate.ini`:
   - `TargetDate=2008-01-01` (a date before the build's/plugins' expiry)
   - `Program=` full path to SADiE's `.exe`
3. Double-click **`SadieDate.exe`**. SADiE starts seeing the past date; your
   Windows clock stays correct.

### Command line

```
SadieDate.exe "2008-01-01" "C:\Program Files\SADiE\SADiE.exe"
SadieDate.exe "2008-01-01 09:00:00" "C:\Program Files\SADiE\SADiE.exe"
```

You can always run `SadieDate.exe` (the 32-bit one); it auto-detects a 64-bit
target and hands off to `SadieDate64.exe`.

---

## Verify (do this first)

**1 — Confirm the spoof, with zero risk to SADiE:**

```
SadieDate.exe "2008-01-01" ShowDate.exe
```

`ShowDate` should print **2008** on every line, while the **Windows taskbar
clock still shows today**. That proves the date is faked for that one process
only and the system clock is untouched. The seconds tick up normally → the fake
clock advances at the real rate. (For 64-bit, use `ShowDate64.exe`.)

**2 — Confirm the audio, before you rely on it:**

Process one file through SADiE under SadieDate, then null-test (A/B /
phase-invert) it against the same file processed normally. It should null
completely. Only trust the tool for real work once you've seen that null.

---

## Building from source

Prebuilt binaries are in `dist/`. To rebuild:

### MinGW-w64 (e.g. on Linux) — builds everything, both bitnesses

```bash
sudo apt-get install gcc-mingw-w64        # Debian/Ubuntu
./build-mingw.sh                          # -> dist/
```

### Microsoft Visual C++ (on Windows)

See `build-msvc.bat`. Build the DLL and the launcher for each target
architecture (x86 and x64) from the matching *Developer Command Prompt*.

---

## How it works (for the curious)

1. `SadieDate.exe` reads `TargetDate`, computes a constant offset
   `real_now − target`, and passes it to the shim via an environment variable.
2. It launches SADiE **suspended**, injects the matching-bitness
   `SadieHook*.dll` (classic `CreateRemoteThread` + `LoadLibrary`), then resumes
   it — so the shim is active before SADiE runs a single instruction.
3. On load, the shim resolves the real date functions and rewrites every loaded
   module's Import Address Table entry for them to point at its own versions,
   which call the real function and subtract the offset. It also hooks
   `GetProcAddress` (for dynamically-resolved calls) and `LoadLibrary*` (so
   plugins loaded later are covered too). It deliberately skips kernel32/ntdll
   internals, so the offset is applied exactly once.
4. The launcher exits. The shim lives inside SADiE for as long as it runs. The
   real clock was never touched, so there's nothing to restore.

---

## Notes & limitations

- The faked date is pinned at launch and drifts forward at the real rate. Over a
  multi-week session it stays ~18 years behind — pick a `TargetDate` far enough
  before expiry.
- Covers date reads via the Win32/NT calendar APIs and the C runtime (which uses
  them underneath). A program that reads the date by some exotic private route
  (e.g. a kernel driver, or a network time query) is outside its scope — the
  `ShowDate` test will reveal this immediately if so.
- In-process hooking can trip aggressive anti-virus heuristics. Whitelist the
  folder if needed.
- This tool only changes the **date the program sees**. It does not patch or
  bypass any copy-protection in the software itself.

---

## Alternative: whole-clock rollback (no injection)

If you ever want a version that doesn't touch SADiE's process **at all** (zero
audio risk, guaranteed), see [`alt-clock-rollback/`](alt-clock-rollback/). It
rolls the whole PC clock back while SADiE runs and restores it after — but the
system date is wrong during the session, which can break network-drive mounts.
The main shim above is the better fit for keeping the real clock.

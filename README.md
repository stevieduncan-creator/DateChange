# SadieDate

A tiny, **non-invasive** date launcher for legacy software such as a SADiE
workstation whose time-limited build refuses to start on today's date.

You validly own the software; it is obsolete and unsupported, and the only
thing stopping it from running is an internal date check. SadieDate lets the
program see an earlier date **without altering anything it produces**.

---

## Why it won't change your audio

Tools like NirSoft **RunAsDate** work by *injecting a DLL into the program and
hooking its time functions*. That injected code runs **inside** the program
while it works — and in a real-time audio application that can perturb thread
timing and buffering just enough that processed files no longer A/B against the
original. That is almost certainly why your files stopped matching.

**SadieDate never injects anything and never hooks anything.** It simply:

1. rolls the whole Windows clock back to your target date,
2. launches SADiE **completely unmodified**, and
3. puts the clock back when SADiE closes.

Because SADiE runs exactly as it would if you had changed the date by hand, its
audio engine behaves identically and its output is **bit-for-bit the same**.

---

## Keeping the correct date on files you save

There is one trade-off to the approach above: while SADiE is open, the entire
PC clock reads the back-date, so any file SADiE writes is *stamped* with the
back-date (this is date **metadata** only — the audio samples inside are
untouched).

SadieDate fixes this for you. Pick whichever of the two methods fits your build:

### Method A — Automatic date correction (default)

When SADiE closes, SadieDate looks through the folders you list in `WatchDirs`
and corrects the dates of the files that were written during the session,
setting each to the **real moment it was written**. It only changes date
metadata on already-closed files; it never opens or rewrites the audio.

Set your output folders in `SadieDate.ini`:

```
WatchDirs=D:\SADiE Projects;D:\Audio Captures
```

> Tip: choose a `TargetDate` you never used for real projects (e.g.
> `2008-01-01`). The correction only touches files whose timestamp falls in the
> short window the session actually ran, so an unusual target date guarantees it
> never disturbs older files that happen to live in the same folder.

### Method B — Early restore (if the date is only checked at startup)

Many old time-limited builds only check the date **when they start**. If that is
true for your SADiE, set:

```
RestoreAfterSeconds=30
```

SadieDate will roll the clock back, launch SADiE, wait ~30 seconds for it to
load, then **restore the real clock while SADiE keeps running**. Everything you
save afterwards gets the correct real date automatically, with no correction
step at all.

Leave this at `0` if SADiE re-checks the date while it is open (it would
complain when the clock jumps forward). If in doubt, start with `0` (Method A).

---

## Supported Windows versions

A single **32-bit** executable runs on:

| OS           | 32-bit | 64-bit |
|--------------|:------:|:------:|
| Windows XP   |   ✓    |   ✓    |
| Windows 7    |   ✓    |   ✓    |
| Windows 10   |   ✓    |   ✓    |

(32-bit Windows programs run natively on 64-bit Windows via WOW64, so one file
covers every case.)

---

## Quick start

1. Put `SadieDate.exe` and `SadieDate.ini` **in the same folder**.
2. Open `SadieDate.ini` in Notepad and set:
   - `TargetDate` — a date before your build's expiry (e.g. `2008-01-01`)
   - `Program`    — the full path to SADiE's `.exe`
   - `WatchDirs`  — the folder(s) where SADiE saves your audio/projects
3. **Right-click `SadieDate.exe` → Run as administrator.**
   (Changing the clock needs admin rights. On Windows XP, just use an
   administrator account — there is no prompt.)
4. SADiE starts with the back-date. Work normally.
5. Close SADiE. SadieDate restores the real clock and corrects your file dates.

Keep the small SadieDate window open while you work; it is what restores the
clock. If you close *it*, the clock is still restored safely.

### Command-line alternative

```
SadieDate.exe "2008-01-01" "C:\Program Files\SADiE\SADiE.exe"
SadieDate.exe "2008-01-01 09:00:00" "C:\Program Files\SADiE\SADiE.exe"
```

Arguments override the INI; anything after the program path is passed to it.

---

## Building from source

The prebuilt binary lives in `dist/SadieDate.exe`. To rebuild it yourself:

### With MinGW-w64 (e.g. on Linux)

```bash
sudo apt-get install gcc-mingw-w64-i686   # Debian/Ubuntu
./build-mingw.sh                          # -> build/SadieDate.exe
```

### With Microsoft Visual C++ (on Windows)

Open a *Developer Command Prompt for VS* and run:

```bat
build-msvc.bat
```

To target Windows XP with a modern Visual Studio, use an `_xp` toolset or keep
the `/SUBSYSTEM:CONSOLE,5.01` switch already in the batch file.

---

## Files

| File                  | Purpose                                            |
|-----------------------|----------------------------------------------------|
| `src/SadieDate.c`     | The launcher (single source file).                 |
| `src/SadieDate.manifest` | Requests admin elevation; declares OS support.  |
| `src/SadieDate.rc`    | Embeds the manifest + version info.                |
| `SadieDate.ini`       | Your configuration (edit this).                    |
| `build-mingw.sh`      | Cross-compile with MinGW-w64.                       |
| `build-msvc.bat`      | Compile with Visual C++.                            |
| `dist/SadieDate.exe`  | Prebuilt 32-bit executable.                         |

---

## Notes & limitations

- **Admin rights are required** — setting the system clock is a privileged
  operation.
- The clock is restored when SADiE exits, or if you close the SadieDate window,
  log off, or shut down while it is running.
- If the machine loses power while SADiE is open, the clock will still read the
  back-date on next boot. Correct it with *Date & Time* settings, or
  `w32tm /resync` if the Windows Time service and a time source are available.
- Automatic date correction (`WatchDirs`) covers the folders you list,
  recursively. Make sure every place SADiE writes the files you care about is
  listed.
- Very long file paths (beyond the classic 260-character limit) are skipped by
  the correction pass.
- This tool only changes the **date the computer reports**. It does not modify,
  patch, or bypass any copy-protection in the software itself.

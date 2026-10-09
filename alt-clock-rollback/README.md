# Alternative: whole-clock rollback (no injection)

This is the **fallback** approach. Most users should use the main date-only
shim in the repository root instead — it keeps the real clock, so file dates and
network-drive mounts are unaffected.

Use this version only if you want **zero** audio risk by guaranteeing SADiE's
process is never touched. It:

1. rolls the **entire** Windows clock back to `TargetDate`,
2. launches SADiE completely unmodified,
3. restores the real clock when SADiE closes (and corrects the dates of files
   written during the session, if you set `WatchDirs`).

Trade-off: while SADiE is open, the whole PC clock reads the past. That breaks
anything time-sensitive during the session — notably network-drive mounts /
domain logins — and it needs administrator rights to change the clock. It also
doesn't suit a workflow where SADiE is kept open for days or drives come and go.

Build it with `./build-mingw.sh` in this folder (produces `SadieDate-clock.exe`)
or `build-msvc.bat` on Windows. See the comments in `SadieDate.c` and the root
`SadieDate.ini` keys `WatchDirs`, `FixFileDates`, `RestoreAfterSeconds`.

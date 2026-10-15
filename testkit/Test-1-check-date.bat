@echo off
title TEST 1 - date check (SadieDate build 2026-10-15a)
echo ============================================================
echo   TEST 1 of the date shim  (build 2026-10-15a)
echo ------------------------------------------------------------
echo   This runs a tiny, harmless test program (ShowDate) under
echo   the shim.  It does NOT touch SADiE.
echo.
echo   Every date it prints below should read  2017-10-12
echo   (NOT 2026).  The seconds should tick up normally.
echo   Meanwhile your Windows taskbar clock stays on today.
echo ============================================================
echo.
REM Wait for ShowDate to finish so all its output stays in this window.
set SADIEDATE_WAIT=1
"%~dp0SadieDate.exe" "2017-10-12" "%~dp0ShowDate.exe"
echo.
echo ============================================================
echo   RESULT:
echo     * Dates above say 2017  -> the shim WORKS on this PC.
echo     * Dates above say 2026  -> the shim did NOT take effect.
echo   Please screenshot this window and send it to Claude.
echo ============================================================
echo.
pause

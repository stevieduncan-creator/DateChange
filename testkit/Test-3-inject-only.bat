@echo off
title TEST 3 - injection-only (build 2026-10-15a)
echo ============================================================
echo   TEST 3  (build 2026-10-15a)   -- only run this if TEST 2 crashed
echo ------------------------------------------------------------
echo   This loads the shim into SADiE but does NOT change the date
echo   (SADiE sees the REAL date, today).  It tells us whether the
echo   crash is caused by loading our shim at all, or by the past
echo   date itself:
echo.
echo     * SADiE CRASHES here too  -> the problem is loading the
echo       shim into SADiE (Claude will fix the injection).
echo     * SADiE RUNS fine here     -> the shim is harmless; the
echo       past DATE is what SADiE dislikes (Claude will change
echo       the date strategy).
echo ============================================================
echo.
call "%~dp0_find-sadie.bat"
if not defined SADIE (
    echo ERROR: Could not read the SADiE path from SadieDate.ini.
    echo Open SadieDate.ini and set the Program= line to your Sadie5.exe.
    pause
    goto :eof
)
echo SADiE path from SadieDate.ini:
echo   %SADIE%
echo.
set "SADIEDATE_LOG=%~dp0SadieDate-log.txt"
set SADIEDATE_FILETIME=0
"%~dp0SadieDate.exe" "now" "%SADIE%"
echo.
echo ------------------------------------------------------------
echo   Tell Claude: did SADiE RUN or CRASH in injection-only mode?
echo   (This is the single most useful answer right now.)
echo ------------------------------------------------------------
echo.
pause

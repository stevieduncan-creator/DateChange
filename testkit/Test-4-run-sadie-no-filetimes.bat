@echo off
title TEST 4 - SADiE, date only (build 2026-10-15a)
echo ============================================================
echo   TEST 4  (build 2026-10-15a)   -- only run this if TEST 2 crashed
echo ------------------------------------------------------------
echo   Same as TEST 2 (SADiE sees 2017-10-12) but with the
echo   file/registry timestamp shifting turned OFF.  This tells us
echo   whether that extra feature helps or hurts:
echo.
echo     * Compare the result with TEST 2.  If TEST 4 behaves
echo       differently, the timestamp shifting is involved.
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
"%~dp0SadieDate.exe" "2017-10-12" "%SADIE%"
echo.
echo ------------------------------------------------------------
echo   Tell Claude: did SADiE RUN or CRASH, and did it differ
echo   from TEST 2?  Screenshot the debug popup and any crash.
echo ------------------------------------------------------------
echo.
pause

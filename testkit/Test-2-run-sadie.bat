@echo off
title TEST 2 - run SADiE with the shim (build 2026-10-15a)
echo ============================================================
echo   TEST 2  (build 2026-10-15a)
echo ------------------------------------------------------------
echo   Launches SADiE so it sees 2017-10-12, with EVERYTHING on
echo   (date spoof + file/registry timestamp shifting).
echo   Your real clock stays on today.
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
REM Turn on the debug popup (it reports the build + the date SADiE will see).
set "SADIEDATE_LOG=%~dp0SadieDate-log.txt"
set SADIEDATE_FILETIME=1
"%~dp0SadieDate.exe" "2017-10-12" "%SADIE%"
echo.
echo ------------------------------------------------------------
echo   A "SadieDate - debug result" popup should have appeared.
echo   Please screenshot it (it shows the build + date SADiE sees).
echo   Then tell Claude whether SADiE ran or crashed.
echo   If it crashed, also screenshot any "crash location" popup
echo   and the Event Viewer entry (eventvwr - Application).
echo ------------------------------------------------------------
echo.
pause

@echo off
REM Shared helper: read the SADiE path (Program=) from SadieDate.ini into %SADIE%.
REM You only ever edit the path in ONE place: SadieDate.ini.
set "SADIE="
for /f "tokens=1* delims==" %%A in ('findstr /b /i "Program=" "%~dp0SadieDate.ini"') do set "SADIE=%%B"
goto :eof

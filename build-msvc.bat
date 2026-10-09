@echo off
REM ============================================================
REM  Build the date-only shim with Microsoft Visual C++.
REM
REM  Run it TWICE:
REM    - from the "x86 Native Tools Command Prompt"  -> 32-bit set
REM    - from the "x64 Native Tools Command Prompt"  -> 64-bit set
REM  Each run detects its architecture from the VS environment and
REM  names the outputs accordingly (…32 / …64).
REM
REM  For Windows XP support with newer Visual Studio, build with an
REM  "_xp" toolset, or add  /SUBSYSTEM:<CONSOLE|WINDOWS>,5.01 .
REM ============================================================
setlocal
set SRC=%~dp0src
set OUT=%~dp0build
set DIST=%~dp0dist
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%DIST%" mkdir "%DIST%"

REM Detect target architecture from the VS dev-prompt environment.
set ARCH=%VSCMD_ARG_TGT_ARCH%
if /I "%ARCH%"=="x64" ( set BIT=64 ) else ( set BIT=32 )
echo Building the %BIT%-bit set...

echo   Hook DLL -> SadieHook%BIT%.dll
cl /nologo /O2 /LD /DWIN32_LEAN_AND_MEAN ^
   /Fe"%OUT%\SadieHook%BIT%.dll" /Fo"%OUT%\\" ^
   "%SRC%\SadieHook.c" /link kernel32.lib
if errorlevel 1 goto :fail

REM The 32-bit launcher is SadieDate.exe; the 64-bit one is SadieDate64.exe.
if "%BIT%"=="64" ( set LAUNCHER=SadieDate64.exe ) else ( set LAUNCHER=SadieDate.exe )
echo   Launcher -> %LAUNCHER%
rc /nologo /fo "%OUT%\SadieInject.res" "%SRC%\SadieInject.rc"
if errorlevel 1 goto :fail
cl /nologo /O2 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   /Fe"%OUT%\%LAUNCHER%" /Fo"%OUT%\\" ^
   "%SRC%\SadieInject.c" ^
   /link "%OUT%\SadieInject.res" shell32.lib user32.lib kernel32.lib
if errorlevel 1 goto :fail

if "%BIT%"=="64" ( set SD=ShowDate64.exe ) else ( set SD=ShowDate.exe )
echo   Test tool -> %SD%
cl /nologo /O2 /Fe"%OUT%\%SD%" /Fo"%OUT%\\" "%SRC%\ShowDate.c"
if errorlevel 1 goto :fail

copy /Y "%OUT%\SadieHook%BIT%.dll" "%DIST%\" >nul
copy /Y "%OUT%\%LAUNCHER%"         "%DIST%\" >nul
copy /Y "%OUT%\%SD%"               "%DIST%\" >nul
echo.
echo Built the %BIT%-bit set into dist\. Run again from the other prompt for the other bitness.
goto :eof

:fail
echo.
echo BUILD FAILED.
exit /b 1

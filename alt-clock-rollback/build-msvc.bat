@echo off
REM ============================================================
REM  Build SadieDate.exe with Microsoft Visual C++ (cl.exe).
REM  Run this from a "Developer Command Prompt for VS".
REM
REM  Produces a 32-bit executable. To target Windows XP with
REM  newer Visual Studio, use the "_xp" toolset (v141_xp etc.)
REM  or add  /SUBSYSTEM:CONSOLE,5.01  to the linker line below.
REM ============================================================
setlocal
set SRC=%~dp0src
set OUT=%~dp0build
if not exist "%OUT%" mkdir "%OUT%"

echo Compiling resources...
rc /nologo /fo "%OUT%\SadieDate.res" "%SRC%\SadieDate.rc"
if errorlevel 1 goto :fail

echo Compiling and linking...
cl /nologo /O2 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   /Fe"%OUT%\SadieDate.exe" /Fo"%OUT%\\" ^
   "%SRC%\SadieDate.c" ^
   /link "%OUT%\SadieDate.res" ^
   /SUBSYSTEM:CONSOLE,5.01 ^
   user32.lib advapi32.lib kernel32.lib
if errorlevel 1 goto :fail

echo.
echo Built: %OUT%\SadieDate.exe
goto :eof

:fail
echo.
echo BUILD FAILED.
exit /b 1

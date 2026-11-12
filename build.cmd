@echo off
rem Builds and tests Superlite Autoshut with the MSVC Build Tools.
rem   build.cmd        run the tests, then build dist\SuperliteAutoshut.exe
rem                    and the safe test build dist\SuperliteAutoshut-test.exe
rem   build.cmd test   run the tests only
setlocal
cd /d "%~dp0"

if defined VSCMD_VER goto :haveenv
rem %%VSWHERE%% is expanded by the inner cmd, so the ")" in "(x86)" can't end the for /f.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%%VSWHERE%%" -products * -latest -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio Build Tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
:haveenv

if not exist obj mkdir obj
if not exist dist mkdir dist

echo == tests
cl /nologo /W4 /WX /Fo:obj\ /Fe:obj\test_logic.exe tests\test_logic.c src\logic.c || exit /b 1
obj\test_logic.exe || exit /b 1
if /i "%1"=="test" exit /b 0

rem No C runtime: the exe only needs kernel32/user32/gdi32/advapi32/shell32/wtsapi32.
set CFLAGS=/nologo /W4 /WX /O1 /Os /GS- /Gs999999 /Zl /DUNICODE /D_UNICODE
set LFLAGS=/link /NODEFAULTLIB /ENTRY:entry /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF /MANIFEST:EMBED /MANIFESTUAC:level='asInvoker' ^
 kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib powrprof.lib

rc /nologo /fo obj\app.res src\app.rc || exit /b 1

echo == SuperliteAutoshut.exe
cl %CFLAGS% /Fo:obj\ /Fe:dist\SuperliteAutoshut.exe src\main.c src\logic.c obj\app.res %LFLAGS% || exit /b 1

echo == SuperliteAutoshut-test.exe (never shuts down, logs instead)
if not exist obj\t mkdir obj\t
cl %CFLAGS% /DAS_TEST_BUILD /Fo:obj\t\ /Fe:dist\SuperliteAutoshut-test.exe src\main.c src\logic.c obj\app.res %LFLAGS% || exit /b 1

for %%f in (dist\*.exe) do echo %%~nxf %%~zf bytes

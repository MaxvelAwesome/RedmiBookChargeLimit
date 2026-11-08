@echo off
rem Сборка RedmiChargeLimit.exe (MSVC, статический CRT, без внешних зависимостей).
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist build mkdir build
if not exist res\app.ico (
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_icon.ps1 || exit /b 1
)

rc /nologo /fo build\app.res res\app.rc || exit /b 1

set CFLAGS=/nologo /std:c++20 /utf-8 /EHsc /MT /O1 /Os /GL /Gy /Gw /GS /sdl /guard:cf /W4 /permissive- ^
 /DUNICODE /D_UNICODE /DNDEBUG /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00
set LIBS=d2d1.lib dwrite.lib dwmapi.lib comctl32.lib shell32.lib setupapi.lib wbemuuid.lib ole32.lib ^
 oleaut32.lib userenv.lib wtsapi32.lib advapi32.lib user32.lib

cl %CFLAGS% /Fobuild\ src\*.cpp build\app.res /Fe:build\RedmiChargeLimit.exe ^
 /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:NO /DYNAMICBASE /NXCOMPAT /CETCOMPAT /guard:cf ^
 /INCREMENTAL:NO %LIBS% || exit /b 1

for %%f in (build\RedmiChargeLimit.exe) do echo Built %%~ff - %%~zf bytes
exit /b 0

:novs
echo MSVC not found: install Visual Studio 2022 Build Tools with C++ workload.
exit /b 1

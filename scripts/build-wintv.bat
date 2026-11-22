@echo off
rem Builds wintv-capture.exe (Hauppauge WinTV USB tuner recorder) with the Visual Studio C++ tools.
rem Run it from a "x64 Native Tools Command Prompt", or let it find Visual Studio itself.
setlocal
cd /d "%~dp0\.."
where cl >nul 2>nul
if not errorlevel 1 goto build
set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" goto novs
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:build
if not exist build mkdir build
cl /nologo /O2 /EHsc /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES /Isrc src\wintv.cpp /Fo:build\ /Fe:build\wintv-capture.exe || exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES /Isrc tests\test_cc608.c /Fo:build\ /Fe:build\test_cc608.exe || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES /Isrc tests\test_tv.cpp /Fo:build\ /Fe:build\test_tv.exe || exit /b 1
build\test_tv.exe || exit /b 1
build\test_cc608.exe || exit /b 1
echo Built build\wintv-capture.exe. Put ffmpeg.exe next to it, or set WINTV_FFMPEG.
exit /b 0
:novs
echo Visual Studio C++ tools not found. Install "Desktop development with C++" or use a Native Tools prompt.
exit /b 1

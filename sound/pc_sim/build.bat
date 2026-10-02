@echo off
rem Builds pc_sim.exe with MSVC from the same synth/sequencer/score sources as the firmware,
rem then runs it to produce the WAV files in this folder.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR echo MSVC not found & exit /b 1
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
if not exist build mkdir build
cl /nologo /O2 /W3 /std:c11 /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /I..\main ^
   pc_sim.c ..\main\piano_synth.c ..\main\sequencer.c ..\main\imperial_march.c ^
   /Fobuild\ /Febuild\pc_sim.exe || exit /b 1

build\pc_sim.exe %*
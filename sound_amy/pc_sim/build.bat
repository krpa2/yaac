@echo off
rem Builds pc_sim.exe with MSVC from the same AMY, player and song sources as the
rem firmware, then renders the songs and every ..\assets\*.mid to WAV files in this folder.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul && goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR echo MSVC not found & exit /b 1
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
set AMY=..\components\amy\src
if not exist build mkdir build
cl /nologo /O2 /W1 /std:c11 /D_CRT_SECURE_NO_WARNINGS /D_USE_MATH_DEFINES ^
   /I%AMY% /I..\main /I..\..\sound\main ^
   pc_sim.c ..\main\amy_player.c ..\main\midi_file.c ..\main\midi_player.c ..\main\song_korobeiniki.c ..\main\song_imperial.c ^
   ..\..\sound\main\imperial_march.c ^
   %AMY%\algorithms.c %AMY%\amy.c %AMY%\envelope.c %AMY%\examples.c %AMY%\parse.c ^
   %AMY%\filters.c %AMY%\oscillators.c %AMY%\pcm.c %AMY%\interp_partials.c %AMY%\custom.c ^
   %AMY%\delay.c %AMY%\log2_exp2.c %AMY%\patches.c %AMY%\transfer.c %AMY%\sequencer.c ^
   %AMY%\libminiaudio-audio.c %AMY%\cv_trigger.c %AMY%\instrument.c %AMY%\note_output.c ^
   %AMY%\amy_midi.c %AMY%\api.c %AMY%\midi_mappings.c ^
   /Fobuild\ /Febuild\pc_sim.exe || exit /b 1

build\pc_sim.exe || exit /b 1
for %%f in (..\assets\*.mid) do build\pc_sim.exe "%%f" || exit /b 1

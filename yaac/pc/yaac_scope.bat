@echo off
rem Start the YAAC voice stream oscilloscope (receives the board's UDP multicast audio).
rem Extra arguments are passed through, e.g.:  yaac_scope.bat --save recordings --window 4
cd /d "%~dp0"

where py >nul 2>nul && (py -3 yaac_scope.py %* & goto :done)
where python >nul 2>nul && (python yaac_scope.py %* & goto :done)
echo Python 3 was not found. Install it from https://www.python.org/ (with tcl/tk).
pause
exit /b 1

:done
if errorlevel 1 pause
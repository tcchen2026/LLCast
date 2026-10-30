@echo off
rem Generated from tools/windows in the LLStream repository; deploy with tools/deploy_windows.py.
setlocal
cd /d "%~dp0"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%i
set OUT=logs\%STAMP%-stream
mkdir "%OUT%" 2>nul
copy /y VERSION.txt "%OUT%\" >nul
echo Logging to %OUT%
rem Streams until Ctrl-C: stream.bat BOX_IP
rem The box receiver must already be running. For latency photos, run clock.bat as well.
if "%~1"=="" (echo Usage: stream.bat BOX_IP & exit /b 2)
echo Streaming until Ctrl-C. There is no time limit.
bin\llstream_win.exe --host %1 > "%OUT%\run.log" 2>&1
set "RESULT=%ERRORLEVEL%"
echo Exit code: %RESULT%>> "%OUT%\run.log"
type "%OUT%\run.log"
exit /b %RESULT%

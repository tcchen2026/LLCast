@echo off
rem Generated from tools/windows in the LLStream repository; deploy with tools/deploy_windows.py.
setlocal
set PROFILE_HOST=192.168.185.100
cd /d "%~dp0"
echo Profile collector: python tools/profile.py --listen %PROFILE_HOST%:47801 --save build/results/pc-profile
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%i
set OUT=logs\%STAMP%-replay
mkdir "%OUT%" 2>nul
copy /y VERSION.txt "%OUT%\" >nul
echo Logging to %OUT%
rem Replays the 1080p60 test clip to the box: replay.bat BOX_IP
rem The box receiver (llstream_rx) must already be running.
if "%~1"=="" (echo Usage: replay.bat BOX_IP & exit /b 2)
bin\llstream_send.exe %1 clips\testsrc2-1920x1080-60fps-180frames-idr.hevc --repeat 10 --profile-to %PROFILE_HOST%:47801 > "%OUT%\run.log" 2>&1
type "%OUT%\run.log"

@echo off
rem Generated from tools/windows in the LLStream repository; deploy with tools/deploy_windows.py.
setlocal
set PROFILE_HOST=192.168.185.100
cd /d "%~dp0"
echo Profile collector: python tools/profile.py --listen %PROFILE_HOST%:47801 --save build/results/pc-profile
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%i
set OUT=logs\%STAMP%-probe
mkdir "%OUT%" 2>nul
copy /y VERSION.txt "%OUT%\" >nul
echo Logging to %OUT%
rem Network probe: synthetic 60 fps frames at a given rate to the box.
rem probe.bat BOX_IP MBPS   e.g. probe.bat 192.168.5.119 24
rem The box receiver must be running with --no-decode.
if "%~2"=="" (echo Usage: probe.bat BOX_IP MBPS & exit /b 2)
set HOST=%~1
set MBPS=%~2
bin\llstream_send.exe %HOST% --synthetic-mbps %MBPS% --seconds 30 --profile-to %PROFILE_HOST%:47801 > "%OUT%\run.log" 2>&1
type "%OUT%\run.log"

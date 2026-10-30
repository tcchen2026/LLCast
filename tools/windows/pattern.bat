@echo off
rem Generated from tools/windows in the LLStream repository; deploy with tools/deploy_windows.py.
setlocal
set PROFILE_HOST=192.168.185.100
cd /d "%~dp0"
echo Profile collector: python tools/profile.py --listen %PROFILE_HOST%:47801 --save build/results/pc-profile
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%i
set OUT=logs\%STAMP%-pattern
mkdir "%OUT%" 2>nul
copy /y VERSION.txt "%OUT%\" >nul
echo Logging to %OUT%
rem Encodes a known test pattern (no capture, no network) to check GPU colour conversion.
bin\llstream_win.exe --pattern --no-scale --output "%OUT%\pattern.hevc" --seconds 2 --profile-to %PROFILE_HOST%:47801 > "%OUT%\run.log" 2>&1
type "%OUT%\run.log"

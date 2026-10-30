@echo off
rem Generated from tools/windows in the LLStream repository; deploy with tools/deploy_windows.py.
setlocal
set PROFILE_HOST=192.168.185.100
cd /d "%~dp0"
echo Profile collector: python tools/profile.py --listen %PROFILE_HOST%:47801 --save build/results/pc-profile
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%i
set OUT=logs\%STAMP%-capture
mkdir "%OUT%" 2>nul
copy /y VERSION.txt "%OUT%\" >nul
echo Logging to %OUT%
rem Local capture test, no network: the clock provides moving content, then
rem 10 s of the whole main screen is fitted to 1920x1080, converted and
rem encoded to capture.hevc with per-stage timings. Do not touch the PC meanwhile.
start "LLStream clock" /min cmd /c "bin\llstream_clock.exe --seconds 14 > "%OUT%\clock.log" 2>&1"
timeout /t 2 /nobreak >nul
bin\llstream_win.exe --output "%OUT%\capture.hevc" --seconds 10 --profile-to %PROFILE_HOST%:47801 > "%OUT%\run.log" 2>&1
type "%OUT%\run.log"

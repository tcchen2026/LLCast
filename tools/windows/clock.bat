@echo off
rem Generated from tools/windows in the LLStream repository; deploy with tools/deploy_windows.py.
setlocal
cd /d "%~dp0"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%i
set OUT=logs\%STAMP%-clock
mkdir "%OUT%" 2>nul
copy /y VERSION.txt "%OUT%\" >nul
echo Logging to %OUT%
rem Millisecond clock at the top-left of the main screen. Esc quits.
rem Photograph the monitor and the projector together while streaming.
bin\llstream_clock.exe %* > "%OUT%\run.log" 2>&1
type "%OUT%\run.log"

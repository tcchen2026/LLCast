@echo off
rem Generated from tools/windows in the LLStream repository; deploy with tools/deploy_windows.py.
setlocal
cd /d "%~dp0"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%i
set OUT=logs\%STAMP%-vmtest
mkdir "%OUT%" 2>nul
copy /y VERSION.txt "%OUT%\" >nul
echo Logging to %OUT%
rem Streams a moving test pattern to the receiver on the Linux VM, in three
rem 20 s runs: clean, 1%% test loss without FEC, and 1%% test loss with 20%% FEC.
rem No capture, so Remote Desktop is fine. The VM receiver must already be running.
set VM=%~1
if "%VM%"=="" set VM=192.168.185.100
set MOTION=--pattern-motion 8 --no-scale --seconds 20
echo [1/3] clean
bin\llstream_win.exe --host %VM% %MOTION% --profile-to %VM%:47801 > "%OUT%\run-clean.log" 2>&1
type "%OUT%\run-clean.log"
echo [2/3] 1%% loss, no FEC
bin\llstream_win.exe --host %VM% %MOTION% --drop-rate 0.01 --profile-to %VM%:47801 > "%OUT%\run-loss.log" 2>&1
type "%OUT%\run-loss.log"
echo [3/3] 1%% loss, 20%% FEC
bin\llstream_win.exe --host %VM% %MOTION% --drop-rate 0.01 --fec-percent 20 --profile-to %VM%:47801 > "%OUT%\run-fec.log" 2>&1
type "%OUT%\run-fec.log"

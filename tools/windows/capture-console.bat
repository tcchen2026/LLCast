@echo off
rem Optional, for when you reach the PC only through Remote Desktop.
rem Moves this RDP session onto the PC's own monitor (Windows tscon), runs
rem capture.bat there, then locks the PC. Needs administrator rights.
rem Your RDP window disconnects; reconnect after about 30 s to read the
rem results. Until the lock, the desktop is visible on the PC's monitor.
setlocal
cd /d "%~dp0"
net session >nul 2>&1 || (echo Right-click and choose "Run as administrator". & pause & exit /b 1)
set SESSION=
rem This session's ID: the third column of its own row (e.g. ">rdp-tcp#5 john 1 Active").
for /f "skip=1 tokens=3" %%s in ('query session %SESSIONNAME%') do if not defined SESSION set SESSION=%%s
if not defined SESSION (echo Could not find this session. & pause & exit /b 1)
echo Moving session %SESSION% to the console in 3 s; reconnect RDP after about 30 s.
timeout /t 3 /nobreak >nul
%windir%\System32\tscon.exe %SESSION% /dest:console || (echo tscon failed. & pause & exit /b 1)
rem Give the console display a moment to come up before capturing it.
timeout /t 5 /nobreak >nul
call capture.bat
rundll32.exe user32.dll,LockWorkStation

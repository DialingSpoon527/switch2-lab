@echo off
setlocal
echo HEN-only release helper - runs locally in WSL. Does not install to the console.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Release.ps1"
if errorlevel 1 echo Build helper stopped. Read the message above.
pause

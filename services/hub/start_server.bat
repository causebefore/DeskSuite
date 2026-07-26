@echo off
REM hyper_rlcd server launcher (ASCII-only, calls PowerShell script)
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_server.ps1"
echo.
echo server has exited. Press any key to close...
pause >nul
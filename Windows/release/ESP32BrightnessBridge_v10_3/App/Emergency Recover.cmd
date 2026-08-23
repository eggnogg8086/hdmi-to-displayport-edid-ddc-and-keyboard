@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title ESP32 Brightness Bridge Emergency Recovery

fltmc >nul 2>&1
if errorlevel 1 (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo Stopping automatic control and forcing the saved brightness back on...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0emergency_recover.ps1"
echo.
echo Recovery command completed.
pause

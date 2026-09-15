@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title ESP32 Brightness Bridge Status
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0health_check.ps1"
echo.
pause

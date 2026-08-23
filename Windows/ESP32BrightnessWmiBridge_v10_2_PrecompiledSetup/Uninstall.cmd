@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title ESP32 Brightness Bridge Uninstaller

fltmc >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator permission...
    if /I "%~1"=="/quiet" (
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '/quiet' -Verb RunAs"
    ) else (
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    )
    exit /b
)

echo.
echo ==============================================
echo   ESP32 Brightness Bridge - Uninstall
echo ==============================================
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall_agent.ps1"
set "RC=%ERRORLEVEL%"

if /I "%~1"=="/quiet" exit /b %RC%

echo.
if "%RC%"=="0" (
    echo Uninstall completed. The backlight was restored before removal.
) else (
    echo Uninstall reported an error. See the message above.
)
echo.
pause
exit /b %RC%

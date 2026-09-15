@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title ESP32 Brightness Bridge Setup

rem Works from either the developer folder or the clean end-user release.
set "INSTALLSCRIPT=%~dp0install_agent.ps1"
if not exist "%INSTALLSCRIPT%" set "INSTALLSCRIPT=%~dp0App\install_agent.ps1"

if not exist "%INSTALLSCRIPT%" (
    echo.
    echo ERROR: Installation files are missing.
    echo Please extract the complete ESP32 Brightness Bridge package and try again.
    echo.
    pause
    exit /b 2
)

fltmc >nul 2>&1
if errorlevel 1 (
    echo Requesting administrator permission...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo.
echo ==============================================
echo   ESP32 Brightness Bridge - Install / Update
echo ==============================================
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%INSTALLSCRIPT%"
if errorlevel 1 goto :failed

echo.
echo Installation completed successfully.
echo.
echo You can uninstall later from:
echo   Settings ^> Apps ^> Installed apps ^> ESP32 Brightness Bridge
echo.
echo Emergency recovery hotkey:
echo   Ctrl+Alt+Shift+B
echo.
pause
exit /b 0

:failed
echo.
echo INSTALLATION FAILED.
echo Automatic display control was not enabled unless the safety checks passed.
echo.
pause
exit /b 1

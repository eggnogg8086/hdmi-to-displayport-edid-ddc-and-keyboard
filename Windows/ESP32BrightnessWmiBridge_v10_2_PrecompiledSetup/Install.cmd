@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title ESP32 Brightness Bridge Setup

rem End-user installer. This package NEVER compiles anything on the user's PC.
rem The developer must run build.bat before creating the release package.

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

if not exist "build\Esp32DisplayPowerAgent.exe" goto :missing
if not exist "build\Esp32DisplayPowerBridge.exe" goto :missing

goto :install

:missing
echo ERROR: Precompiled application files are missing.
echo.
echo This installer does not compile software on the user's computer.
echo Please use the complete release package supplied by the developer.
echo.
pause
exit /b 2

:install
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_agent.ps1"
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
echo The installer has left automatic display control disabled or restored the
echo backlight where possible. Review the message above before retrying.
echo.
pause
exit /b 1

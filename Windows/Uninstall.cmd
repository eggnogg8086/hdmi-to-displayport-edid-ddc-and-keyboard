@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title ESP32 Brightness Bridge Uninstaller

rem Works both from the extracted release and from Program Files after install.
set "UNINSTALLSCRIPT=%~dp0uninstall_agent.ps1"
if not exist "%UNINSTALLSCRIPT%" set "UNINSTALLSCRIPT=%~dp0App\uninstall_agent.ps1"

if not exist "%UNINSTALLSCRIPT%" (
    echo.
    echo ERROR: Uninstallation files are missing.
    echo You can also uninstall from Windows Settings ^> Apps ^> Installed apps.
    echo.
    pause
    exit /b 2
)

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

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%UNINSTALLSCRIPT%"
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

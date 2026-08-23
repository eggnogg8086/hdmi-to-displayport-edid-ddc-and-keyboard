@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title ESP32 Brightness Bridge Release Packager

rem This script NEVER compiles. Run build.bat first on the developer PC.
if not exist "build\Esp32DisplayPowerAgent.exe" goto :missing
if not exist "build\Esp32DisplayPowerBridge.exe" goto :missing

set "NAME=ESP32BrightnessBridge_v10_3"
set "RELROOT=%CD%\release"
set "RELDIR=%RELROOT%\%NAME%"
set "APPDIR=%RELDIR%\App"
set "ZIP=%RELROOT%\%NAME%.zip"

if exist "%RELDIR%" rmdir /s /q "%RELDIR%"
if not exist "%RELROOT%" mkdir "%RELROOT%"
mkdir "%RELDIR%"
mkdir "%APPDIR%"
mkdir "%APPDIR%\bin"

rem The user-facing root deliberately contains only Install.cmd, Uninstall.cmd,
rem and the App support directory. Everything else is kept out of the way.
copy /y "Install.cmd" "%RELDIR%\Install.cmd" >nul
copy /y "Uninstall.cmd" "%RELDIR%\Uninstall.cmd" >nul

copy /y "build\Esp32DisplayPowerAgent.exe" "%APPDIR%\bin\" >nul
copy /y "build\Esp32DisplayPowerBridge.exe" "%APPDIR%\bin\" >nul
copy /y "install_agent.ps1" "%APPDIR%\" >nul
copy /y "uninstall_agent.ps1" "%APPDIR%\" >nul
copy /y "emergency_recover.ps1" "%APPDIR%\" >nul
copy /y "health_check.ps1" "%APPDIR%\" >nul
copy /y "Emergency Recover.cmd" "%APPDIR%\" >nul
copy /y "Status.cmd" "%APPDIR%\" >nul
copy /y "View Log.cmd" "%APPDIR%\" >nul
copy /y "Uninstall.cmd" "%APPDIR%\Uninstall.cmd" >nul
copy /y "README_FIRST.txt" "%APPDIR%\README.txt" >nul

if exist "%ZIP%" del /q "%ZIP%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%RELDIR%\*' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 exit /b 1

echo.
echo RELEASE READY:
echo   %ZIP%
echo.
echo The user will see only:
echo   Install.cmd
echo   Uninstall.cmd
echo   App\
echo.
echo The App folder contains internal support files and precompiled binaries.
echo No Visual Studio, compiler, SDK, or source code is included in the release.
echo.
pause
exit /b 0

:missing
echo.
echo PRECOMPILED BINARIES NOT FOUND.
echo Run build.bat successfully first, then run MakeRelease.bat again.
echo.
pause
exit /b 2

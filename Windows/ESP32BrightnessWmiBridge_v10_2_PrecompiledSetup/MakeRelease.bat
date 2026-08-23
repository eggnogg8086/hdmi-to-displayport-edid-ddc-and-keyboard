@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title ESP32 Brightness Bridge Release Packager

rem IMPORTANT: this script does not compile. Run build.bat first.
if not exist "build\Esp32DisplayPowerAgent.exe" goto :missing
if not exist "build\Esp32DisplayPowerBridge.exe" goto :missing

set "NAME=ESP32BrightnessBridge_v10_2_PrecompiledSetup"
set "RELROOT=%CD%\release"
set "RELDIR=%RELROOT%\%NAME%"
set "ZIP=%RELROOT%\%NAME%.zip"

if exist "%RELDIR%" rmdir /s /q "%RELDIR%"
if not exist "%RELROOT%" mkdir "%RELROOT%"
mkdir "%RELDIR%"
mkdir "%RELDIR%\build"

copy /y "build\Esp32DisplayPowerAgent.exe" "%RELDIR%\build\" >nul
copy /y "build\Esp32DisplayPowerBridge.exe" "%RELDIR%\build\" >nul
copy /y "Install.cmd" "%RELDIR%\" >nul
copy /y "Uninstall.cmd" "%RELDIR%\" >nul
copy /y "Emergency Recover.cmd" "%RELDIR%\" >nul
copy /y "Status.cmd" "%RELDIR%\" >nul
copy /y "View Log.cmd" "%RELDIR%\" >nul
copy /y "install_agent.ps1" "%RELDIR%\" >nul
copy /y "uninstall_agent.ps1" "%RELDIR%\" >nul
copy /y "emergency_recover.ps1" "%RELDIR%\" >nul
copy /y "health_check.ps1" "%RELDIR%\" >nul
copy /y "README_FIRST.txt" "%RELDIR%\" >nul

if exist "%ZIP%" del /q "%ZIP%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%RELDIR%\*' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 exit /b 1

echo.
echo RELEASE READY:
echo   %ZIP%
echo.
echo The release contains PRECOMPILED binaries. The recipient does not need
echo Visual Studio, MSVC, the Windows SDK, or build.bat.
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

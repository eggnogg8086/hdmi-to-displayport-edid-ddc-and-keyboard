@echo off
setlocal
set "CFG=%ProgramData%\ESP32BrightnessBridge\settings.ini"
if not exist "%CFG%" (
  echo Settings file not found:
  echo   %CFG%
  echo.
  echo Reinstall ESP32 Brightness Bridge to recreate it.
  pause
  exit /b 1
)
start "" notepad.exe "%CFG%"

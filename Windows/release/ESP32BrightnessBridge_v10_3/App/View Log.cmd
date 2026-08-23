@echo off
setlocal EnableExtensions
set "LOG=C:\ProgramData\ESP32BrightnessBridge\powerbridge.log"
if not exist "%LOG%" (
    echo No log file exists yet.
    pause
    exit /b 0
)
start "" notepad.exe "%LOG%"

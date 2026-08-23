@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found.
    echo Install Visual Studio Build Tools with Desktop development with C++.
    exit /b 1
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
    echo ERROR: Visual Studio C++ Build Tools not found.
    exit /b 1
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

if not exist build mkdir build

rem Console build: used by test_ddc.ps1 and run_agent_test.ps1 so diagnostics remain visible.
cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE ^
    Esp32DisplayPowerBridge.cpp ^
    /Fe:build\Esp32DisplayPowerBridge.exe ^
    /link User32.lib Advapi32.lib Dxva2.lib PowrProf.lib

if errorlevel 1 (
    echo.
    echo CONSOLE BUILD FAILED
    exit /b 1
)

rem Hidden installed-agent build: same code, Windows subsystem, no blank CMD window at logon.
cl /nologo /std:c++17 /EHsc /W4 /DUNICODE /D_UNICODE ^
    Esp32DisplayPowerBridge.cpp ^
    /Fe:build\Esp32DisplayPowerAgent.exe ^
    /link User32.lib Advapi32.lib Dxva2.lib PowrProf.lib /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup

if errorlevel 1 (
    echo.
    echo AGENT BUILD FAILED
    exit /b 1
)

echo.
echo BUILD OK:
echo   %CD%\build\Esp32DisplayPowerBridge.exe  ^(console diagnostics^)
echo   %CD%\build\Esp32DisplayPowerAgent.exe   ^(hidden installed agent^)

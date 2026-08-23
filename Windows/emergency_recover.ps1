$ErrorActionPreference = 'SilentlyContinue'

# This script is intentionally simple enough to run blindly from an elevated
# terminal if necessary. It disables the automation first, then restores light.
Stop-ScheduledTask -TaskName 'ESP32 Display Power Agent'
Get-Process Esp32DisplayPowerAgent | Stop-Process -Force
Start-Sleep -Milliseconds 200

$cli = Join-Path $env:ProgramFiles 'ESP32BrightnessBridge\Esp32DisplayPowerBridge.exe'
if (-not (Test-Path $cli)) {
    $cli = Join-Path $PSScriptRoot 'build\Esp32DisplayPowerBridge.exe'
}

if (Test-Path $cli) {
    & $cli --on
    Write-Host 'Automatic agent stopped and saved brightness restore requested.'
} else {
    Write-Host 'Agent stopped, but recovery CLI was not found.'
}

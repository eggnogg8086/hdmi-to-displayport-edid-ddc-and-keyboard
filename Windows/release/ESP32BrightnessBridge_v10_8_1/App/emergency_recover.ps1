$ErrorActionPreference = 'SilentlyContinue'

# Blind-safe recovery: stop automatic control first, then request the last saved
# nonzero brightness through the standalone CLI.
Stop-ScheduledTask -TaskName 'ESP32 Display Power Agent'
Get-Process Esp32DisplayPowerAgent | Stop-Process -Force
Start-Sleep -Milliseconds 200

$cli = Join-Path $env:ProgramFiles 'ESP32BrightnessBridge\Esp32DisplayPowerBridge.exe'
if (-not (Test-Path $cli)) {
    $cli = Join-Path $PSScriptRoot 'bin\Esp32DisplayPowerBridge.exe'
}
if (-not (Test-Path $cli)) {
    $cli = Join-Path $PSScriptRoot 'build\Esp32DisplayPowerBridge.exe'
}

if (Test-Path $cli) {
    $p = Start-Process -FilePath $cli -ArgumentList '--on' -Wait -PassThru
    if ($p.ExitCode -eq 0) {
        Write-Host 'Automatic agent stopped and saved brightness was restored.'
        exit 0
    }
    Write-Host "Automatic agent stopped, but brightness restore failed (exit $($p.ExitCode)). Wake the display output/DDC link and run recovery again."
    exit $p.ExitCode
}

Write-Host 'Agent stopped, but recovery CLI was not found.'
exit 2

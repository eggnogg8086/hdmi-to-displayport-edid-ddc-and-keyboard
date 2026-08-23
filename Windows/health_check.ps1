$ErrorActionPreference = 'Continue'
$taskName = 'ESP32 Display Power Agent'
$cli = Join-Path $env:ProgramFiles 'ESP32BrightnessBridge\Esp32DisplayPowerBridge.exe'
if (-not (Test-Path $cli)) { $cli = Join-Path $PSScriptRoot 'build\Esp32DisplayPowerBridge.exe' }

Write-Host '=== Agent task ==='
Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue | Select-Object TaskName, State
Get-ScheduledTaskInfo -TaskName $taskName -ErrorAction SilentlyContinue | Select-Object LastRunTime, LastTaskResult, NextRunTime, NumberOfMissedRuns

Write-Host "`n=== Agent process ==="
Get-Process Esp32DisplayPowerAgent -ErrorAction SilentlyContinue | Select-Object Id, StartTime, Path

Write-Host "`n=== DDC probe ==="
if (Test-Path $cli) { & $cli --probe } else { Write-Host 'CLI not found.' }

Write-Host "`n=== Saved brightness ==="
(Get-ItemProperty 'HKLM:\SOFTWARE\ESP32BrightnessBridge' -Name CurrentBrightness -ErrorAction SilentlyContinue).CurrentBrightness

Write-Host "`n=== Recent log ==="
$log = 'C:\ProgramData\ESP32BrightnessBridge\powerbridge.log'
if (Test-Path $log) { Get-Content $log -Tail 80 } else { Write-Host 'No log yet.' }

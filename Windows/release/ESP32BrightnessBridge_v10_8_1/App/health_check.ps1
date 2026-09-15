$ErrorActionPreference = 'Continue'
$taskName = 'ESP32 Display Power Agent'

$cli = Join-Path $env:ProgramFiles 'ESP32BrightnessBridge\Esp32DisplayPowerBridge.exe'
if (-not (Test-Path $cli)) {
    $cli = Join-Path $PSScriptRoot 'bin\Esp32DisplayPowerBridge.exe'
}
if (-not (Test-Path $cli)) {
    $cli = Join-Path $PSScriptRoot 'build\Esp32DisplayPowerBridge.exe'
}

Write-Host '=== Agent task ==='
Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue |
    Select-Object TaskName, State,
        @{N='UserId';E={$_.Principal.UserId}},
        @{N='RunLevel';E={$_.Principal.RunLevel}},
        @{N='Action';E={$_.Actions.Execute}}
Get-ScheduledTaskInfo -TaskName $taskName -ErrorAction SilentlyContinue |
    Select-Object LastRunTime, LastTaskResult, NextRunTime, NumberOfMissedRuns

Write-Host "`n=== Agent process ==="
Get-Process Esp32DisplayPowerAgent -ErrorAction SilentlyContinue |
    Select-Object Id, StartTime, Path

Write-Host "`n=== DDC probe ==="
if (Test-Path $cli) {
    & $cli --probe
} else {
    Write-Host 'CLI not found.'
}

Write-Host "`n=== Saved brightness ==="
(Get-ItemProperty 'HKLM:\SOFTWARE\ESP32BrightnessBridge' -Name CurrentBrightness -ErrorAction SilentlyContinue).CurrentBrightness

Write-Host "`n=== Windows power requests ==="
$requests = (& powercfg.exe /requests 2>&1 | Out-String)
$requests.TrimEnd() | Write-Host
if ($requests -match 'Esp32DisplayPowerAgent|ESP32 panel fade|ESP32BrightnessBridge') {
    Write-Warning 'ESP32 Brightness Bridge appears in powercfg /requests. v10.8.1.1 creates NO Windows power requests; check for an old process/version.'
} else {
    Write-Host 'PASS: no ESP32 Brightness Bridge power request is registered.'
}


Write-Host "`n=== Predictive DIM-to-OFF settings ==="
$settings = 'C:\ProgramData\ESP32BrightnessBridge\settings.ini'
if (Test-Path $settings) {
    Get-Content $settings | Write-Host
} else {
    Write-Warning 'settings.ini is missing.'
}

Write-Host "`n=== Learned DIM-to-OFF timing ==="
$state = Get-ItemProperty 'HKLM:\SOFTWARE\ESP32BrightnessBridge' -ErrorAction SilentlyContinue
Write-Host ("LearnedDimToOffMs: {0}" -f $state.LearnedDimToOffMs)
Write-Host ("LearnedDimToOffSamples: {0}" -f $state.LearnedDimToOffSamples)

Write-Host "`n=== Recent log ==="
$log = 'C:\ProgramData\ESP32BrightnessBridge\powerbridge.log'
if (Test-Path $log) {
    Get-Content $log -Tail 80
} else {
    Write-Host 'No log yet.'
}

$ErrorActionPreference = 'SilentlyContinue'

function Require-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator permission is required. Use Uninstall.cmd or Windows Installed Apps.'
    }
}

Require-Admin

$taskName = 'ESP32 Display Power Agent'
$installDir = Join-Path $env:ProgramFiles 'ESP32BrightnessBridge'
$cli = Join-Path $installDir 'Esp32DisplayPowerBridge.exe'

# First stop automation so it cannot race the recovery command.
Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
Get-Process Esp32DisplayPowerAgent -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 250

# SAFETY: restore the user's saved nonzero brightness before removing anything.
if (Test-Path $cli) {
    & $cli --on | Out-Null
    Start-Sleep -Milliseconds 500
}

Get-Process Esp32DisplayPowerBridge, Esp32DisplayPowerAgent -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue

# Remove the old development service as part of a full uninstall.
$svc = Get-Service -Name 'ESP32DisplayPowerBridge' -ErrorAction SilentlyContinue
if ($svc) {
    Stop-Service -Name 'ESP32DisplayPowerBridge' -Force -ErrorAction SilentlyContinue
    & sc.exe delete ESP32DisplayPowerBridge | Out-Null
}


# Best-effort cleanup for obsolete v5/v6 WMI-provider experiments.
foreach ($className in @(
    'ESP32_ProviderSelfTest',
    'ESP32_WmiMonitorBrightnessMethods',
    'ESP32_WmiMonitorBrightness'
)) {
    try {
        $scope = New-Object System.Management.ManagementScope('\\.\root\wmi')
        $scope.Connect()
        $path = New-Object System.Management.ManagementPath($className)
        $mc = New-Object System.Management.ManagementClass($scope, $path, $null)
        $mc.Delete()
    } catch {}
}
foreach ($registrationClass in @('__MethodProviderRegistration','__InstanceProviderRegistration')) {
    try {
        Get-CimInstance -Namespace root/wmi -ClassName $registrationClass -ErrorAction Stop |
            Where-Object {
                $provider = $_.Provider
                if ($provider -is [Microsoft.Management.Infrastructure.CimInstance]) {
                    return $provider.Name -eq 'ESP32BrightnessProvider'
                }
                return "$provider" -match 'ESP32BrightnessProvider'
            } |
            Remove-CimInstance -ErrorAction SilentlyContinue
    } catch {}
}
try {
    Get-CimInstance -Namespace root/wmi -ClassName __Win32Provider -Filter "Name='ESP32BrightnessProvider'" -ErrorAction Stop |
        Remove-CimInstance -ErrorAction SilentlyContinue
} catch {}

$legacyDll = Join-Path $installDir 'Esp32BrightnessProvider.dll'
if (Test-Path $legacyDll) {
    try {
        $regsvr = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
        Start-Process -FilePath $regsvr -ArgumentList @('/s','/u', "`"$legacyDll`"") -Wait | Out-Null
    } catch {}
}

# Remove Start Menu and Installed Apps registration.
$startMenuDir = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\ESP32 Brightness Bridge'
Remove-Item -LiteralPath $startMenuDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ESP32BrightnessBridge' -Recurse -Force -ErrorAction SilentlyContinue

# Remove application settings/logs only after the restore command above has run.
Remove-Item -LiteralPath 'HKLM:\SOFTWARE\ESP32BrightnessBridge' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath 'C:\ProgramData\ESP32BrightnessBridge' -Recurse -Force -ErrorAction SilentlyContinue

# This script may itself be running from Program Files. Ask a short-lived helper
# to remove the install directory after this PowerShell process and Uninstall.cmd
# have exited.
if (Test-Path $installDir) {
    $cleanup = Join-Path $env:TEMP ("ESP32BrightnessBridge_cleanup_{0}.ps1" -f $PID)
    $escapedInstallDir = $installDir.Replace("'", "''")
    $escapedCleanup = $cleanup.Replace("'", "''")
    @"
Start-Sleep -Seconds 4
Remove-Item -LiteralPath '$escapedInstallDir' -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath '$escapedCleanup' -Force -ErrorAction SilentlyContinue
"@ | Set-Content -LiteralPath $cleanup -Encoding UTF8

    Start-Process -FilePath 'powershell.exe' `
        -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File', ("`"$cleanup`"")) `
        -WindowStyle Hidden | Out-Null
}

Write-Host 'ESP32 Brightness Bridge was removed and the saved brightness was restored.'
exit 0

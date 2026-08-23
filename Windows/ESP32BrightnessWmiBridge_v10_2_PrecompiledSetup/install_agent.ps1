$ErrorActionPreference = 'Stop'

function Require-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator permission is required. Double-click Install.cmd instead of running this script directly.'
    }
}


function Remove-LegacyWmiProvider {
    Write-Host 'Checking for obsolete experimental WMI provider...'

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
            Write-Host "  removed legacy WMI class $className"
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

    $legacyDll = Join-Path $env:ProgramFiles 'ESP32BrightnessBridge\Esp32BrightnessProvider.dll'
    if (Test-Path $legacyDll) {
        try {
            $regsvr = Join-Path $env:SystemRoot 'System32\regsvr32.exe'
            Start-Process -FilePath $regsvr -ArgumentList @('/s','/u', "`"$legacyDll`"") -Wait | Out-Null
        } catch {}
        Remove-Item -LiteralPath $legacyDll -Force -ErrorAction SilentlyContinue
    }
}

function New-AppShortcut {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Target,
        [string]$WorkingDirectory = '',
        [string]$Arguments = ''
    )

    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    $shortcut.TargetPath = $Target
    if ($WorkingDirectory) { $shortcut.WorkingDirectory = $WorkingDirectory }
    if ($Arguments) { $shortcut.Arguments = $Arguments }
    $shortcut.Save()
}

Require-Admin

# The UAC prompt may be approved with a different administrator account. Always
# install the interactive agent for the user who is actually logged on to the
# desktop, not blindly for the elevated account running this script.
$targetUser = $null
try {
    $targetUser = (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).UserName
} catch {}
if (-not $targetUser) {
    $targetUser = "$env:USERDOMAIN\$env:USERNAME"
}
Write-Host "Interactive user: $targetUser"

Remove-LegacyWmiProvider

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcAgent = Join-Path $root 'build\Esp32DisplayPowerAgent.exe'
$srcCli = Join-Path $root 'build\Esp32DisplayPowerBridge.exe'
if (-not (Test-Path $srcAgent)) { throw "Agent EXE not found: $srcAgent`nThe precompiled Agent EXE is missing. Use the complete release package supplied by the developer." }
if (-not (Test-Path $srcCli)) { throw "CLI EXE not found: $srcCli`nThe precompiled CLI EXE is missing. Use the complete release package supplied by the developer." }

$taskName = 'ESP32 Display Power Agent'
$existingTask = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($existingTask) {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
}

# Stop every old development build before replacing files. This prevents two
# versions from racing each other over VCP 0x10.
Get-Process Esp32DisplayPowerBridge, Esp32DisplayPowerAgent -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

$installDir = Join-Path $env:ProgramFiles 'ESP32BrightnessBridge'
$dstAgent = Join-Path $installDir 'Esp32DisplayPowerAgent.exe'
$dstCli = Join-Path $installDir 'Esp32DisplayPowerBridge.exe'
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Copy-Item $srcAgent $dstAgent -Force
Copy-Item $srcCli $dstCli -Force

# Copy the tiny maintenance tools so Windows Settings can uninstall the app
# without needing the original extracted ZIP.
$supportFiles = @(
    'Uninstall.cmd',
    'Emergency Recover.cmd',
    'Status.cmd',
    'View Log.cmd',
    'uninstall_agent.ps1',
    'emergency_recover.ps1',
    'health_check.ps1',
    'README_FIRST.txt'
)
foreach ($name in $supportFiles) {
    $source = Join-Path $root $name
    if (Test-Path $source) {
        Copy-Item $source (Join-Path $installDir $name) -Force
    }
}

New-Item -ItemType Directory -Force -Path 'C:\ProgramData\ESP32BrightnessBridge' | Out-Null
New-Item -Path 'HKLM:\SOFTWARE\ESP32BrightnessBridge' -Force | Out-Null
if ($null -eq (Get-ItemProperty -Path 'HKLM:\SOFTWARE\ESP32BrightnessBridge' -Name CurrentBrightness -ErrorAction SilentlyContinue)) {
    New-ItemProperty -Path 'HKLM:\SOFTWARE\ESP32BrightnessBridge' -Name CurrentBrightness -PropertyType DWord -Value 50 -Force | Out-Null
}


# The agent itself does not need administrator privileges. Give the actual
# interactive user write access only to this application's state and log folder,
# so a standard-user account works after an administrator approves installation.
try {
    $regPath = 'HKLM:\SOFTWARE\ESP32BrightnessBridge'
    $regAcl = Get-Acl $regPath
    $regRule = [System.Security.AccessControl.RegistryAccessRule]::new(
        $targetUser,
        [System.Security.AccessControl.RegistryRights]::FullControl,
        [System.Security.AccessControl.InheritanceFlags]::ContainerInherit,
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Allow
    )
    $regAcl.SetAccessRule($regRule)
    Set-Acl -Path $regPath -AclObject $regAcl
} catch {
    throw "Could not grant the interactive user access to brightness state: $($_.Exception.Message)"
}

try {
    $dataPath = 'C:\ProgramData\ESP32BrightnessBridge'
    $fsAcl = Get-Acl $dataPath
    $fsRule = [System.Security.AccessControl.FileSystemAccessRule]::new(
        $targetUser,
        [System.Security.AccessControl.FileSystemRights]::Modify,
        ([System.Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [System.Security.AccessControl.InheritanceFlags]::ObjectInherit),
        [System.Security.AccessControl.PropagationFlags]::None,
        [System.Security.AccessControl.AccessControlType]::Allow
    )
    $fsAcl.SetAccessRule($fsRule)
    Set-Acl -Path $dataPath -AclObject $fsAcl
} catch {
    throw "Could not grant the interactive user access to the log folder: $($_.Exception.Message)"
}

# Stop the obsolete session-0 service so it cannot race the interactive agent.
$svc = Get-Service -Name 'ESP32DisplayPowerBridge' -ErrorAction SilentlyContinue
if ($svc) {
    if ($svc.Status -ne 'Stopped') { Stop-Service $svc.Name -Force }
    Set-Service $svc.Name -StartupType Disabled
    Write-Host 'Disabled old ESP32DisplayPowerBridge service.'
}

# SAFETY: before enabling any automation, force the panel visible and prove that
# the target AUOD0A2 DDC path can be opened. If this fails, do not enable an
# automatic display-off agent.
& $dstCli --on | Out-Null
$probe = Start-Process -FilePath $dstCli -ArgumentList '--probe' -Wait -PassThru -WindowStyle Hidden
if ($probe.ExitCode -ne 0) {
    & $dstCli --on | Out-Null
    throw "DDC safety probe failed with exit code $($probe.ExitCode). Automatic display control was NOT enabled."
}

$action = New-ScheduledTaskAction -Execute $dstAgent -Argument '--agent'
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $targetUser
$principal = New-ScheduledTaskPrincipal -UserId $targetUser -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -MultipleInstances IgnoreNew `
    -RestartCount 999 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -StartWhenAvailable

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 2

$agent = Get-Process Esp32DisplayPowerAgent -ErrorAction SilentlyContinue
if (-not $agent) {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    & $dstCli --on | Out-Null
    throw 'The hidden agent did not remain running. The task was removed and brightness was restored; check powerbridge.log before retrying.'
}

try {
    # Register a normal Installed Apps entry.
    $uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\ESP32BrightnessBridge'
    $installedUninstall = Join-Path $installDir 'Uninstall.cmd'
    $uninstallCommand = ('{0} /d /c ""{1}" /quiet"' -f $env:ComSpec, $installedUninstall)
    New-Item -Path $uninstallKey -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name DisplayName -PropertyType String -Value 'ESP32 Brightness Bridge' -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name DisplayVersion -PropertyType String -Value '10.2' -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name Publisher -PropertyType String -Value 'ESP32 Display Project' -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name InstallLocation -PropertyType String -Value $installDir -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name DisplayIcon -PropertyType String -Value $dstAgent -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name UninstallString -PropertyType String -Value $uninstallCommand -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name QuietUninstallString -PropertyType String -Value $uninstallCommand -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name NoModify -PropertyType DWord -Value 1 -Force | Out-Null
    New-ItemProperty -Path $uninstallKey -Name NoRepair -PropertyType DWord -Value 1 -Force | Out-Null

    # Add simple Start Menu recovery/uninstall shortcuts.
    $startMenuDir = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\ESP32 Brightness Bridge'
    New-Item -ItemType Directory -Force -Path $startMenuDir | Out-Null
    New-AppShortcut -Path (Join-Path $startMenuDir 'Emergency Recover.lnk') -Target (Join-Path $installDir 'Emergency Recover.cmd') -WorkingDirectory $installDir
    New-AppShortcut -Path (Join-Path $startMenuDir 'Status and Diagnostics.lnk') -Target (Join-Path $installDir 'Status.cmd') -WorkingDirectory $installDir
    New-AppShortcut -Path (Join-Path $startMenuDir 'View Log.lnk') -Target (Join-Path $installDir 'View Log.cmd') -WorkingDirectory $installDir
    New-AppShortcut -Path (Join-Path $startMenuDir 'Uninstall ESP32 Brightness Bridge.lnk') -Target $installedUninstall -WorkingDirectory $installDir -Arguments '/quiet'
} catch {
    Write-Warning "The agent is installed, but Windows Apps/Start Menu registration could not be completed: $($_.Exception.Message)"
}

Write-Host ''
Write-Host 'ESP32 Brightness Bridge 10.2 installed and started.'
Write-Host 'Installed Apps entry: ESP32 Brightness Bridge'
Write-Host 'Agent:' $dstAgent
Write-Host 'Log: C:\ProgramData\ESP32BrightnessBridge\powerbridge.log'
Write-Host 'Emergency blind recovery: Ctrl+Alt+Shift+B'
Write-Host ''
Write-Host 'The user can uninstall from Windows Settings > Apps > Installed apps.'

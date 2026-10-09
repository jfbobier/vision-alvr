<#
.SYNOPSIS
  Registers (or with -Unregister, unregisters) the VisionALVR folder this script sits in as the Windows OpenXR runtime.

.DESCRIPTION
  VisionALVR is portable: unzip it anywhere, run register_openxr_runtime.bat once (it asks for administrator rights). This sets
  HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime to <this folder>\virtualdesktop-openxr.json (the previous runtime is saved and put
  back by -Unregister), lists it under AvailableRuntimes, and adds an inbound firewall rule for <this folder>\alvr_host.exe
  (skip with -NoFirewall). Nothing is copied anywhere. Moving the folder: run it again from the new place.
  Earlier installs of this project (ClearXR, or VisionALVR in Program Files) are taken over: their saved previous runtime,
  their host config (paired headset) and their registry/firewall entries; their files are deleted only if their install marker
  is there. -RegistryRoot / -StateKey / -Legacy* exist for the tests (a throwaway HKCU key).
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$Unregister,
    [switch]$NoFirewall,
    [switch]$Quiet,              # no pause (used by VisionALVR.exe)
    [string]$InstallDir = '',
    [string]$RegistryRoot = 'HKLM:\SOFTWARE\Khronos\OpenXR\1',
    [string]$StateKey = 'HKLM:\SOFTWARE\VisionALVR',
    [string]$LegacyStateKey = 'HKLM:\SOFTWARE\ClearXR',
    [string[]]$OldInstallDirs = @(),
    [string[]]$OldConfigDirs = @()
)

$ErrorActionPreference = 'Stop'
if (-not $InstallDir) { $InstallDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path } }
$InstallDir = (Resolve-Path $InstallDir).Path.TrimEnd('\')
$pf = if ($env:ProgramFiles) { $env:ProgramFiles } else { 'C:\Program Files' }
$lad = if ($env:LOCALAPPDATA) { $env:LOCALAPPDATA } else { Join-Path $env:USERPROFILE 'AppData\Local' }
if (-not $OldInstallDirs) { $OldInstallDirs = @((Join-Path $pf 'ClearXR'), (Join-Path $pf 'VisionALVR')) }
if (-not $OldConfigDirs) { $OldConfigDirs = @((Join-Path $lad 'VisionALVR\host'), (Join-Path $lad 'ClearXR\host')) }
$Manifest = Join-Path $InstallDir 'virtualdesktop-openxr.json'
$HostExe = Join-Path $InstallDir 'alvr_host.exe'
$FirewallName = 'VisionALVR (alvr_host)'
$OldFirewallNames = @('VisionALVR streamer (alvr_host)', 'ClearXR streamer (alvr_host)')

function Done($code) { if (-not $Quiet) { Read-Host 'Press Enter to close' | Out-Null }; exit $code }

# A manifest that belongs to this project (any folder, any earlier name): never saved as "the runtime to restore".
function Test-Ours([string]$path) {
    if (-not $path) { return $false }
    if ($path -ieq $Manifest) { return $true }
    $dir = Split-Path -Parent $path
    if (-not $dir) { return $false }
    if ((Test-Path (Join-Path $dir 'alvr_host.exe')) -or (Test-Path (Join-Path (Split-Path -Parent $dir) 'host\alvr_host.exe'))) { return $true }
    foreach ($d in $OldInstallDirs) { if ($path -like "$d\*") { return $true } }
    return $false
}

function Get-Active { if (Test-Path $RegistryRoot) { (Get-ItemProperty -Path $RegistryRoot -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime } }

try {
    if (-not (Test-Path $Manifest)) { throw "virtualdesktop-openxr.json not found next to this script ($InstallDir)" }

    if (-not $Unregister) {
        # ---- the runtime to put back on -Unregister
        $active = Get-Active
        $saved = $null
        if (Test-Path $StateKey) { $saved = (Get-ItemProperty -Path $StateKey -Name PreviousActiveRuntime -ErrorAction SilentlyContinue).PreviousActiveRuntime }
        if ($null -eq $saved -and (Test-Path $LegacyStateKey)) { $saved = (Get-ItemProperty -Path $LegacyStateKey -Name PreviousActiveRuntime -ErrorAction SilentlyContinue).PreviousActiveRuntime }
        $prev = if (Test-Ours $active) { $saved } else { $active }
        if ($PSCmdlet.ShouldProcess($RegistryRoot, "ActiveRuntime = $Manifest (previous: '$prev')")) {
            # NB: never `New-Item -Force` an existing registry key: it clears the key's values (and would wipe other runtimes)
            if (-not (Test-Path $StateKey)) { New-Item -Path $StateKey -Force | Out-Null }
            New-ItemProperty -Path $StateKey -Name PreviousActiveRuntime -Value $(if ($prev) { $prev } else { '' }) -PropertyType String -Force | Out-Null
            New-ItemProperty -Path $StateKey -Name InstallDir -Value $InstallDir -PropertyType String -Force | Out-Null
            if (-not (Test-Path $RegistryRoot)) { New-Item -Path $RegistryRoot -Force | Out-Null }
            New-ItemProperty -Path $RegistryRoot -Name ActiveRuntime -Value $Manifest -PropertyType String -Force | Out-Null
            if (-not (Test-Path "$RegistryRoot\AvailableRuntimes")) { New-Item -Path "$RegistryRoot\AvailableRuntimes" | Out-Null }
            # our other (older, moved) manifests leave the list; other runtimes stay
            foreach ($name in (Get-Item "$RegistryRoot\AvailableRuntimes").Property) {
                if ($name -ine $Manifest -and (Test-Ours $name)) { Remove-ItemProperty -Path "$RegistryRoot\AvailableRuntimes" -Name $name }
            }
            New-ItemProperty -Path "$RegistryRoot\AvailableRuntimes" -Name $Manifest -Value 0 -PropertyType DWord -Force | Out-Null
            if (Test-Path $LegacyStateKey) { Remove-Item -Path $LegacyStateKey -Recurse -Force }
            Write-Host "OpenXR runtime: $Manifest"
            Write-Host "previous runtime (restored by unregister_openxr_runtime.bat): '$prev'"
        }

        # ---- host config of an earlier install: keep the paired headset
        $cfg = Join-Path $InstallDir 'config'
        if (-not (Test-Path (Join-Path $cfg 'session.json'))) {
            foreach ($old in $OldConfigDirs) {
                if ((Test-Path (Join-Path $old 'session.json')) -and $PSCmdlet.ShouldProcess($cfg, "take over the host config from $old")) {
                    New-Item -ItemType Directory -Force -Path $cfg | Out-Null
                    Copy-Item (Join-Path $old 'session.json') (Join-Path $cfg 'session.json')
                    foreach ($v in 'visionalvr_views.json', 'clearxr_views.json') {
                        if (Test-Path (Join-Path $old $v)) { Copy-Item (Join-Path $old $v) (Join-Path $cfg 'visionalvr_views.json') }
                    }
                    Write-Host "host config taken over from $old"
                    break
                }
            }
        }

        # ---- firewall (inbound: discovery UDP 9943, stream UDP 9944)
        foreach ($n in @($FirewallName) + $OldFirewallNames) {
            $r = Get-NetFirewallRule -DisplayName $n -ErrorAction SilentlyContinue
            if ($r -and $PSCmdlet.ShouldProcess($n, 'remove the old firewall rule')) { $r | Remove-NetFirewallRule }
        }
        if (-not $NoFirewall -and $PSCmdlet.ShouldProcess($HostExe, 'add inbound firewall rule')) {
            New-NetFirewallRule -DisplayName $FirewallName -Direction Inbound -Action Allow -Program $HostExe -Profile Any | Out-Null
            Write-Host "firewall: inbound allowed for $HostExe"
        }

        # ---- leftovers of the installer era: logon tasks, Program Files copies (only with their install marker)
        foreach ($t in 'VisionALVR Streamer', 'ClearXR Streamer') {
            if ((Get-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue) -and $PSCmdlet.ShouldProcess($t, 'remove the old logon task')) {
                Stop-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue
                Unregister-ScheduledTask -TaskName $t -Confirm:$false
            }
        }
        foreach ($d in $OldInstallDirs) {
            $marked = (Test-Path (Join-Path $d 'clearxr-install.json')) -or (Test-Path (Join-Path $d 'visionalvr-install.json'))
            if ($marked -and ($d -ine $InstallDir) -and $PSCmdlet.ShouldProcess($d, 'delete the old installed copy (its install marker is present)')) {
                Remove-Item -Path $d -Recurse -Force
                Write-Host "old copy removed: $d"
            }
        }
        Write-Host 'done.'
        Done 0
    }

    # ---------------------------------------------------------------- unregister
    if ($PSCmdlet.ShouldProcess($RegistryRoot, 'restore the previous ActiveRuntime and drop our AvailableRuntimes entry')) {
        $active = Get-Active
        if ($active -ieq $Manifest) {
            $prev = $null
            if (Test-Path $StateKey) { $prev = (Get-ItemProperty -Path $StateKey -Name PreviousActiveRuntime -ErrorAction SilentlyContinue).PreviousActiveRuntime }
            if ($prev) { Set-ItemProperty -Path $RegistryRoot -Name ActiveRuntime -Value $prev; Write-Host "ActiveRuntime restored: $prev" }
            else { Remove-ItemProperty -Path $RegistryRoot -Name ActiveRuntime -ErrorAction SilentlyContinue; Write-Host 'ActiveRuntime removed (there was none before)' }
        } else {
            Write-Host "ActiveRuntime is '$active', not this folder: left alone"
        }
        if (Test-Path "$RegistryRoot\AvailableRuntimes") { Remove-ItemProperty -Path "$RegistryRoot\AvailableRuntimes" -Name $Manifest -ErrorAction SilentlyContinue }
        if (Test-Path $StateKey) { Remove-Item -Path $StateKey -Recurse -Force }
    }
    if (-not $NoFirewall -and $PSCmdlet.ShouldProcess($FirewallName, 'remove firewall rule')) {
        Get-NetFirewallRule -DisplayName $FirewallName -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    }
    Write-Host 'unregistered (the folder, its config and logs are left as they are).'
    Done 0
} catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
    Done 1
}

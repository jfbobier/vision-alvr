# Exercises register_openxr_runtime.ps1 (portable VisionALVR folder) against a THROWAWAY registry key under HKCU (never HKLM)
# and scratch folders: register, re-register, WhatIf, unregister (restores the previous runtime), no previous runtime, a
# foreign runtime activated meanwhile, a moved folder, and taking over an earlier install (ClearXR / VisionALVR in Program Files).
$ErrorActionPreference = 'Stop'
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$pkg = "$root\out\dist\VisionALVR"
$tmp = "$root\runs\install_test"
$testRoot = 'HKCU:\Software\VisionALVRTest'
$reg = "$testRoot\Khronos\OpenXR\1"
$state = "$testRoot\State"
$legacyState = "$testRoot\LegacyState"
$inst = "$tmp\VisionALVR"                # "unzipped" here
$manifest = "$inst\virtualdesktop-openxr.json"
$fail = 0
function Check($name, $ok, $detail = '') { if ($ok) { Write-Host "CHECK $name PASS $detail" } else { Write-Host "CHECK $name FAIL $detail"; $script:fail++ } }
function Active { (Get-ItemProperty -Path $reg -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime }
function Saved { (Get-ItemProperty -Path $state -Name PreviousActiveRuntime -ErrorAction SilentlyContinue).PreviousActiveRuntime }

if (Test-Path $testRoot) { Remove-Item $testRoot -Recurse -Force }
if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
New-Item -ItemType Directory -Force $tmp | Out-Null
Copy-Item $pkg $inst -Recurse
New-Item -Path $reg -Force | Out-Null
New-ItemProperty -Path $reg -Name ActiveRuntime -Value 'C:\fake\previous_runtime.json' -PropertyType String | Out-Null
New-Item -Path "$reg\AvailableRuntimes" | Out-Null
New-ItemProperty -Path "$reg\AvailableRuntimes" -Name 'C:\fake\sibling_runtime.json' -Value 0 -PropertyType DWord | Out-Null   # another runtime next to ours

$oldPf = @("$tmp\pf_ClearXR", "$tmp\pf_VisionALVR")
$oldCfg = @("$tmp\lad_VisionALVR_host", "$tmp\lad_ClearXR_host")
$common = @{ RegistryRoot = $reg; StateKey = $state; LegacyStateKey = $legacyState; NoFirewall = $true; Quiet = $true; OldInstallDirs = $oldPf; OldConfigDirs = $oldCfg }
$ps = "$inst\register_openxr_runtime.ps1"

Check 'package_is_flat' ((Test-Path "$inst\alvr_host.exe") -and (Test-Path "$inst\LibOVRRT64_1.dll") -and (Test-Path "$inst\virtualdesktop-openxr.dll") -and
    (Test-Path $manifest) -and (Test-Path "$inst\register_openxr_runtime.bat") -and (Test-Path "$inst\config\session.default.json"))
& $ps @common | Out-Null
Check 'active_runtime_is_this_folder' ((Active) -eq $manifest) (Active)
Check 'available_runtimes_entry' ($null -ne (Get-ItemProperty "$reg\AvailableRuntimes" -Name $manifest -ErrorAction SilentlyContinue))
Check 'previous_runtime_saved' ((Saved) -eq 'C:\fake\previous_runtime.json') (Saved)
Check 'sibling_runtime_kept' ($null -ne (Get-ItemProperty "$reg\AvailableRuntimes" -Name 'C:\fake\sibling_runtime.json' -ErrorAction SilentlyContinue))

& $ps @common | Out-Null
Check 'reregister_keeps_previous' ((Saved) -eq 'C:\fake\previous_runtime.json') (Saved)

& $ps @common -Unregister -WhatIf | Out-Null
Check 'whatif_changes_nothing' ((Active) -eq $manifest)

& $ps @common -Unregister | Out-Null
Check 'unregister_restores_previous' ((Active) -eq 'C:\fake\previous_runtime.json') (Active)
Check 'unregister_drops_available_entry' ($null -eq (Get-ItemProperty "$reg\AvailableRuntimes" -Name $manifest -ErrorAction SilentlyContinue))
Check 'unregister_keeps_folder' (Test-Path "$inst\alvr_host.exe")

Remove-ItemProperty -Path $reg -Name ActiveRuntime
& $ps @common | Out-Null
& $ps @common -Unregister | Out-Null
Check 'unregister_without_previous_removes_value' ($null -eq (Active))

& $ps @common | Out-Null
Set-ItemProperty -Path $reg -Name ActiveRuntime -Value 'C:\other\runtime.json'
& $ps @common -Unregister | Out-Null
Check 'unregister_leaves_foreign_runtime' ((Active) -eq 'C:\other\runtime.json')

# moved folder: register from the new place; the old place is ours and must not become "the previous runtime"
Set-ItemProperty -Path $reg -Name ActiveRuntime -Value 'C:\fake\previous_runtime.json'
& $ps @common | Out-Null
$moved = "$tmp\moved\VisionALVR"
New-Item -ItemType Directory -Force (Split-Path $moved) | Out-Null
Copy-Item $inst $moved -Recurse
& "$moved\register_openxr_runtime.ps1" @common | Out-Null
Check 'moved_folder_is_active' ((Active) -eq "$moved\virtualdesktop-openxr.json") (Active)
Check 'moved_folder_keeps_previous' ((Saved) -eq 'C:\fake\previous_runtime.json') (Saved)
Check 'moved_folder_old_entry_removed' ($null -eq (Get-ItemProperty "$reg\AvailableRuntimes" -Name $manifest -ErrorAction SilentlyContinue))
& "$moved\register_openxr_runtime.ps1" @common -Unregister | Out-Null

# earlier install: ClearXR in "Program Files" with its marker, its state key, its config with a paired headset
$cx = $oldPf[0]
New-Item -ItemType Directory -Force "$cx\runtime", "$cx\host", $oldCfg[1] | Out-Null
Set-Content "$cx\clearxr-install.json" '{}'
Set-Content "$cx\host\alvr_host.exe" ''
Set-Content "$cx\runtime\virtualdesktop-openxr.json" '{}'
Set-Content "$($oldCfg[1])\session.json" '{"client_connections":{"x":{}}}'
Set-Content "$($oldCfg[1])\clearxr_views.json" '{}'
if (Test-Path $state) { Remove-Item $state -Recurse -Force }
New-Item -Path $legacyState -Force | Out-Null
New-ItemProperty -Path $legacyState -Name PreviousActiveRuntime -Value 'C:\fake\before_clearxr.json' -PropertyType String | Out-Null
Set-ItemProperty -Path $reg -Name ActiveRuntime -Value "$cx\runtime\virtualdesktop-openxr.json"
New-ItemProperty -Path "$reg\AvailableRuntimes" -Name "$cx\runtime\virtualdesktop-openxr.json" -Value 0 -PropertyType DWord -Force | Out-Null
if (Test-Path "$inst\config\session.json") { Remove-Item "$inst\config\session.json" }
& $ps @common | Out-Null
Check 'takeover_active_runtime_is_ours' ((Active) -eq $manifest) (Active)
Check 'takeover_previous_from_clearxr' ((Saved) -eq 'C:\fake\before_clearxr.json') (Saved)
Check 'takeover_old_entry_removed' ($null -eq (Get-ItemProperty "$reg\AvailableRuntimes" -Name "$cx\runtime\virtualdesktop-openxr.json" -ErrorAction SilentlyContinue))
Check 'takeover_legacy_state_removed' (-not (Test-Path $legacyState))
Check 'takeover_config_copied' ((Test-Path "$inst\config\session.json") -and (Test-Path "$inst\config\visionalvr_views.json"))
Check 'takeover_old_files_removed' (-not (Test-Path $cx))
& $ps @common -Unregister | Out-Null
Check 'takeover_unregister_restores_pre_clearxr' ((Active) -eq 'C:\fake\before_clearxr.json') (Active)
Check 'sibling_runtime_kept_at_end' ($null -ne (Get-ItemProperty "$reg\AvailableRuntimes" -Name 'C:\fake\sibling_runtime.json' -ErrorAction SilentlyContinue))

Remove-Item $testRoot -Recurse -Force
Remove-Item $tmp -Recurse -Force
Write-Host "RESULT: $(if ($fail -eq 0) { 'PASS' } else { "FAIL ($fail)" })"

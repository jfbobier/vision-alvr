# Idempotently applies the VisionALVR patches to the builder's ALVR v20.14.1 checkout:
#   loopback-client.patch   mock client / harness loopback
#   client-stats-log.patch  server_core logs the headset's raw per-frame statistics (host: client_frames.csv)
$ErrorActionPreference = 'Continue'
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
Set-Location "$root\src\ALVR-v20.14.1"
$ok = $true
foreach ($name in 'loopback-client.patch', 'client-stats-log.patch') {
    $patch = "$root\_tmp\$name"
    if (-not (Test-Path $patch)) { Write-Host "patch: $name missing in _tmp"; $ok = $false; continue }
    git apply --reverse --check $patch 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Host "patch: $name already applied"; continue }
    git apply --check $patch
    if ($LASTEXITCODE -ne 0) { Write-Host "patch: $name DOES NOT APPLY"; $ok = $false; continue }
    git apply $patch
    Write-Host "patch: $name applied"
}
if (-not $ok) { exit 1 }

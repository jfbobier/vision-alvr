# Idempotently applies tools/patches/loopback-client.patch to the builder's ALVR v20.14.1 checkout.
$ErrorActionPreference = 'Continue'
$root = "$env:USERPROFILE\openxr"
Set-Location "$root\src\ALVR-v20.14.1"
$patch = "$root\_tmp\loopback-client.patch"
git apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -eq 0) { Write-Host "patch: already applied"; exit 0 }
git apply --check $patch
if ($LASTEXITCODE -ne 0) { Write-Host "patch: DOES NOT APPLY"; exit 1 }
git apply $patch
Write-Host "patch: applied"

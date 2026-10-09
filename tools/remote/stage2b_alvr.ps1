$ErrorActionPreference = 'Continue'
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$env:Path = [System.Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [System.Environment]::GetEnvironmentVariable('Path','User') + ';C:\Program Files\LLVM\bin'
Set-Location "$root\src\ALVR-v20.14.1"
Write-Host "=== openvr submodule (shallow)"
git submodule update --init --depth 1 openvr 2>&1 | Select-Object -Last 4
git submodule status
Write-Host "=== cargo build alvr_server_core --release"
cargo build -p alvr_server_core --release 2>&1 | Tee-Object "$root\logs\alvr_server_core_build.log" | Select-Object -Last 15
Get-ChildItem target\release -Filter 'alvr_server_core*' -ErrorAction SilentlyContinue | Select-Object Name, Length

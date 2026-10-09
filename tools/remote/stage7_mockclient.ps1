# Builds the headless mock client inside the builder's ALVR v20.14.1 checkout (workspace member via alvr/* glob).
$ErrorActionPreference = 'Continue'
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$dst  = "$root\src\ALVR-v20.14.1\alvr\client_headless"
New-Item -ItemType Directory -Force "$dst\src" | Out-Null
Copy-Item "$root\_tmp\mock_client\Cargo.toml" "$dst\Cargo.toml" -Force
Copy-Item "$root\_tmp\mock_client\src\main.rs" "$dst\src\main.rs" -Force
Set-Location "$root\src\ALVR-v20.14.1"
$env:PATH += ";$env:USERPROFILE\.cargo\bin"
cargo build --release -p alvr_client_headless 2>&1 | Out-File -Encoding utf8 "$root\logs\mockclient_build.log"
Get-Content "$root\logs\mockclient_build.log" | Select-String 'error|warning: unused|Finished|could not' | Select-Object -First 30
Get-Item "$root\src\ALVR-v20.14.1\target\release\alvr_client_headless.exe" -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime

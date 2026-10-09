# Builds alvr_host inside the builder's ALVR v20.14.1 checkout (workspace member via alvr/* glob).
$ErrorActionPreference = 'Continue'
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$dst  = "$root\src\ALVR-v20.14.1\alvr\host_harness"
New-Item -ItemType Directory -Force "$dst\src" | Out-Null
Copy-Item "$root\_tmp\host\Cargo.toml" "$dst\Cargo.toml" -Force
Copy-Item "$root\_tmp\host\src\*.rs" "$dst\src\" -Force
Copy-Item "$root\_tmp\host\build.rs" "$dst\build.rs" -Force
Set-Location "$root\src\ALVR-v20.14.1"
$env:PATH += ";$env:USERPROFILE\.cargo\bin"
cargo build --release -p alvr_host 2>&1 | Out-File -Encoding utf8 "$root\logs\host_build.log"
Get-Content "$root\logs\host_build.log" | Select-String -Pattern '^error|^warning: unused|Finished|could not' -Context 0,6 | Select-Object -First 40
Get-Item "$root\src\ALVR-v20.14.1\target\release\alvr_host.exe" -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime

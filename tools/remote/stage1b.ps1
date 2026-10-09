$ErrorActionPreference = 'Continue'
$env:Path = [System.Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [System.Environment]::GetEnvironmentVariable('Path','User') + ';C:\Program Files\LLVM\bin'
Set-Location "$env:USERPROFILE\openxr\src"

if (Test-Path VDXR) { Write-Host "VDXR exists; updating submodules"; } else {
  git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/mbucchia/VirtualDesktop-OpenXR.git VDXR 2>&1 | Select-Object -Last 8
}
Write-Host "== VDXR"
git -C VDXR rev-parse --short HEAD
git -C VDXR submodule update --init --recursive --depth 1 2>&1 | Select-Object -Last 8
git -C VDXR submodule status

Write-Host "== tools"
clang --version 2>&1 | Select-Object -First 1
rustc --version; cargo --version; cmake --version | Select-Object -First 1; nuget help | Select-Object -First 1
Write-Host "== ALVR"
git -C ALVR-v20.14.1 rev-parse --short HEAD

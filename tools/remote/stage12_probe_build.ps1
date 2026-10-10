# Builds tools/probe/xr_probe.cpp -> build\probe\xr_probe.exe (no run). Includes nvenc\hostlib\bench_scene.h (XR_PROBE_SCENE) and stb_image.
$ErrorActionPreference = 'Continue'
$start = Get-Date
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$sdk  = "$root\src\VDXR\external\OpenXR-SDK\include"
$lib  = "$root\build\oxr-sdk\src\loader\Release\openxr_loader.lib"
$bdir = "$root\build\probe"
New-Item -ItemType Directory -Force $bdir | Out-Null
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
@"
@echo off
call "$vcvars" >nul
cd /d "$bdir"
cl /nologo /EHsc /MD /O2 /std:c++17 /I"$sdk" /I"$root\nvenc\hostlib" /I"$root\third_party\stb" "$root\_tmp\xr_probe.cpp" /Fe:xr_probe.exe /link "$lib" d3d11.lib dxgi.lib d3dcompiler.lib advapi32.lib
"@ | Set-Content -Encoding ASCII "$bdir\build.cmd"
cmd /c "$bdir\build.cmd" 2>&1 | Out-File -Encoding utf8 "$root\logs\xr_probe_build.log"
Get-Content "$root\logs\xr_probe_build.log" | Select-String -Pattern 'error|fatal' | Select-Object -First 10
$exe = Get-Item "$bdir\xr_probe.exe" -ErrorAction SilentlyContinue
if ($exe -and $exe.LastWriteTime -ge $start) { Write-Host "RESULT: OK $($exe.FullName) $($exe.Length)" } else { Write-Host "RESULT: FAILED (exe not rebuilt; see logs\xr_probe_build.log)" }

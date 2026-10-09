$ErrorActionPreference = 'Continue'
# Builds tools/probe/xr_probe.cpp on the builder, then runs it against VDXR+OVRNull at LIMITED integrity
# via a temporary scheduled task (see stage4c). PASS/FAIL is taken from the probe's own log + exit code.
$root = "$env:USERPROFILE\openxr"
$sdk  = "$root\src\VDXR\external\OpenXR-SDK\include"
$lib  = "$root\build\oxr-sdk\src\loader\Release\openxr_loader.lib"
$rt   = "$root\out\vdxr-null"
$bdir = "$root\build\probe"
$log  = "$root\logs\xr_probe.log"
$rcf  = "$root\logs\xr_probe.rc"
$cmd  = "$root\out\probe_run.cmd"
$task = 'openxr_probe_tmp'
New-Item -ItemType Directory -Force $bdir, "$root\logs" | Out-Null

# --- build
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$src = "$root\_tmp\xr_probe.cpp"
$bld = "$bdir\build.cmd"
@"
@echo off
call "$vcvars" >nul
cd /d "$bdir"
cl /nologo /EHsc /MD /O2 /std:c++17 /I"$sdk" "$src" /Fe:xr_probe.exe /link "$lib" d3d11.lib dxgi.lib advapi32.lib
"@ | Set-Content -Encoding ASCII $bld
cmd /c $bld 2>&1 | Tee-Object "$root\logs\xr_probe_build.log" | Select-Object -Last 25
if (-not (Test-Path "$bdir\xr_probe.exe")) { Write-Host "RESULT: BLOCKED (build failed)"; exit 1 }

# --- run
Set-Content $log ''; Set-Content $rcf 'pending'
@"
@echo off
set XR_RUNTIME_JSON=$rt\virtualdesktop-openxr.json
cd /d $bdir
xr_probe.exe "$log" 120 30
echo %ERRORLEVEL% > "$rcf"
"@ | Set-Content -Encoding ASCII $cmd
schtasks /create /tn $task /tr "cmd /c `"$cmd`"" /sc once /st 00:00 /rl LIMITED /it /f | Out-Null
schtasks /run /tn $task | Out-Null
for ($i = 0; $i -lt 50 -and (Get-Content $rcf).Trim() -eq 'pending'; $i++) { Start-Sleep -Seconds 1 }
if ((Get-Content $rcf).Trim() -eq 'pending') { Get-Process xr_probe -ErrorAction SilentlyContinue | Stop-Process -Force; Write-Host "probe did not finish in 50 s -> killed" }
schtasks /delete /tn $task /f | Out-Null
Write-Host "=== probe log"
Get-Content $log -ErrorAction SilentlyContinue
Write-Host "=== exit code: $(if (Test-Path $rcf) { (Get-Content $rcf).Trim() } else { 'none' })"
Write-Host "=== VDXR log tail"
Get-Content "$env:LOCALAPPDATA\VirtualDesktop-OpenXR\OpenXR.log" -Tail 25 -ErrorAction SilentlyContinue

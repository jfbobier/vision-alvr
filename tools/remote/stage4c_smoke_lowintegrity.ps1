$ErrorActionPreference = 'Continue'
# Runs a command in the logged-on user's interactive session at MEDIUM integrity (not elevated),
# via a temporary scheduled task that this script creates and removes. Needed because the OpenXR
# loader ignores XR_RUNTIME_JSON for elevated processes and SSH sessions hold a full admin token.
$root  = "$env:USERPROFILE\openxr"
$rt    = "$root\out\vdxr-null"
$hxdir = "$root\build\oxr-sdk\src\tests\hello_xr\Release"
$cmd   = "$root\out\smoke_run.cmd"
$out   = "$root\logs\smoke4_out.txt"
$task  = 'openxr_smoke_tmp'

@"
@echo off
set XR_RUNTIME_JSON=$rt\virtualdesktop-openxr.json
cd /d $hxdir
hello_xr.exe -G D3D11 -ff Hmd -vc Stereo -bm Opaque > "$out" 2>&1
"@ | Set-Content -Encoding ASCII $cmd
if (Test-Path $out) { Clear-Content $out }

schtasks /create /tn $task /tr "cmd /c `"$cmd`"" /sc once /st 00:00 /rl LIMITED /it /f
schtasks /run /tn $task
Start-Sleep -Seconds 40
$p = Get-Process hello_xr -ErrorAction SilentlyContinue
if ($p) { Write-Host "hello_xr still running (pid $($p.Id), session $($p.SessionId)) -> stopping"; $p | Stop-Process -Force } else { Write-Host "hello_xr not running" }
schtasks /delete /tn $task /f
Write-Host "=== output (non-verbose)"
Get-Content $out -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch '^Verbose' } | Select-Object -First 80
Write-Host "=== VDXR/OVRNull logs"
Get-ChildItem $env:LOCALAPPDATA -Recurse -Include '*OpenXR*.log','*OVR*.log','*VirtualDesktop*' -Depth 3 -ErrorAction SilentlyContinue |
  Select-Object FullName, Length, LastWriteTime
Write-Host "=== VDXR log"
Get-Content "$env:LOCALAPPDATA\VirtualDesktop-OpenXR\OpenXR.log" -Tail 40

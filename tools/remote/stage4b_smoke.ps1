$ErrorActionPreference = 'Continue'
$root = "$env:USERPROFILE\openxr"
$rt   = "$root\out\vdxr-null"
New-Item -ItemType Directory -Force $rt | Out-Null
Copy-Item "$root\src\VDXR\bin\x64\Release\virtualdesktop-openxr.dll"  $rt -Force
Copy-Item "$root\src\VDXR\bin\x64\Release\virtualdesktop-openxr.json" $rt -Force
Copy-Item "$root\src\VDXR\OVRNull\bin\x64\Release\LibOVRRT64_1.dll"   $rt -Force
Get-ChildItem $rt | Select-Object Name, Length

$hx   = "$root\build\oxr-sdk\src\tests\hello_xr\Release\hello_xr.exe"
$ldr  = Get-ChildItem "$root\build\oxr-sdk" -Recurse -Filter openxr_loader.dll | Select-Object -First 1
Write-Host "hello_xr: $hx"; Write-Host "loader  : $($ldr.FullName)"
Write-Host "VirtualDesktop.Server running? " ([bool](Get-Process VirtualDesktop.Server -ErrorAction SilentlyContinue))
Write-Host "Session: $((Get-Process -Id $PID).SessionId)  (0 = non-interactive service session)"

$env:XR_RUNTIME_JSON    = "$rt\virtualdesktop-openxr.json"
$env:XR_LOADER_DEBUG    = 'all'
$out = "$root\logs\smoke_stdout.txt"; $err = "$root\logs\smoke_stderr.txt"
$p = Start-Process -FilePath $hx -ArgumentList '-G','D3D11','-ff','Hmd','-vc','Stereo','-bm','Opaque' `
      -WorkingDirectory (Split-Path $hx) -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 25
if (-not $p.HasExited) { Write-Host "hello_xr still running after 25 s -> stopping it"; Stop-Process -Id $p.Id -Force } else { Write-Host "hello_xr exited, code $($p.ExitCode)" }

Write-Host "=== stdout (last 60)"; Get-Content $out -Tail 60
Write-Host "=== stderr (last 40)"; Get-Content $err -Tail 40
Write-Host "=== VDXR logs"
Get-ChildItem "$env:LOCALAPPDATA" -Recurse -Filter '*OpenXR*.log' -Depth 2 -ErrorAction SilentlyContinue | Select-Object FullName, Length, LastWriteTime

param([string]$Sub = 'all')
# Builds nvenc/ (unmodified ALVR NVENC files + shims) on the builder and runs the standalone encode tests.
$ErrorActionPreference = 'Continue'
$root = "$env:USERPROFILE\openxr"
$n = "$root\nvenc"
$bd = "$root\build\nvenc"
New-Item -ItemType Directory -Force $bd, "$root\logs", "$root\out\nvenc" | Out-Null
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$u = "$n\upstream"
$srcs = "$n\nvenc_test.cpp $n\shim\shim.cpp $u\platform\win32\VideoEncoderNVENC.cpp $u\platform\win32\NvEncoder.cpp $u\platform\win32\NvEncoderD3D11.cpp $u\platform\win32\shared\d3drender.cpp $u\ALVR-common\exception.cpp"
@"
@echo off
call "$vcvars" >nul
cd /d "$bd"
cl /nologo /EHsc /MD /O2 /std:c++17 /DNOMINMAX /I"$n\shim" /I"$u" /I"$u\platform\win32" /I"$n" $srcs /Fe:nvenc_test.exe /link d3d11.lib dxgi.lib user32.lib advapi32.lib
"@ | Set-Content -Encoding ASCII "$bd\build.cmd"
cmd /c "$bd\build.cmd" 2>&1 | Out-File -Encoding utf8 "$root\logs\nvenc_build.log"
Get-Content "$root\logs\nvenc_build.log" | Select-String -Pattern 'error|fatal' | Select-Object -First 25
if (-not (Test-Path "$bd\nvenc_test.exe") -or (Get-Item "$bd\nvenc_test.exe").LastWriteTime -lt (Get-Date).AddMinutes(-3)) { Write-Host "RESULT: BLOCKED (build failed)"; exit 1 }
# Patched variant: ABGR10 -> R10G10B10A2 (build-time copy; upstream file stays untouched)
$pat = "$bd\NvEncoderD3D11_fix10.cpp"
$txt = Get-Content "$u\platform\win32\NvEncoderD3D11.cpp" -Raw
$new = $txt -replace '(case NV_ENC_BUFFER_FORMAT_ABGR10:\s*return )DXGI_FORMAT_R8G8B8A8_UNORM;', '$1DXGI_FORMAT_R10G10B10A2_UNORM;'
if ($new -eq $txt) { Write-Host "patch did not apply"; exit 1 }
[IO.File]::WriteAllText($pat, $new)
$srcs2 = $srcs.Replace("$u\platform\win32\NvEncoderD3D11.cpp", $pat)
@"
@echo off
call "$vcvars" >nul
cd /d "$bd"
cl /nologo /EHsc /MD /O2 /std:c++17 /DNOMINMAX /I"$n\shim" /I"$u" /I"$u\platform\win32" /I"$n" $srcs2 /Fe:nvenc_test_fix10.exe /link d3d11.lib dxgi.lib user32.lib advapi32.lib
"@ | Set-Content -Encoding ASCII "$bd\build2.cmd"
cmd /c "$bd\build2.cmd" 2>&1 | Out-File -Encoding utf8 "$root\logs\nvenc_build2.log"
if (-not (Test-Path "$bd\nvenc_test_fix10.exe")) { Write-Host "RESULT: BLOCKED (patched build failed)"; exit 1 }
Write-Host "build ok"
$o = "$root\out\nvenc"
foreach ($cfg in @(
  @{n='hevc10_250'; a='--bits 10 --mbps 250'},
  @{n='hevc8_250';  a='--bits 8 --mbps 250'},
  @{n='hevc10_250_fix10'; a='--bits 10 --mbps 250'; exe='nvenc_test_fix10.exe'},
  @{n='hevc10_30_fix10'; a='--bits 10 --mbps 30'; exe='nvenc_test_fix10.exe'},
  @{n='hevc10_60_fix10'; a='--bits 10 --mbps 60'; exe='nvenc_test_fix10.exe'}
)) {
  $exe = if ($cfg.exe) { $cfg.exe } else { 'nvenc_test.exe' }
  Write-Host "=== $($cfg.n)"
  & "$bd\$exe" --out "$o\$($cfg.n).hevc" --frames 120 $cfg.a.Split(' ') 2>&1
  Write-Host "exit $LASTEXITCODE"
}

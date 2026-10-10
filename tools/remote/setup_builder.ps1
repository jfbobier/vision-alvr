# One-time setup of a builder (the Windows PC that builds and runs VisionALVR; see docs/DEVELOPMENT.md). Idempotent:
#  1. checks the toolchain (git, rustup/cargo, cmake, clang, nuget, Visual Studio 2022 with C++),
#  2. clones ALVR v20.14.1 and VirtualDesktop-OpenXR at the commits pinned in deps.lock.json (<root>\_tmp\deps.lock.json)
#     into <Src> (default <root>\src; root = $env:VISIONALVR_ROOT or %USERPROFILE%\openxr) and verifies them (existing clones are verified, never changed),
#  3. builds what the later stages expect: VDXR (unpatched, + out\vdxr-null), the OpenXR loader (build\oxr-sdk), ALVR's
#     server_core once (fetches the crates).
# -CloneOnly stops after step 2 (used to test the clone step into another folder).
param([string]$Src = '', [switch]$CloneOnly)
$ErrorActionPreference = 'Continue'
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
if (-not $Src) { $Src = "$root\src" }
$env:Path = [System.Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [System.Environment]::GetEnvironmentVariable('Path','User') + ';C:\Program Files\LLVM\bin'
$fails = 0
function Step($name, $ok, $detail) { if (-not $ok) { $script:fails++ }; Write-Host ("SETUP {0} {1}: {2}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $name, $detail) }
New-Item -ItemType Directory -Force $Src, "$root\logs", "$root\out", "$root\build" | Out-Null

# ---- 1. toolchain
$vs = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
foreach ($t in @(@('git', 'git --version'), @('cargo', 'cargo --version'), @('cmake', 'cmake --version'), @('clang', 'clang --version'), @('nuget', 'nuget help'))) {
  $v = (cmd /c "$($t[1]) 2>&1" | Select-Object -First 1)
  Step "tool_$($t[0])" ($LASTEXITCODE -eq 0 -or $v) "$v"
}
Step 'visual_studio_2022' (Test-Path "$vs\MSBuild\Current\Bin\MSBuild.exe") "$vs (Desktop development with C++)"
Step 'dotnet_framework_48' (Test-Path 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\System.Windows.Forms.dll') 'WinForms assemblies (GUIs)'

# ---- 2. pinned sources
$lock = Get-Content "$root\_tmp\deps.lock.json" -Raw | ConvertFrom-Json
$alvr = $lock.builder_sources.'ALVR-v20.14.1'
$vdxr = $lock.builder_sources.VDXR
function Head($dir) { (git -C $dir rev-parse HEAD 2>$null) }
$a = "$Src\ALVR-v20.14.1"
if (-not (Test-Path "$a\.git")) {
  git clone --depth 1 --branch $alvr.tag https://github.com/alvr-org/ALVR.git $a 2>&1 | Select-Object -Last 2
}
Step 'alvr_commit' ((Head $a) -eq $alvr.full_hash) "$(Head $a) (pinned $($alvr.full_hash), tag $($alvr.tag))"
git -C $a submodule update --init --depth 1 openvr 2>&1 | Select-Object -Last 2
$ov = (git -C "$a\openvr" rev-parse HEAD 2>$null)
Step 'alvr_openvr_submodule' ($ov -eq $alvr.submodules.openvr) "$ov"
$v = "$Src\VDXR"
if (-not (Test-Path "$v\.git")) {
  git init -q $v
  git -C $v remote add origin $vdxr.fork   # VisionALVR fork (deps.lock.json): upstream is mbucchia/VirtualDesktop-OpenXR
  git -C $v fetch -q --depth 1 origin $vdxr.full_hash 2>&1 | Select-Object -Last 2
  git -C $v checkout -q FETCH_HEAD 2>&1 | Select-Object -Last 2
}
Step 'vdxr_commit' ((Head $v) -eq $vdxr.full_hash) "$(Head $v) (pinned $($vdxr.full_hash))"
git -C $v submodule update --init --recursive --depth 1 2>&1 | Select-Object -Last 3
foreach ($p in $vdxr.submodules.PSObject.Properties) {
  $h = (git -C "$v\$($p.Name)" rev-parse HEAD 2>$null)
  Step "vdxr_$($p.Name -replace '^external/', '')" ($h -eq $p.Value) "$h"
}
if ($CloneOnly) { if ($fails -eq 0) { Write-Host 'RESULT: OK (clone only)' } else { Write-Host "RESULT: FAILED ($fails)" }; exit }

# ---- 3. prerequisites of the later stages
$msbuild = "$vs\MSBuild\Current\Bin\MSBuild.exe"
Set-Location $v
nuget restore VirtualDesktop-OpenXR.sln 2>&1 | Out-File -Encoding utf8 "$root\logs\setup_nuget.log"
& $msbuild virtualdesktop-openxr\virtualdesktop-openxr.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$v\\" /m /v:minimal /nologo 2>&1 |
  Out-File -Encoding utf8 "$root\logs\setup_vdxr.log"
$vd = "$v\bin\x64\Release\virtualdesktop-openxr.dll"
Step 'vdxr_build' (Test-Path $vd) 'logs\setup_vdxr.log'
New-Item -ItemType Directory -Force "$root\out\vdxr-null" | Out-Null
foreach ($f in 'virtualdesktop-openxr.dll', 'virtualdesktop-openxr.json') { Copy-Item "$v\bin\x64\Release\$f" "$root\out\vdxr-null" -Force -ErrorAction SilentlyContinue }
Step 'vdxr_runtime_manifest' (Test-Path "$root\out\vdxr-null\virtualdesktop-openxr.json") 'out\vdxr-null'
$bld = "$root\build\oxr-sdk"
cmake -S "$v\external\OpenXR-SDK-Source" -B $bld -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTS=ON -DBUILD_CONFORMANCE_TESTS=OFF 2>&1 | Out-File -Encoding utf8 "$root\logs\setup_oxr_sdk.log"
cmake --build $bld --config Release --target openxr_loader hello_xr -j 2>&1 | Out-File -Encoding utf8 -Append "$root\logs\setup_oxr_sdk.log"
Step 'openxr_loader' (Test-Path "$bld\src\loader\Release\openxr_loader.lib") 'logs\setup_oxr_sdk.log'
Set-Location $a
cargo build -p alvr_server_core --release 2>&1 | Out-File -Encoding utf8 "$root\logs\setup_server_core.log"
Step 'alvr_server_core' (Test-Path "$a\target\release\alvr_server_core.dll") 'logs\setup_server_core.log'
if ($fails -eq 0) { Write-Host 'RESULT: OK' } else { Write-Host "RESULT: FAILED ($fails)" }

<#
Builds VisionALVR on Windows from this checkout (see docs/DEVELOPMENT.md for the prerequisites):

    powershell -ExecutionPolicy Bypass -File build.ps1                 # everything for the release zip
    powershell -ExecutionPolicy Bypass -File build.ps1 host gui        # only some parts (setup runs first if needed)

Targets, in build order: vdxr host shim gui package (default: all five), plus client probe (test tools for the harness).
The first run clones ALVR v20.14.1 and VirtualDesktop-OpenXR at the commits in deps.lock.json into src\ and builds their
prerequisites (tools\remote\setup_builder.ps1, idempotent). Output: out\dist\VisionALVR\ and out\dist\VisionALVR-<version>.zip.
Logs: logs\build_<step>.txt. Nothing outside this folder is changed (no registry, no OpenXR runtime registration).
#>
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Targets)
$ErrorActionPreference = 'Continue'
$repo = $PSScriptRoot
$env:VISIONALVR_ROOT = $repo          # the build scripts work in <root>\src, <root>\out, <root>\_tmp, <root>\logs
$all = @('vdxr', 'host', 'shim', 'gui', 'package')
if (-not $Targets -or $Targets.Count -eq 0) { $Targets = $all }
$known = $all + @('client', 'probe')
foreach ($t in $Targets) { if ($known -notcontains $t) { Write-Host "unknown target '$t' (known: $($known -join ', '))"; exit 3 } }
New-Item -ItemType Directory -Force "$repo\_tmp", "$repo\logs", "$repo\bench" | Out-Null

function Stage($name, $script, $ok, [string[]]$a = @()) {
    Write-Host "== $name"
    $log = "$repo\logs\build_$name.txt"
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\remote\$script" @a 2>&1 | Out-File -Encoding utf8 $log
    $text = Get-Content $log -Raw
    if ($text -notmatch $ok) {
        Write-Host "FAILED: $name (log: $log)"
        Get-Content $log | Select-Object -Last 25 | ForEach-Object { Write-Host "   $_" }
        exit 2
    }
    Write-Host "   ok"
}
function Put($from, $to) { New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null; Copy-Item $from $to -Force }

# ---- the inputs the build scripts read from _tmp (the harness uploads the same files over SSH)
Put "$repo\deps.lock.json" "$repo\_tmp\deps.lock.json"
Put "$repo\tools\patches\loopback-client.patch" "$repo\_tmp\loopback-client.patch"
Put "$repo\tools\patches\client-stats-log.patch" "$repo\_tmp\client-stats-log.patch"
Put "$repo\tools\host\Cargo.toml" "$repo\_tmp\host\Cargo.toml"
Put "$repo\tools\host\build.rs" "$repo\_tmp\host\build.rs"
Get-ChildItem "$repo\tools\host\src\*.rs" | ForEach-Object { Put $_.FullName "$repo\_tmp\host\src\$($_.Name)" }
Put "$repo\tools\mock_client\Cargo.toml" "$repo\_tmp\mock_client\Cargo.toml"
Put "$repo\tools\mock_client\src\main.rs" "$repo\_tmp\mock_client\src\main.rs"
Get-ChildItem "$repo\ovrshim" -File | ForEach-Object { Put $_.FullName "$repo\_tmp\ovrshim\$($_.Name)" }
foreach ($f in 'Common.cs', 'VisionALVR.cs', 'Configure.cs', 'app.manifest') { Put "$repo\tools\gui\$f" "$repo\_tmp\gui\$f" }
foreach ($f in 'visionalvr.ico', 'logo_640.png') { Put "$repo\tools\gui\res\$f" "$repo\_tmp\gui\res\$f" }
foreach ($f in 'register_openxr_runtime.ps1', 'register_openxr_runtime.bat', 'unregister_openxr_runtime.bat', 'session.default.json', 'README.txt') {
    Put "$repo\tools\install\$f" "$repo\_tmp\install\$f"
}
Put "$repo\THIRD_PARTY_NOTICES.md" "$repo\_tmp\install\THIRD_PARTY_NOTICES.md"
Put "$repo\LICENSE" "$repo\_tmp\install\LICENSE.txt"
Get-ChildItem "$repo\third_party\licenses\*.txt" | ForEach-Object { Put $_.FullName "$repo\_tmp\install\licenses\$($_.Name)" }
Put "$repo\tools\probe\xr_probe.cpp" "$repo\_tmp\xr_probe.cpp"

# ---- the benchmark scene: converted in Linux by tools\bench_scene\build.sh, else taken from the published release (verified)
$vab = "$repo\bench\littlest_tokyo.vab"
$vabSha = 'c2fe8ebe167282079adea9088c90a99e29a28d9f893b80c599e128be3f051a93'
if (Test-Path "$repo\build\bench\littlest_tokyo.vab") { Copy-Item "$repo\build\bench\littlest_tokyo.vab" $vab -Force }
if (-not (Test-Path $vab) -or (Get-FileHash $vab -Algorithm SHA256).Hash.ToLower() -ne $vabSha) {
    Write-Host "== bench scene (from the v0.1.0-alpha release)"
    $zip = "$repo\_tmp\release.zip"
    Invoke-WebRequest -UseBasicParsing 'https://github.com/jfbobier/vision-alvr/releases/download/v0.1.0-alpha/VisionALVR-0.1-alpha.zip' -OutFile $zip
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    $e = $z.Entries | Where-Object { $_.FullName -eq 'VisionALVR/bench/littlest_tokyo.vab' }
    if ($e) { [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $vab, $true) }
    $z.Dispose()
    if (-not (Test-Path $vab) -or (Get-FileHash $vab -Algorithm SHA256).Hash.ToLower() -ne $vabSha) {
        Write-Host "FAILED: could not get bench\littlest_tokyo.vab with the expected SHA-256"; exit 2
    }
    Write-Host "   ok"
}

# ---- pinned sources + prerequisites (skipped once done: the clones and the VDXR/OpenXR/server_core builds exist)
$ready = (Test-Path "$repo\src\ALVR-v20.14.1\target\release\alvr_server_core.dll") -and (Test-Path "$repo\out\vdxr-null\virtualdesktop-openxr.json") -and
         (Test-Path "$repo\build\oxr-sdk\src\loader\Release\openxr_loader.lib")
if (-not $ready) { Stage 'setup' 'setup_builder.ps1' 'RESULT: OK' }

# ---- the builds, in dependency order
if ($Targets -contains 'host' -or $Targets -contains 'client') { Stage 'alvr_patch' 'stage9_patch.ps1' 'applied' }
if ($Targets -contains 'vdxr')    { Stage 'vdxr'    'stage15_vdxr.ps1'       'RESULT: OK' }
if ($Targets -contains 'host')    { Stage 'host'    'stage8_host.ps1'        'Finished' }
if ($Targets -contains 'client')  { Stage 'client'  'stage7_mockclient.ps1'  'Finished' }
if ($Targets -contains 'shim')    { Stage 'shim'    'stage11_ovrshim.ps1'    'RESULT: OK' }
if ($Targets -contains 'probe')   { Stage 'probe'   'stage12_probe_build.ps1' 'RESULT: OK' }
if ($Targets -contains 'gui')     { Stage 'gui'     'stage16_gui.ps1'        'RESULT: OK' }
if ($Targets -contains 'package') { Stage 'package' 'stage13_package.ps1'    'RESULT: OK' }
Write-Host "RESULT: OK ($($Targets -join ' '))"
if ($Targets -contains 'package') { Write-Host "package: $repo\out\dist\VisionALVR (+ zip)" }

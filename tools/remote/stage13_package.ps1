# Assembles the portable VisionALVR folder out\VisionALVR (what a user unzips anywhere) and out\VisionALVR-<version>.zip:
# alvr_host.exe, the OpenXR runtime (VDXR + OVRShim), the two GUIs, the register scripts, config\session.default.json,
# bench\littlest_tokyo.vab (the benchmark scene).
$ErrorActionPreference = 'Continue'
$root = "$env:USERPROFILE\openxr"
$version = '0.1-alpha'
$pkg = "$root\out\dist\VisionALVR"   # a folder no earlier layout used (out\visionalvr = the old installer package)
New-Item -ItemType Directory -Force $pkg, "$pkg\config" | Out-Null
Copy-Item "$root\out\vdxr-shim\*" $pkg -Force                 # virtualdesktop-openxr.dll / .json, LibOVRRT64_1.dll
Copy-Item "$root\src\ALVR-v20.14.1\target\release\alvr_host.exe" $pkg -Force
foreach ($f in 'VisionALVR.exe', 'configure.exe') { if (Test-Path "$root\out\gui\$f") { Copy-Item "$root\out\gui\$f" $pkg -Force } }
foreach ($f in 'register_openxr_runtime.ps1', 'register_openxr_runtime.bat', 'unregister_openxr_runtime.bat', 'README.txt') { Copy-Item "$root\_tmp\install\$f" $pkg -Force }
Copy-Item "$root\_tmp\install\session.default.json" "$pkg\config\session.default.json" -Force
# licences: notices, the project licence (if present) and the upstream texts
New-Item -ItemType Directory -Force "$pkg\licenses" | Out-Null
Copy-Item "$root\_tmp\install\THIRD_PARTY_NOTICES.md" $pkg -Force
if (Test-Path "$root\_tmp\install\LICENSE.txt") { Copy-Item "$root\_tmp\install\LICENSE.txt" $pkg -Force }
Copy-Item "$root\_tmp\install\licenses\*.txt" "$pkg\licenses" -Force
# the benchmark scene (tools/bench_scene/build.sh, uploaded by `run.py build host`)
New-Item -ItemType Directory -Force "$pkg\bench" | Out-Null
if (Test-Path "$root\bench\littlest_tokyo.vab") { Copy-Item "$root\bench\littlest_tokyo.vab" "$pkg\bench\littlest_tokyo.vab" -Force }
$zip = "$root\out\dist\VisionALVR-$version.zip"
# the zip holds only what ships (never logs\, config\session.json or results a test run may have left in the folder);
# everything sits under VisionALVR\ so it unzips to one folder. logs\ and the user's config files are created on first use.
$ship = @('alvr_host.exe', 'VisionALVR.exe', 'configure.exe', 'LibOVRRT64_1.dll', 'virtualdesktop-openxr.dll', 'virtualdesktop-openxr.json',
          'register_openxr_runtime.ps1', 'register_openxr_runtime.bat', 'unregister_openxr_runtime.bat', 'README.txt',
          'config\session.default.json', 'bench\littlest_tokyo.vab', 'THIRD_PARTY_NOTICES.md', 'LICENSE.txt')
$ship += Get-ChildItem "$pkg\licenses" -File | ForEach-Object { "licenses\$($_.Name)" }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$fs = [IO.File]::Open($zip, [IO.FileMode]::Create)
$za = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
foreach ($f in $ship) {
  if (Test-Path "$pkg\$f") { [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, "$pkg\$f", ('VisionALVR/' + $f.Replace('\', '/')), [IO.Compression.CompressionLevel]::Optimal) | Out-Null }
}
$za.Dispose(); $fs.Dispose()
Get-ChildItem $pkg -Recurse -File | Select-Object @{n='file';e={$_.FullName.Substring($pkg.Length + 1)}}, Length | Format-Table -AutoSize | Out-String -Width 200
$missing = @('alvr_host.exe', 'VisionALVR.exe', 'configure.exe', 'LibOVRRT64_1.dll', 'virtualdesktop-openxr.dll', 'virtualdesktop-openxr.json', 'bench\littlest_tokyo.vab', 'THIRD_PARTY_NOTICES.md', 'licenses\ALVR-LICENSE.txt') | Where-Object { -not (Test-Path "$pkg\$_") }
if ($missing) { Write-Host "RESULT: FAILED missing $($missing -join ', ')" } else { Write-Host "RESULT: OK $pkg ($zip)" }

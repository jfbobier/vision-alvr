$ErrorActionPreference = 'Continue'
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$env:Path = [System.Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [System.Environment]::GetEnvironmentVariable('Path','User')
Set-Location "$root\src\VDXR"
Write-Host "=== VDXR bin"
Get-ChildItem bin\x64\Release | Select-Object Name, Length
Write-Host "=== runtime manifests"
Get-ChildItem -Recurse -Filter '*.json' -Depth 3 | Where-Object { $_.FullName -notmatch 'external|obj|\.git' } | Select-Object -ExpandProperty FullName
Get-ChildItem -Recurse -Filter '*.json' -Depth 3 | Where-Object { $_.Name -match 'virtualdesktop|runtime' -and $_.FullName -notmatch 'external|obj' } | ForEach-Object { Write-Host "--- $($_.FullName)"; Get-Content $_.FullName }

Write-Host "=== cmake configure hello_xr"
$src = "$root\src\VDXR\external\OpenXR-SDK-Source"
$bld = "$root\build\oxr-sdk"
cmake -S $src -B $bld -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTS=ON -DBUILD_CONFORMANCE_TESTS=OFF 2>&1 | Select-Object -Last 8
Write-Host "=== cmake build hello_xr"
cmake --build $bld --config Release --target hello_xr -j 2>&1 | Tee-Object "$root\logs\helloxr_build.log" | Select-Object -Last 12
Get-ChildItem -Recurse $bld -Include hello_xr.exe, openxr_loader.dll | Select-Object FullName, Length

$ErrorActionPreference = 'Continue'
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$env:Path = [System.Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [System.Environment]::GetEnvironmentVariable('Path','User') + ';C:\Program Files\LLVM\bin'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
New-Item -ItemType Directory -Force "$root\logs" | Out-Null

Write-Host "=== VDXR: projects"
Set-Location "$root\src\VDXR"
Get-ChildItem -Recurse -Filter *.vcxproj -Depth 2 | Select-Object -ExpandProperty FullName

Write-Host "=== nuget restore"
nuget restore VirtualDesktop-OpenXR.sln 2>&1 | Select-Object -Last 6

Write-Host "=== msbuild OVRNull (Release x64)"
$proj = Get-ChildItem -Recurse -Filter OVRNull.vcxproj | Select-Object -First 1
if ($proj) {
  & $msbuild $proj.FullName /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo 2>&1 | Tee-Object "$root\logs\ovrnull_build.log" | Select-Object -Last 25
  Get-ChildItem -Recurse -Filter 'LibOVRRT64_1.dll' | Select-Object FullName, Length, LastWriteTime
} else { Write-Host "OVRNull.vcxproj not found" }

Write-Host "=== cargo build alvr_server_core (release)"
Set-Location "$root\src\ALVR-v20.14.1"
cargo build -p alvr_server_core --release 2>&1 | Tee-Object "$root\logs\alvr_server_core_build.log" | Select-Object -Last 15
Get-ChildItem target\release -Filter 'alvr_server_core*' -ErrorAction SilentlyContinue | Select-Object Name, Length
"STAGE2_DONE" | Out-File "$root\logs\stage2.done"

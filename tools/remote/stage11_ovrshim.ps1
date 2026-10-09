# Builds OVRShim (fork of VDXR's OVRNull with the host-driven driver) and assembles out\vdxr-shim.
$ErrorActionPreference = 'Continue'
$start = Get-Date
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$vdxr = "$root\src\VDXR"
$dst  = "$vdxr\OVRShim"
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
New-Item -ItemType Directory -Force $dst | Out-Null
# unchanged OVRNull sources
robocopy "$vdxr\OVRNull" $dst /E /XD bin obj /XF OVRNull.vcxproj OVRNull.vcxproj.user OVRNull.vcxproj.filters /NFL /NDL /NJH /NJS /NP | Out-Null
# our overrides / additions
Copy-Item "$root\_tmp\ovrshim\*" $dst -Force -Recurse
$proj = Get-Content "$vdxr\OVRNull\OVRNull.vcxproj" -Raw
# the project lists its shaders as FxCompile items: add ours by cloning the ReprojectVS/PS entries
$fxVs = [regex]::Match($proj, '<FxCompile Include="ReprojectVS.hlsl">.*?</FxCompile>', 'Singleline').Value
$fxPs = [regex]::Match($proj, '<FxCompile Include="ReprojectPS.hlsl">.*?</FxCompile>', 'Singleline').Value
if (-not $fxVs -or -not $fxPs) { Write-Host 'RESULT: BLOCKED (cannot find the FxCompile items)'; exit 1 }
$extra = $fxVs.Replace('ReprojectVS.hlsl', 'LayerVS.hlsl') + "`n    " + $fxPs.Replace('ReprojectPS.hlsl', 'LayerPS.hlsl') + "`n    " + $fxPs.Replace('ReprojectPS.hlsl', 'LayerCubePS.hlsl')
$proj = $proj.Replace($fxPs, $fxPs + "`n    " + $extra)
$proj = $proj.Replace('$(SolutionDir)\bin\', '$(SolutionDir)\bin-shim\').Replace('<ClInclude Include="driver.h" />', '<ClInclude Include="driver.h" /><ClInclude Include="ipc.h" /><ClInclude Include="ipc_win.h" />')
Set-Content "$dst\OVRShim.vcxproj" $proj -Encoding UTF8
Set-Location $vdxr
& $msbuild "OVRShim\OVRShim.vcxproj" /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$vdxr\\" /m /v:minimal /nologo 2>&1 |
  Out-File -Encoding utf8 "$root\logs\ovrshim_build.log"
Get-Content "$root\logs\ovrshim_build.log" | Select-String -Pattern 'error|warning C4|Build succeeded' | Select-Object -First 25
$dll = "$vdxr\bin-shim\x64\Release\LibOVRRT64_1.dll"
if ((Test-Path $dll) -and ((Get-Item $dll).LastWriteTime -ge $start)) {
  $out = "$root\out\vdxr-shim"
  New-Item -ItemType Directory -Force $out | Out-Null
  # the VDXR runtime DLL: the latest build (stage15 adds the cube-layer define), else the original build
  $vd = "$vdxr\bin\x64\Release\virtualdesktop-openxr.dll"
  if (-not (Test-Path $vd)) { $vd = "$root\out\vdxr-null\virtualdesktop-openxr.dll" }
  Copy-Item $vd $out -Force
  Copy-Item "$root\out\vdxr-null\virtualdesktop-openxr.json" $out -Force
  Copy-Item $dll $out -Force
  Get-Item "$out\LibOVRRT64_1.dll" | Select-Object FullName, Length, LastWriteTime
  Write-Host "RESULT: OK"
} else { Write-Host "RESULT: FAILED (the DLL was not rebuilt; see logs\ovrshim_build.log)" }

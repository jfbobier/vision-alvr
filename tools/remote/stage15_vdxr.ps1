# Rebuilds VDXR with the small OVRShim patches (text patches applied to the builder's VDXR checkout, idempotent):
#  1. frame.cpp: honor eyeVisibility on quad/cylinder/cube layers also in "Oculus runtime" mode (VDXR computes the per-eye
#     flags but only sets them when Virtual Desktop's own server is running; OVRShim implements the flags).
#  2. d3d11_native.cpp: copy all 6 faces (and mips) of a cube swapchain; upstream copies subresource 0 only.
#  3. -DHAS_CUBE_LAYERS (through the CL environment variable): advertise XR_KHR_composition_layer_cube.
# Everything else in VDXR is untouched. Unpatched sources: tools/remote/stage3_vdxr.ps1.
param([switch]$Unpatched)
$ErrorActionPreference = 'Continue'
$start = Get-Date
$root = if ($env:VISIONALVR_ROOT) { $env:VISIONALVR_ROOT } else { "$env:USERPROFILE\openxr" }
$src = "$root\src\VDXR\virtualdesktop-openxr"
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'

function Patch-File($path, $pattern, $replacement, $marker) {
    $txt = [IO.File]::ReadAllText($path)
    if ($txt.Contains($marker)) { Write-Host "already patched: $(Split-Path $path -Leaf) [$marker]"; return $true }
    $rx = [regex]::new($pattern, [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $rx.IsMatch($txt)) { Write-Host "PATCH FAILED (pattern not found) in $path [$marker]"; return $false }
    $txt = $rx.Replace($txt, $replacement, 1)
    [IO.File]::WriteAllText($path, $txt)
    Write-Host "patched: $(Split-Path $path -Leaf) [$marker]"
    return $true
}

$ok = $true
if (-not $Unpatched) {
    # 1. eyeVisibility (quad/cylinder block uses `quad.`, cube block uses `cube.`)
    foreach ($v in 'quad', 'cube') {
        $m = "OVRShim: eyeVisibility honored ($v)"
        $ok = (Patch-File "$src\frame.cpp" "($v\.eyeVisibility != XR_EYE_VISIBILITY_BOTH\)\s*\{\s*)if \(!m_useOculusRuntime\)" ('${1}if (true /* ' + $m + ' */)') $m) -and $ok
    }
    # 2. copy every face/mip of cube swapchains
    $pat = 'm_ovrSubmissionContext->CopySubresourceRegion\(\s*xrSwapchain\.resolvedSlices\[slice\]\.images\[ovrDestIndex\]\.Get\(\),\s*0,\s*0,\s*0,\s*0,\s*xrSwapchain\.appSwapchain\.images\[lastReleasedIndex\]\.Get\(\),\s*slice,\s*nullptr\);'
    $new = @'
if (xrSwapchain.ovrDesc.Type == ovrTexture_Cube) { /* OVRShim: copy every face and mip */
                    for (UINT face = 0; face < 6; face++) {
                        for (UINT mip = 0; mip < (UINT)xrSwapchain.ovrDesc.MipLevels; mip++) {
                            const UINT sub = mip + face * (UINT)xrSwapchain.ovrDesc.MipLevels;
                            m_ovrSubmissionContext->CopySubresourceRegion(
                                xrSwapchain.resolvedSlices[slice].images[ovrDestIndex].Get(), sub, 0, 0, 0,
                                xrSwapchain.appSwapchain.images[lastReleasedIndex].Get(), sub, nullptr);
                        }
                    }
                } else {
                    m_ovrSubmissionContext->CopySubresourceRegion(
                        xrSwapchain.resolvedSlices[slice].images[ovrDestIndex].Get(), 0, 0, 0, 0,
                        xrSwapchain.appSwapchain.images[lastReleasedIndex].Get(), slice, nullptr);
                }
'@
    $ok = (Patch-File "$src\d3d11_native.cpp" $pat $new.Replace('$', '$$') 'OVRShim: copy every face and mip') -and $ok
    $env:CL = '/DHAS_CUBE_LAYERS'
} else { $env:CL = '' }
if (-not $ok) { Write-Host "RESULT: FAILED (patch)"; exit 1 }

Set-Location "$root\src\VDXR"
& $msbuild virtualdesktop-openxr\virtualdesktop-openxr.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$root\src\VDXR\\" /m /v:minimal /nologo 2>&1 |
  Out-File -Encoding utf8 "$root\logs\vdxr_patched_build.log"
$env:CL = ''
$dll = "$root\src\VDXR\bin\x64\Release\virtualdesktop-openxr.dll"
if ((Test-Path $dll) -and ((Get-Item $dll).LastWriteTime -ge $start)) {
  Get-Item $dll | Select-Object FullName, Length, LastWriteTime
  Write-Host "RESULT: OK"
} else {
  Get-Content "$root\logs\vdxr_patched_build.log" | Select-String -Pattern 'error' | Select-Object -First 8
  Write-Host "RESULT: FAILED (see logs\vdxr_patched_build.log)"
}

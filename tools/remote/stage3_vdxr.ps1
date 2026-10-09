$ErrorActionPreference = 'Continue'
$root = "$env:USERPROFILE\openxr"
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
Set-Location "$root\src\VDXR"
Write-Host "=== msbuild virtualdesktop-openxr (Release x64)"
& $msbuild virtualdesktop-openxr\virtualdesktop-openxr.vcxproj /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$root\src\VDXR\\"/m /v:minimal /nologo 2>&1 |
  Tee-Object "$root\logs\vdxr_build.log" | Select-Object -Last 30
Get-ChildItem -Recurse -Include 'XR_APILAYER*','*openxr*.dll','*.json' -Path virtualdesktop-openxr\bin -ErrorAction SilentlyContinue |
  Select-Object FullName, Length

# Builds VisionALVR.exe and configure.exe (C# WinForms, .NET Framework 4.8 = part of Windows) with Visual Studio's Roslyn compiler
# against the framework's own assemblies (no SDK needed). Sources: _tmp\gui (uploaded by `run.py build gui`). Output: out\gui.
$ErrorActionPreference = 'Continue'
$root = "$env:USERPROFILE\openxr"
$src = "$root\_tmp\gui"
$out = "$root\out\gui"
$csc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\Roslyn\csc.exe'
$fw = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319'
New-Item -ItemType Directory -Force $out | Out-Null
Set-Content -Path "$out\build.log" -Value '' -Encoding UTF8
$refs = 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Windows.Forms.dll', 'System.Web.Extensions.dll' | ForEach-Object { "/r:$fw\$_" }
$common = @('/nologo', '/noconfig', '/target:winexe', '/platform:x64', '/optimize+', '/langversion:latest', '/nowarn:1701,1702',
            "/win32icon:$src\res\visionalvr.ico", "/win32manifest:$src\app.manifest", "/resource:$src\res\logo_640.png,logo_640.png") + $refs
$ok = $true
foreach ($t in @(@{ exe = 'VisionALVR.exe'; files = @("$src\Common.cs", "$src\VisionALVR.cs") }, @{ exe = 'configure.exe'; files = @("$src\Common.cs", "$src\Configure.cs") })) {
    $o = & $csc @common "/out:$out\$($t.exe)" @($t.files) 2>&1
    ($o | Out-String) | Add-Content -Path "$out\build.log" -Encoding UTF8
    $o | ForEach-Object { Write-Host ("$_" -replace "[^\x20-\x7E]", '?') }
    if ($LASTEXITCODE -ne 0) { $ok = $false; Write-Host "FAILED: $($t.exe)" } else { Write-Host "built $out\$($t.exe)" }
}
if ($ok) { Write-Host 'RESULT: OK' } else { Write-Host 'RESULT: FAILED' }

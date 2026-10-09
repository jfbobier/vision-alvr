# Syntax-checks the encoder sources against the vendored NVENC header and shows the compiler's own messages
# (the cc crate hides them).
$root = "$env:USERPROFILE\openxr"
$n = "$root\nvenc"; $u = "$n\upstream"
$out = (Get-ChildItem "$root\src\ALVR-v20.14.1\target\release\build\alvr_host-*\out\VideoEncoderNVENC_split.cpp" | Sort-Object LastWriteTime | Select-Object -Last 1).DirectoryName
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
@"
@echo off
call "$vcvars" >nul
cd /d "$root\logs"
cl /nologo /Zs /EHsc /MD /std:c++17 /DNOMINMAX /I"$out" /I"$n\shim" /I"$u" /I"$u\platform\win32" /I"$u\platform\win32\d3d-render-utils" /I"$n" /I"$n\hostlib" /I"$root\ovrshim" "$out\VideoEncoderNVENC_split.cpp" "$u\platform\win32\NvEncoder.cpp" "$u\platform\win32\NvEncoderD3D11.cpp"
"@ | Set-Content -Encoding ASCII "$root\logs\cc_check.cmd"
cmd /c "$root\logs\cc_check.cmd" 2>&1 | Out-File -Encoding utf8 "$root\logs\cc_check.log"
Get-Content "$root\logs\cc_check.log" | Select-String -Pattern 'error' | Select-Object -First 15

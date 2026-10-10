#!/bin/bash
# Compiles the two C# GUIs with the Windows Roslyn compiler through WSL interop (sources read over \\wsl.localhost, output in a
# Windows temp folder), the same flags as tools/remote/stage16_gui.ps1. A compile check only: the exes are not deployed.
R=$(cd "$(dirname "$0")/../.." && pwd)
CSC=$(ls "/mnt/c/Program Files (x86)/Microsoft Visual Studio/"*/BuildTools/MSBuild/Current/Bin/Roslyn/csc.exe 2>/dev/null | head -1)
[ -n "$CSC" ] || { echo "csc.exe not found under /mnt/c"; exit 1; }
WUSER=$(ls /mnt/c/Users | grep -vi 'public\|default\|all users' | grep -i "${WINUSER:-.}" | head -1)
T="/mnt/c/Users/$WUSER/AppData/Local/Temp/visionalvr_gui"; mkdir -p "$T" || exit 1
FW='C:\Windows\Microsoft.NET\Framework64\v4.0.30319'; SRC=$(wslpath -w "$R/tools/gui"); OUT=$(wslpath -w "$T")
REFS=(); for r in System.dll System.Core.dll System.Drawing.dll System.Windows.Forms.dll System.Web.Extensions.dll; do REFS+=("/r:$FW\\$r"); done
rc=0
for t in "VisionALVR.exe:VisionALVR.cs" "configure.exe:Configure.cs"; do
  exe=${t%%:*}; src=${t##*:}; echo "== $exe"
  (cd "$T" && "$CSC" /nologo /noconfig /target:winexe /platform:x64 /langversion:latest /nowarn:1701,1702 "${REFS[@]}" \
     "/win32manifest:$SRC\\app.manifest" "/resource:$SRC\\res\\logo_640.png,logo_640.png" "/out:$OUT\\$exe" "$SRC\\Common.cs" "$SRC\\$src" 2>&1 | tr -d '\r') || rc=1
done
echo "check_gui: $([ $rc = 0 ] && echo OK || echo FAILED)"; exit $rc

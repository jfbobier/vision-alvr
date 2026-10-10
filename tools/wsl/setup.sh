#!/bin/bash
# One-time setup for compile checks from WSL (no MSVC build, no link): header-only dependencies and symlinks under
# external/_wslcheck (gitignored). Needs: clang-cl (Arch `clang`), rustup target x86_64-pc-windows-msvc, Visual Studio Build
# Tools + Windows SDK on the Windows side (headers only are read, through /mnt/c), network for the clones.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
D=$R/external/_wslcheck
mkdir -p "$D/win" "$D/fakebin"
MSVC=$(ls -d "/mnt/c/Program Files (x86)/Microsoft Visual Studio/"*/BuildTools/VC/Tools/MSVC/*/include 2>/dev/null | sort -V | tail -1)
KITS=$(ls -d "/mnt/c/Program Files (x86)/Windows Kits/10/Include/"*/ 2>/dev/null | sort -V | tail -1)
[ -n "$MSVC" ] && [ -n "$KITS" ] || { echo "MSVC/Windows SDK headers not found under /mnt/c"; exit 1; }
ln -sfn "$MSVC" "$D/win/msvc"
for k in ucrt um shared winrt cppwinrt; do ln -sfn "${KITS%/}/$k" "$D/win/$k"; done
[ -d "$D/wil" ] || git clone -q --depth 1 https://github.com/microsoft/wil.git "$D/wil"
# LibOVR headers (VDXR submodule) for the shim
[ -d "$R/external/VDXR/external/LibOVR/Include" ] || git -C "$R/external/VDXR" submodule update --init --depth 1 external/LibOVR
# the ALVR workspace: the host crate as a member (the builder does the same copy), the OpenVR header its session crate parses
W=$R/external/ALVR-v20.14.1
[ -e "$W/alvr/host_harness" ] || ln -s "$R/tools/host" "$W/alvr/host_harness"
mkdir -p "$W/openvr/headers"
[ -s "$W/openvr/headers/openvr_driver.h" ] || curl -sL -o "$W/openvr/headers/openvr_driver.h" https://raw.githubusercontent.com/ValveSoftware/openvr/master/headers/openvr_driver.h
cp "$R/tools/wsl/lib.exe" "$D/fakebin/lib.exe" && chmod +x "$D/fakebin/lib.exe"
rustup target list --installed | grep -q x86_64-pc-windows-msvc || rustup target add x86_64-pc-windows-msvc
echo "ok: $D (msvc: $MSVC)"

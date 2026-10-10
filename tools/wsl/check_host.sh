#!/bin/bash
# `cargo check` of the host crate for the Windows target, from WSL (run tools/wsl/setup.sh once). clang-cl compiles the
# dependencies' C code against the Windows SDK headers; our own C++ (nvenc/) is skipped (NVH_SKIP_CC) and checked by check_cpp.sh.
R=$(cd "$(dirname "$0")/../.." && pwd); D=$R/external/_wslcheck
cd "$R/external/ALVR-v20.14.1" || exit 1
I="/imsvc$D/win/msvc /imsvc$D/win/ucrt /imsvc$D/win/um /imsvc$D/win/shared /imsvc$D/win/winrt -Wno-everything"
PATH=$D/fakebin:$PATH NVENC_DIR=$R/nvenc NVH_SKIP_CC=1 \
CC_x86_64_pc_windows_msvc=clang-cl CXX_x86_64_pc_windows_msvc=clang-cl AR_x86_64_pc_windows_msvc=$D/fakebin/lib.exe \
CFLAGS_x86_64_pc_windows_msvc="$I" CXXFLAGS_x86_64_pc_windows_msvc="$I" \
cargo check -p alvr_host --target x86_64-pc-windows-msvc "$@" 2>&1

#!/bin/bash
# clang-cl syntax/semantic check (no codegen, no shaders, no link) of the OVRShim driver and the host's C++ library against
# the MSVC + Windows SDK headers (run tools/wsl/setup.sh once). Catches type and API errors before the builder does.
R=$(cd "$(dirname "$0")/../.." && pwd); D=$R/external/_wslcheck
B=$D/shim; mkdir -p "$B" "$D/nvhinc"
W="/imsvc $D/win/msvc /imsvc $D/win/ucrt /imsvc $D/win/um /imsvc $D/win/shared /imsvc $D/win/winrt /imsvc $D/win/cppwinrt"
NOWARN="-Wno-microsoft-include -Wno-unknown-pragmas -Wno-pragma-pack -Wno-ignored-attributes -Wno-ignored-pragma-intrinsic -Wno-nonportable-include-path -Wno-deprecated-enum-enum-conversion -Wno-deprecated-anon-enum-enum-conversion -Wno-switch"
rc=0
# --- shim: OVRNull sources + our overrides (as tools/remote/stage11_ovrshim.ps1 assembles them), stub shader headers
rsync -a --exclude 'OVRNull.vcxproj*' --exclude bin --exclude obj "$R/external/VDXR/OVRNull/" "$B/"
cp -r "$R/ovrshim/"* "$B/"
for n in ReprojectVS ReprojectPS LayerVS LayerPS LayerCubePS; do printf '#pragma once\nconst unsigned char k_%s[] = {0, 1, 2, 3};\n' "$n" > "$B/$n.h"; done
echo "== ovrshim/driver.cpp"
clang-cl /std:c++20 /EHsc -fsyntax-only /W3 /D OVR_DLL_BUILD /D NDEBUG /D _WINDOWS /D _USRDLL /D _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING $NOWARN \
  /I"$B" /I"$R/external/VDXR/external/LibOVR/Include" /I"$R/external/VDXR/external/LibOVR/Include/Extras" /I"$D/wil/include" $W "$B/driver.cpp" || rc=1
# --- host library: the build.rs header patches reproduced for the two headers nvh.cpp needs
python3 -I - "$D/nvhinc" "$R/nvenc/upstream/platform/win32/" <<'PY'
import sys, shutil
out, up = sys.argv[1], sys.argv[2]
h = open(up + 'VideoEncoderNVENC.h').read().replace('\r\n', '\n')
h = h.replace('    void Shutdown();', '    void Shutdown();\n    int GetEncoderEngineCount();\n    std::string DescribeConfig();\n    NvEncoder* GetNvEncoder() { return m_NvNecoder.get(); }')
open(out + '/VideoEncoderNVENC.h', 'w').write(h)
nv = open(up + 'NvEncoder.h').read().replace('\r\n', '\n')
acc = '    uint32_t GetEncoderBufferCount() const { return m_nEncoderBuffer; }'
nv = nv.replace(acc, acc + '\n    void* GetSessionHandle() const { return m_hEncoder; }\n    const NV_ENCODE_API_FUNCTION_LIST& GetApi() const { return m_nvenc; }')
open(out + '/NvEncoder.h', 'w').write(nv)
for f in ['NvEncoderD3D11.h', 'VideoEncoder.h']: shutil.copy(up + f, out)
PY
for f in nvh.cpp bench.cpp; do   # (STBI_NO_SIMD below: stb_image.h's cpuid probe trips clang-cl; MSVC builds it as is)
  echo "== nvenc/hostlib/$f"
  clang-cl /std:c++17 /EHsc -fsyntax-only /DNOMINMAX /DSTBI_NO_SIMD -Wno-everything /I"$D/nvhinc" /I"$R/nvenc/shim122" /I"$R/nvenc/shim" /I"$R/nvenc/upstream" \
    /I"$R/nvenc/upstream/platform/win32/d3d-render-utils" /I"$R/nvenc/upstream/platform/win32" /I"$R/nvenc" /I"$R/nvenc/hostlib" /I"$R/ovrshim" \
    /I"$R/third_party/stb" $W "$R/nvenc/hostlib/$f" || rc=1
done
echo "== tools/probe/xr_probe.cpp"
clang-cl /std:c++17 /EHsc -fsyntax-only /DNOMINMAX /DSTBI_NO_SIMD -Wno-everything /I"$R/external/VDXR/external/OpenXR-SDK/include" /I"$R/nvenc/hostlib" /I"$R/third_party/stb" $W "$R/tools/probe/xr_probe.cpp" || rc=1
echo "check_cpp: $([ $rc = 0 ] && echo OK || echo FAILED)"; exit $rc

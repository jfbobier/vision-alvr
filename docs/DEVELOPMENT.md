# Developing VisionALVR

VisionALVR is built **on Windows**, with Microsoft's and NVIDIA's toolchains. Cross-compiling from Linux is not supported.
It is a personal project focused on what I run: NVIDIA GPUs (NVENC) and the Apple Vision Pro. AMD/Intel GPUs and other
headsets are out of scope; forks that adapt it are welcome.

## Prerequisites (Windows 10/11, x64)
| What | Why | How to get it |
|---|---|---|
| Git | sources, pinned clones of ALVR and VDXR | `winget install --id Git.Git -e` |
| Rust (rustup, MSVC toolchain) | the host (`alvr_host`) and ALVR's crates | `winget install --id Rustlang.Rustup -e`, then `rustup default stable-x86_64-pc-windows-msvc` |
| Visual Studio 2022 Community, "Desktop development with C++" | MSVC, Windows SDK, MSBuild (VDXR, OVRShim, the C++ encoder library), Roslyn `csc` (GUIs) | `winget install --id Microsoft.VisualStudio.2022.Community -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"` |
| CMake | the OpenXR loader (from VDXR's OpenXR SDK submodule) | `winget install --id Kitware.CMake -e` |
| LLVM (clang) | used by ALVR's build | `winget install --id LLVM.LLVM -e` |
| NuGet CLI | VDXR's NuGet packages | `winget install --id Microsoft.NuGet -e` |
| NVIDIA GPU + driver | NVENC (encoding, benchmarks, tests) | from NVIDIA: nvidia.com/drivers or the NVIDIA app (no CUDA toolkit or Video Codec SDK needed: the SDK header is vendored) |
| .NET Framework 4.8 | the two GUIs | part of Windows 10/11 |

Open a new terminal after installing so `PATH` is updated. The build scripts expect Visual Studio **Community** at its default
path. Known-good versions (`deps.lock.json`, `toolchain_builder`): Rust 1.99, CMake 4.4, clang 23.1, NuGet 7.9, MSVC 14.44,
Windows SDK 26100, NVIDIA driver 617.14 (NVENC SDK 12.2). Any NVENC GPU can build and run the tests; only `VisionALVR.exe`
refuses GPUs older than RTX 40 (`VISIONALVR_ALLOW_ANY_GPU=1` lifts that check for development).

## Build
```powershell
git clone https://github.com/jfbobier/vision-alvr.git
cd vision-alvr
powershell -ExecutionPolicy Bypass -File build.ps1              # everything: vdxr host shim gui package
powershell -ExecutionPolicy Bypass -File build.ps1 host gui     # only some parts afterwards
```
- The first run checks the tools, clones ALVR v20.14.1 and VirtualDesktop-OpenXR (with its submodules) at the commits pinned
  in `deps.lock.json` into `src\` and verifies them, then builds VDXR, the OpenXR loader and ALVR's server_core once
  (`tools/remote/setup_builder.ps1`). Allow 30-60 minutes and ~10 GB; later builds take minutes.
- Output: `out\dist\VisionALVR\` (the portable folder) and `out\dist\VisionALVR-<version>.zip`. Logs: `logs\build_<step>.txt`.
- Nothing outside the checkout is changed: no registry, no OpenXR runtime registration (that is `register_openxr_runtime.bat`,
  run by hand from the built folder).
- The benchmark scene (`bench\littlest_tokyo.vab`) is generated, not committed. `build.ps1` takes it from the published release
  (SHA-256 checked); to regenerate it, `tools/bench_scene/build.sh <work dir> build/bench/littlest_tokyo.vab` (a Linux shell
  with g++, cmake and curl: downloads the pinned GLB, Draco and cgltf, checks their hashes, converts).

## How the build fits together
- `alvr_host` is a member of ALVR's own Rust workspace (`tools/remote/stage8_host.ps1` copies `tools/host` to
  `src\ALVR-v20.14.1\alvr\host_harness`), so it links ALVR's crates exactly as ALVR's server does.
- ALVR's encoder C++ files are vendored unmodified in `nvenc/upstream/` (checksums: `nvenc/upstream.sha256`).
  `tools/host/build.rs` makes build-time copies with small, asserted text patches (10-bit formats, SDK 12.2 fields, split
  encode, QP map, reconstructed-frame output) and compiles them with `nvenc/hostlib/` into a static library.
- The only patch to ALVR's sources is `tools/patches/loopback-client.patch` (test only: lets the mock client bind its own
  loopback address; inactive without its environment variable).
- VDXR gets three small patches at build time (`tools/remote/stage15_vdxr.ps1`); OVRShim is built from VDXR's OVRNull project
  with the files of `ovrshim/` (`stage11_ovrshim.ps1`).
- GUIs: compiled with Visual Studio's Roslyn `csc` against the .NET Framework assemblies (`stage16_gui.ps1`), no .NET SDK.
- Package: `stage13_package.ps1` assembles `out\dist\VisionALVR` and the zip from an explicit list of files.
- The `tools/remote/stage*.ps1` scripts work in `%VISIONALVR_ROOT%` (set by `build.ps1` to the checkout; the harness uses
  `%USERPROFILE%\openxr`).

## Automated tests (optional)
The harness (`harness/`, Python stdlib) runs end-to-end scenarios on the Windows PC with a headless mock ALVR client: real
NVENC encodes, the OpenXR test app through VDXR + OVRShim, then checks on the received stream (frame counters, timestamps,
pacing, layers, gamma, ...). It is how this project is tested, but it is not needed to build. It is driven from a Linux
shell over SSH (WSL on the same PC works: enable Windows' OpenSSH Server with key authentication) and keeps its own working
copy in `%USERPROFILE%\openxr` on the Windows side. Driver side: `bash`, `ssh`/`scp`, `python3`, `ffmpeg`, `ffprobe`.
```bash
echo 'VISIONALVR_BUILDER=you@windows-host' > tools/builder.local    # not committed; or export VISIONALVR_BUILDER
python3 harness/run.py setup                                        # pinned sources, prerequisites, every build
python3 harness/run.py run e2e_baseline                             # one scenario; `all` for the suite, `list` to see them
python3 harness/run.py bench-quality quick | test-install | test-zip
```
Details: `harness/README.md`.

## Rules (keep them)
- Tests never change the machine's OpenXR runtime registration: apps get the runtime through a process-local
  `XR_RUNTIME_JSON`. `test-install` works on a throwaway `HKCU` key.
- `tools/builder.sh` refuses commands that look destructive (deletes, formatting, registry deletes); pass
  `--allow-destructive` only for a command you have checked.
- A result is reported as **tested** only when a scenario or a measurement shows it; otherwise *built* or *source-verified*
  (`docs/progress.md` legend). Say what was not tested.

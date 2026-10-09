# Developing VisionALVR

## Topology
Two roles, which can be the same physical PC:
- **Builder**: Windows 10/11 with an NVIDIA GPU (NVENC). Everything compiles and runs here: Rust host, C++ encoder library,
  VDXR + OVRShim, GUIs, the mock client, the OpenXR test app. Any NVENC GPU works for development: only `VisionALVR.exe`
  refuses GPUs older than RTX 40 (`VISIONALVR_ALLOW_ANY_GPU=1` lifts that check); the host and the harness do not check.
- **Driver**: a Linux shell (WSL is fine) that runs `harness/run.py`: uploads sources over SSH, starts builds and tests on the
  builder, collects and checks the results.

One PC: install WSL on the builder and enable Windows' OpenSSH Server; from WSL the Windows side is reachable at the default
gateway (`ip route show default`) with WSL's NAT networking, or at `localhost` with mirrored networking.

## Builder prerequisites
- Git, Rust (rustup, default MSVC toolchain), CMake, LLVM/clang, the NuGet CLI (all on `PATH`; e.g. with `winget`).
- Visual Studio 2022 **Community** with "Desktop development with C++" (the scripts use its default install path; tested with
  MSVC 14.44 and Windows SDK 26100). .NET Framework 4.8 (part of Windows) for the GUIs.
- OpenSSH Server with **key authentication** for the account used; the working folder is `%USERPROFILE%\openxr`.
- An NVIDIA driver recent enough for NVENC SDK 12.2 (tested with 617.14).
- Toolchain versions known to work: `deps.lock.json` (`toolchain_builder`).

## Driver prerequisites
`bash`, `ssh`/`scp`, `python3` (stdlib only), `ffmpeg` and `ffprobe` (stream checks). For the benchmark scene converter only:
`g++`, `cmake`, `make`, `curl`.

## First setup
```bash
echo 'VISIONALVR_BUILDER=you@builder-host' > tools/builder.local    # not committed; or export VISIONALVR_BUILDER
bash tools/builder.sh run "echo hello"                              # key auth must work without a prompt
python3 harness/run.py setup                                        # pinned sources + prerequisites + every build
```
`setup` checks the toolchain, clones ALVR v20.14.1 and VirtualDesktop-OpenXR at the commits pinned in `deps.lock.json` into
`%USERPROFILE%\openxr\src` (existing clones are verified, never changed), builds VDXR, the OpenXR loader and ALVR's
server_core once, then `build nvenc vdxr client host shim probe gui package`. `setup --clone-only <dir>` only clones and verifies.

The benchmark scene is generated, not committed (`build/` is ignored): `tools/bench_scene/build.sh <work dir>
build/bench/littlest_tokyo.vab` downloads the pinned GLB, Draco and cgltf, checks their hashes and converts. `run.py build host`
uploads it to the builder when it exists.

## Daily loop
```bash
python3 harness/run.py build host        # or: client shim vdxr probe gui package nvenc
python3 harness/run.py run e2e_baseline  # one scenario; `all` for the whole suite, `list` to see them
python3 harness/run.py bench-quality quick
python3 harness/run.py test-install      # installer logic against a throwaway HKCU key
python3 harness/run.py test-zip          # the shipped zip, unzipped and run from a fresh folder
```
Results go to `harness/results/<UTC>_<scenario>/` (ignored by git). `harness/README.md` explains topologies and checks.

## How the build fits together
- `alvr_host` is a member of the builder's ALVR workspace (`tools/remote/stage8_host.ps1` copies `tools/host` to
  `alvr/host_harness`), so it links ALVR's crates exactly as ALVR's own server does. Rust builds only happen on the builder.
- ALVR's encoder C++ files are vendored unmodified in `nvenc/upstream/` (checksums: `nvenc/upstream.sha256`).
  `tools/host/build.rs` makes build-time copies with small, asserted text patches (10-bit formats, SDK 12.2 fields, split
  encode, QP map, reconstructed-frame output) and compiles them with `nvenc/hostlib/` into a static library.
- The only patch to ALVR's sources is `tools/patches/loopback-client.patch` (test only: lets the mock client bind its own
  loopback address; inactive without its environment variable).
- VDXR gets three small patches at build time (`tools/remote/stage15_vdxr.ps1`); OVRShim is built from VDXR's OVRNull project
  with the files of `ovrshim/` (`stage11_ovrshim.ps1`).
- GUIs: compiled with Visual Studio's Roslyn `csc` against the .NET Framework assemblies (`stage16_gui.ps1`), no SDK.
- Package: `stage13_package.ps1` assembles `out\dist\VisionALVR` and the release zip (an explicit list of files).

## Rules the harness follows (keep them)
- Tests never change the machine's OpenXR runtime registration: apps get the runtime through a process-local
  `XR_RUNTIME_JSON`. `test-install` works on a throwaway `HKCU` key.
- `tools/builder.sh` refuses commands that look destructive (deletes, formatting, registry deletes); pass
  `--allow-destructive` only for a command you have checked.
- A result is reported as **tested** only when a scenario or a measurement shows it; otherwise *built* or *source-verified*
  (`docs/progress.md` legend). Say what was not tested.

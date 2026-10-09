# VisionALVR

Stream PC OpenXR games to **Apple Vision Pro** with the stock **ALVR** visionOS app, **without SteamVR**, on NVIDIA GPUs.

> **Alpha software (0.1).** It runs real games on an RTX 5090 + Vision Pro, but expect rough edges. Use at your own risk.

VisionALVR replaces the SteamVR + ALVR server pair on the PC with one lean streamer. It speaks the ALVR 20.14.1 protocol
unchanged, so the ALVR app on the headset sees an ordinary ALVR server. In between, it does less work:
no SteamVR compositor, no extra frame copies between processes, and an encode path written for NVENC only.

```
OpenXR game
  -> VirtualDesktop-OpenXR (VDXR, OpenXR runtime, small patches)
  -> OVRShim (LibOVRRT64_1.dll: composes layers into a shared 10-bit side-by-side frame ring)
  -> alvr_host.exe (ALVR v20.14.1 server_core + ALVR's own foveated encoding + NVENC HEVC 10-bit)
  -> ALVR protocol over Wi-Fi
  -> ALVR app on Vision Pro (stock, from the App Store)
```

## Status
- **Works on the real headset** (first pilot, RTX 5090 + Vision Pro): most UEVR games, Luke Ross mods (Dead Space Remake),
  controllers (ALVR's Quest/Touch emulation), haptics, game audio, discovery/pairing with the stock app.
- **Known issues**: one Unity game crashes, one UEVR game shows no image, one Unity game shows its menu very small.
  Fixed since the pilot but not yet re-tested on the headset: frame pacing (host display clock), head bobbing, image too dark,
  games running elevated (e.g. Cemu) never reaching the streamer.
- The benchmark that picks settings for your PC + router + headset is half built: the headless quality sweep works
  (`docs/BENCHMARK_DESIGN.md`); the headset phases and applying the results are next.
- Measured on a 1-NVENC laptop GPU and a 5090. Multi-engine split encode is wired in but not yet active in `auto` mode.

`docs/progress.md` is the detailed log: what is built, what is tested (and how), what is not.

## Requirements
- Windows 10/11, **NVIDIA GeForce RTX 40 series or newer** (the runtime refuses older GPUs).
- Apple Vision Pro with the **ALVR** app (20.14.x), PC and headset on the same LAN (5 GHz / 6 GHz Wi-Fi recommended).
- SteamVR and ALVR's own launcher closed while streaming (same ports).

## Install (users)
Download the release zip, unzip it anywhere, run `register_openxr_runtime.bat` once, then `configure.exe` (pair the headset,
benchmark, settings) and `VisionALVR.exe` (keep it open while you play). Details, settings and logs: [docs/INSTALL.md](docs/INSTALL.md).

## Build and test (contributors)
Everything builds and runs on a Windows "builder" PC with an NVIDIA GPU, driven over SSH from a Linux shell (WSL works, also
on the builder itself). One command sets up the pinned sources and builds everything; an automated harness runs end-to-end
scenarios with a headless mock ALVR client, so most changes can be tested without a headset.
See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

## Repository layout
| Path | What |
|---|---|
| `tools/host/` | `alvr_host` (Rust): ALVR server_core embedded, encoder loop, pacing, benchmarks, logging |
| `nvenc/hostlib/` | C++ library of the host: NVENC wrapper around ALVR's encoder, IPC, QP map, benchmark renderer + quality metric |
| `nvenc/upstream/` | ALVR's encoder sources, vendored **unmodified** (checksums in `nvenc/upstream.sha256`; patched at build time by `tools/host/build.rs`) |
| `ovrshim/` | OVRShim: the LibOVR-side driver loaded by VDXR (fork of VDXR's OVRNull): layer composition, frame ring, IPC |
| `tools/gui/` | `VisionALVR.exe` (runtime window) and `configure.exe` (pairing, benchmarks, settings), C# WinForms |
| `tools/install/` | portable-folder scripts (OpenXR runtime registration), shipped default session |
| `tools/mock_client/` | headless ALVR client (Rust, ALVR's client_core) used by the harness |
| `tools/probe/` | `xr_probe`: OpenXR test app (layers, input, pacing) |
| `tools/bench_scene/` | offline converter of the benchmark scene (glTF + Draco -> `.vab`) |
| `tools/patches/`, `tools/remote/` | the one ALVR patch (test-only loopback binding), PowerShell build/test stages run on the builder |
| `harness/` | test harness (Python, stdlib only): scenarios, checks, builder orchestration |
| `third_party/` | vendored headers (cgltf, stb_image) and licence texts |
| `docs/` | install guide, benchmark design, progress log |

## Credits
- [ALVR](https://github.com/alvr-org/ALVR) (MIT): protocol, server core and encoder code; the
  [ALVR visionOS client](https://github.com/alvr-org/alvr-visionos) on the headset.
- [VirtualDesktop-OpenXR](https://github.com/mbucchia/VirtualDesktop-OpenXR) by Matthieu Bucchianeri (MIT): the OpenXR runtime;
  OVRShim is a fork of its OVRNull.
- NVIDIA Video Codec SDK header (MIT). cgltf (MIT), stb_image (public domain / MIT), Draco (Apache-2.0, offline converter only).
- Benchmark scene: "Littlest Tokyo" by Glen Fox ([glenatron](https://sketchfab.com/glenatron)), CC-BY-4.0, converted and placed
  in a textured room.

Details: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). VisionALVR is not affiliated with or endorsed by the ALVR project,
Apple, NVIDIA or Virtual Desktop. Apple Vision Pro and visionOS are trademarks of Apple Inc.

## License
MIT, see [LICENSE](LICENSE). Third-party components keep their own licences ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

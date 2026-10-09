# Third-party notices

## In this repository
| Component | Where | License | Notes |
|---|---|---|---|
| ALVR encoder sources (v20.14.1) | `nvenc/upstream/` | MIT, Copyright (c) 2018-2019 polygraphene, (c) 2020-2024 alvr-org: `third_party/licenses/ALVR-LICENSE.txt` | vendored unmodified (`nvenc/upstream.sha256`), patched at build time by `tools/host/build.rs`; `nvenc/shim/` replaces ALVR's logger/settings glue |
| NVIDIA Video Codec SDK header `nvEncodeAPI.h` (12.2, and the older copy inside `nvenc/upstream`) | `nvenc/shim122/alvr_server/` | MIT-style notice in the header, Copyright (c) 2010-2024 NVIDIA Corporation | |
| OVRShim (fork of VDXR's OVRNull) | `ovrshim/` | MIT, Copyright (c) Matthieu Bucchianeri: `third_party/licenses/VirtualDesktop-OpenXR-LICENSE.txt` | files that came from VDXR keep their MIT header; built inside the VDXR checkout (its LibOVR headers are not vendored here) |
| cgltf 1.14 | `third_party/cgltf/` | MIT (in the header) | offline scene converter only |
| stb_image 2.30 | `third_party/stb/` | public domain / MIT (in the header) | host benchmark renderer |
| ALVR source patch (test only) | `tools/patches/loopback-client.patch` | MIT (ALVR) | applied to the builder's ALVR checkout |

Versions, sources and checksums: `third_party/README.md`, `deps.lock.json`.

## Fetched at build time (not in this repository)
- **ALVR v20.14.1** (MIT, github.com/alvr-org/ALVR): `server_core` and its crates are linked into `alvr_host.exe`.
- **VirtualDesktop-OpenXR** (MIT, github.com/mbucchia/VirtualDesktop-OpenXR) at the commit in `deps.lock.json`, with its
  submodules (OpenXR SDK: Apache-2.0; LibOVR; FidelityFX; fmt; cJSON; ...): the OpenXR runtime DLL is built from it with three
  small patches (`tools/remote/stage15_vdxr.ps1`).
- **Draco 1.5.7** (Apache-2.0) and the *Littlest Tokyo* GLB: only for the offline scene converter (`tools/bench_scene/build.sh`).

## In the release zip
- `alvr_host.exe`: VisionALVR + ALVR (MIT) + 263 Rust crates, listed with their licenses in
  `third_party/licenses/alvr_host-crates.txt`. All are permissive (MIT / Apache-2.0 / BSD / ISC / Zlib / Unicode-3.0 / BSL-1.0)
  except six under **MPL-2.0** (`symphonia*`, `webpki-roots`, `option-ext`), used unmodified; their source is at crates.io.
- `virtualdesktop-openxr.dll` / `.json`: VirtualDesktop-OpenXR (MIT), patched as above.
- `LibOVRRT64_1.dll`: OVRShim (MIT).
- `bench\littlest_tokyo.vab`: **"Littlest Tokyo" by Glen Fox** (glenatron, https://sketchfab.com/glenatron), licensed
  **CC-BY-4.0** (https://creativecommons.org/licenses/by/4.0/), from the three.js examples (r169). Changes: converted to
  VisionALVR's format (meshes decompressed, simplified shading), shown inside a room tiled with the scene's own texture.

VisionALVR is not affiliated with or endorsed by the ALVR project, Apple, NVIDIA, Virtual Desktop or Glen Fox.

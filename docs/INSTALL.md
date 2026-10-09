# VisionALVR: install, use, logs (0.1 alpha)

VisionALVR is **portable**, like ALVR and UEVR: one folder you unzip wherever you want. Nothing goes to Program Files; the only
system changes are the OpenXR runtime registration and one firewall rule, both undone by `unregister_openxr_runtime.bat`.
Requirements: Windows 10/11, NVIDIA GeForce RTX 40 series or newer, the ALVR app (20.14.x) on the Vision Pro, same LAN.

## The folder

| File | What it is |
|---|---|
| `VisionALVR.exe` | the runtime window: keep it open while you play (starts the streamer, shows status, gamma / debug switches) |
| `configure.exe` | one-time setup: pair the headset, settings, optional benchmarks |
| `alvr_host.exe` | the streamer (ALVR server_core + NVENC); started by VisionALVR.exe, no window |
| `virtualdesktop-openxr.dll`, `.json`, `LibOVRRT64_1.dll` | the OpenXR runtime games load (VDXR + VisionALVR's OVR shim) |
| `register_openxr_runtime.bat` / `unregister_openxr_runtime.bat` (+ `.ps1`) | make this folder the Windows OpenXR runtime / put the previous one back |
| `bench\littlest_tokyo.vab` | the benchmark scene ("Littlest Tokyo" by Glen Fox, CC-BY-4.0) |
| `THIRD_PARTY_NOTICES.md`, `licenses\` | third-party licences (ALVR, VirtualDesktop-OpenXR, the Rust crates in `alvr_host.exe`, the scene) |
| `config\session.default.json` | shipped ALVR settings; `config\session.json` (yours) and `config\visionalvr.json` are created on first use |
| `logs\` | created on first use (see below) |

Build it with `python3 harness/run.py build gui package` (on the builder: `out\dist\VisionALVR` and `out\dist\VisionALVR-0.1-alpha.zip`;
the zip holds exactly the files above under one `VisionALVR\` folder, never logs or user config).
`python3 harness/run.py test-zip` unzips it into a fresh folder on the builder and runs from there (logs created on first run,
general log appended by every run, one `logs\debug\<time>\` per debug run, config seeded, benchmark finds its scene).

## Steps
1. Unzip. 2. `register_openxr_runtime.bat` (asks for administrator rights; moved the folder? run it again). 3. `configure.exe`:
open the ALVR app on the Vision Pro, select it, **Pair**; check the settings; set the ALVR app's chroma key to the "colour when no
game runs" (default #00FF00). 4. `VisionALVR.exe`, then start an OpenXR game. Close SteamVR and ALVR's own launcher first (same ports).

VisionALVR.exe refuses to stream on GPUs older than RTX 40 (NVML architecture < Ada). Developers: `VISIONALVR_ALLOW_ANY_GPU=1`.

## Settings: what is used
`configure.exe` edits only what VisionALVR uses; everything else stays in `config\session.json` for hand editing.
- ALVR (session.json): resolution per eye, refresh rate, constant bitrate, foveated encoding + its geometry, game audio + mute
  PC speakers, stream protocol (UDP/TCP), packet size, haptics strength. ALVR's controller section IS used (we deliver ALVR's own
  Quest/Touch emulation: button mapping, haptics curve, controller offsets), as is its network section.
- VisionALVR (visionalvr.json): encode profile (`foveated` or `full-split`), idle colour, debug default, display gamma. The NVENC
  encoder parameters are fixed (`tools/host/src/nvidia_profile.rs`).
- Not used: SteamVR/OpenVR options, color correction / sharpening (ALVR compositor features we bypass), wired/ADB.

## Benchmarks (configure.exe, optional)
- **Encoder test** (no headset): 300 frames of each encode profile at the configured resolution and bitrate: p50/p95 ms and
  whether it fits the frame time.
- **Network test** (headset paired, ALVR app open; it shows noise): random frames (16-pixel random blocks + fine noise, so every
  frame costs the full bitrate and rate control can reach targets from ~100 to 500 Mbps) at each bitrate step, 2 s settle + N s
  measured with ALVR's own client statistics: delivered Mbps, headset fps, packets lost/s, total latency split into encode /
  network / decode. Recommendation = highest step that kept the fps, lost nothing and did not raise the network latency, minus
  10%. A GPU keep-alive load runs during the test (an idle GPU downclocks NVENC).

- **Quality benchmark** (no headset, ~75 s; `alvr_host --benchmark-quality`, configure.exe's wizard comes with S3): the shipped
  scene `bench\littlest_tokyo.vab` ("Littlest Tokyo" by Glen Fox, CC-BY-4.0, inside a textured room) through ALVR's foveation and
  NVENC; NVENC's reconstructed frame is compared on the headset's display grid with the native render. ~30 configurations
  (preset, resolution, foveation, AQ, split): encode time, bitrate, slices, PSNR; three proposals in `config\benchmark_quality.json`.

## Logs
| File | When | Content |
|---|---|---|
| `logs\VisionALVR.log` | always (appended; > 10 MB rotates to .1) | one line per fact from every component (`[host]`, `[shim:<game>.exe]`, `[gui]`, `[configure]`): start/stop, GPU, paired headset, headset connected/disconnected, negotiated stream, game start (exe, size, FoV, gamma, IPC) and end (duration, rendered/streamed fps), settings saved, benchmark results, every warning and error |
| `logs\alvr.log` | debug off | ALVR server_core's own log for the last run |
| `logs\debug\<yyyymmdd-hhmmss>\` | debug on (one folder per streamer run) | `host_events.jsonl` (every event: encoder_stats every 2 s incl. ALVR's client statistics, status every 10 s, ...), `alvr.log`, `gpu.csv` (NVML 1 Hz: utilization of GPU/encoder/decoder, clocks, power, temperature, P-state, VRAM, clock event reasons), `shim_<game>_<pid>.log` (layer layouts, pacing counters, swapchains) |

Report a problem with `logs\VisionALVR.log` + the matching `logs\debug\<time>\` folder. PresentMon is not used: OpenXR games do
not present through DXGI here, the host measures the game's real frame rate itself (`app_fps`, `app_frame_interval_ms`).

## Tested vs untested (harness, laptop builder, mock client)
Tested: the portable folder end to end (`e2e_installed_layout`: host with `--install-dir` and debug, the app against only the
folder's manifest, the general log's key lines and the debug folder's files), the registration script against a throwaway HKCU
key (`test-install`: register, re-register, unregister, foreign runtime, moved folder, taking over ClearXR / Program Files
installs), the network benchmark (`e2e_benchmark_network`), the GUIs build and render (screenshots), the encoder test.
**Not tested:** the real HKLM write and firewall rule, clicking through the GUIs with a real headset, the GPU refusal on a real
older GPU (the builder's RTX 30 laptop was used with the override).

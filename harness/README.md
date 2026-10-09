# Automated harness

Everything is driven from WSL: `python3 harness/run.py <command>`. Stdlib Python only.

| Command | What it does |
|---|---|
| `list` | scenarios in `harness/scenarios/*.json` |
| `build [host\|client\|shim\|probe\|gui\|package\|nvenc]` | (re)build on the builder (cargo / MSVC); default `client host` |
| `run <name>...` / `all` | run scenarios; exit 0 all PASS, 1 any FAIL, 2 any BLOCKED, 3 usage |
| `report [dir]` | re-evaluate saved artifacts with the current checks (no builder needed) |
| `bench-quality [quick\|full]` | benchmark phase 2 headless on the builder (`stage17_bench_quality.ps1`) + checks: recon used and matching the input's colour conversion, plausible PSNR, quality ordering (preset, foveation, resolution), proposals, duration |
| `test-zip` | the shipped zip unzipped into a fresh folder on the builder and run from it (`stage18_zip_test.ps1`): single top folder, no logs/user config shipped, logs created on first run, general log appended per run, one debug folder per debug run |
| `test-install` | runs the installer's registry/files logic against a throwaway `HKCU` key (never HKLM), see `docs/INSTALL.md` |

Results: `harness/results/<UTC>_<scenario>/` (`result.json`, `analysis.json`, host/client reports, `client.hevc`,
`frames.csv`, logs); `harness/results/latest` points at the newest. A scenario is BLOCKED (not FAIL) when the
infrastructure is missing (builder unreachable, binary or source absent, impossible topology).

## Topology
Note: a client on a loopback address makes `server_core` treat it as *wired* and force the **TCP** stream protocol, so loopback
scenarios do not exercise ALVR's UDP stream (the real AVP uses UDP).


**Default (loopback):** host and client both run on the builder, so the full 250 Mbps stream and a real GPU host can be used.
The client is given its own loopback address with `ALVR_BIND_IP=127.0.0.2` (bind + `SO_REUSEADDR`) and the host reaches it by
manual IP. This needs the env-gated patch `tools/patches/loopback-client.patch` on the builder's ALVR checkout (applied
automatically by `python3 harness/run.py build`); without the variable the client behaves exactly like upstream.
Packet loss and latency across a real network are **out of scope** (the headset's Wi-Fi handles that); the thresholds
only guard against loopback regressions.

## Cross-network topology (optional scenarios in `scenarios/optional/`, not part of `all`)
- Host and client **cannot share a machine**: both bind UDP 9943 (`os error 10048`). Discovery broadcasts do not
  cross subnets, so the host is given the client by **pinned hostname + manual IP** (session `client_connections`).
- In the original setup the dev PC's firewall blocked inbound connections from the builder, so these scenarios run
  **host = `alvr_host.exe` from WSL via interop (dev PC), client = builder**. The host needs no GPU while it replays a
  pre-encoded file. Live NVENC on the builder with a client elsewhere needs inbound UDP to the builder.
- Some Windows policies (e.g. Windows Information Protection) block Windows processes from writing to `\\wsl.localhost\...`, so the host works in a
  Windows-local scratch dir (`%LOCALAPPDATA%\Temp\openxr_harness\<run>`, deleted after) and the harness copies the
  report back. Everything else (results, cache, ffmpeg inputs) stays on the WSL filesystem.

## Checks (per scenario, `checks` block)
Protocol: host/client connected, negotiated config (resolution, full range, foveation), codec, timestamp
monotonic/duplicates, pose-match (client timestamp echo), arrival-minus-timestamp p95, fps, delivery ratio (UDP loss).
Stream: ffprobe profile/pix_fmt/size/range. Decode: Windows `ffmpeg.exe -hwaccel d3d11va` on the AMD iGPU (software
ffmpeg fallback), frame count vs received, and **content integrity** (per-frame md5 of every received frame against the
source stream decoded the same way; tolerance = `max_unknown_frame_ratio`, to allow loss-damaged frames).

## Scenarios
- `client_no_host`: negative control (client alone must report "never connected").
- `e2e_loopback_hevc10`: the main regression scenario (250 Mbps, 10-bit HEVC, 7104x3200, 90 fps, loopback on the builder).
- `e2e_live_nvenc` (3552x1600) / `e2e_live_nvenc_fullres` (7104x3200): `alvr_host --live` encodes a stamped pattern with NVENC; the
  harness reads the frame counter back from the decoded client stream and checks it against the host's per-frame log.
- `e2e_openxr_probe`: **the real chain.** An OpenXR app (`tools/probe/xr_probe.cpp`, D3D11) runs against VDXR with our OVRShim as its
  OVR runtime; the shim composes the app's layers into a side-by-side texture shared with `alvr_host --shim`, which encodes it with
  NVENC and streams it to the mock client. Checks: the app's per-frame counter and an sRGB gray patch come out of the decoded
  stream, the head pose the app received equals the yaw the client sent for that frame's timestamp, plus all stream checks.
- `e2e_openxr_input`: controllers: scripted Touch input and poses from the client must equal what the app reads; the app's haptics must reach the client.
- `e2e_green_idle` / `e2e_app_lifecycle`: pure green when no app runs; green -> app -> green when an app starts and exits.
- `e2e_openxr_foveated`: the user's real settings (3552x3200 per eye, foveation on): encoded size must equal ALVR's layout (independent Python
  port of `FFR.cpp`) and the stamp is read back at the positions that port derives from the compress shader.
- `e2e_daemon_reconnect`: one `--daemon` host process serves a headset session, a disconnect, and a second session.
- `e2e_avp_late_views`: the user's session with the mock behaving like the stock visionOS client in its immersive space
  (`--views-after-first-frame`: ViewsConfig only after the first decoded frame). Also checks game-audio capture started, pose-matched
  frame timestamps and the async submit mode.
- `e2e_installed_layout`: the portable folder `out\dist\VisionALVR` copied as a user unzips it; host with `--install-dir --debug`, app against
  only its manifest; checks the general log's key lines and the debug session files.
- `e2e_benchmark_network`: `--benchmark-network 100,200` to the mock client: steps must reach their target bitrate at >= 80 fps.
- `e2e_heavy_app_async` (and `optional/e2e_heavy_app_sync`, `OVRSHIM_SYNC_SUBMIT=1`): xr_probe with a synthetic CPU + GPU load per
  frame (`XR_PROBE_CPU_MS`, `XR_PROBE_GPU_COPIES`) that only fits 90 Hz when the app's CPU and GPU work overlap; laptop: 93.5 vs 65.4 fps.
- optional: `e2e_openxr_foveated_split` (needs a GPU with >= 2 NVENC engines, e.g. the 5090) and `e2e_wslhost_hevc10_30mbps` (host in WSL, client on the builder over the real network) and
  `net_capacity_250mbps` (records what that network can carry).

## Adding a scenario
Copy a JSON file; set `source` (a `.hevc` on the builder, e.g. from `nvenc/` stage6), `host`/`client` args, `checks`.
Sources are fetched into `harness/cache/` once. Regenerate streams with `python3 harness/run.py build nvenc`.

## Cross-network note
Only inbound UDP 5000 is known to be open on the WSL machine. ALVR's stream port is configurable (session `connection.stream_port`), but the
server also opens a TCP control connection to the client on the hardcoded port 9943, which the laptop firewall blocked. A real
WSL-client chain needs one open inbound TCP port plus a small patch making the control port configurable (like `loopback-client.patch`).

## GPU power state matters for timing
With a light synthetic load the laptop GPU drops to P8 (NVENC clock 555 MHz vs 1560 MHz), making encodes ~3x slower. Live scenarios therefore
pass `host.gpu_keepalive: 90` (a background GPU load standing in for the compositor/game). `gpu_sample: true` records `nvidia-smi`
(p-state, clocks, encoder/decoder utilization, power) to `gpu.csv` for every run. Concurrent NVDEC decode does not slow NVENC.

## OpenXR chain details (`e2e_openxr_probe`)
- **Components:** `ovrshim/` (fork of VDXR OVRNull: `driver.cpp` = `HostDriver`, `ipc.h`/`ipc_win.h` = shared-memory contract,
  modified `ReprojectPS.hlsl`/`constantsbuffer.h`), built by `build shim` into `out\vdxr-shim` together with the unmodified VDXR dll.
  The host side is `alvr_host --shim` (`nvh_ipc_*` in `nvenc/hostlib`).
- **Process model:** host, client and app are separate processes. The host and the app are both started as non-elevated scheduled
  tasks in the interactive session (`topology.host_launch: "interactive"`): the OpenXR loader ignores `XR_RUNTIME_JSON` for elevated
  processes, and D3D shared textures do not cross Windows sessions. The IPC objects use the `Global\` namespace (fallback: local).
- **Frame timestamp contract:** the shim remembers the client tracking timestamp of the head pose the app last read and attaches it to the
  frame it composes; the host sends the frame with that timestamp (what the client echoes back).

## Pacing checks
`checks.pacing` (scenarios `e2e_baseline`, `e2e_openxr_probe`): client arrival spacing of the app's frames after `warmup_frames` (`client_p99_ms`, `max_late_frac` = share of gaps over 1.5 periods), and the host's own `pacing` report (`vsync_p99_ms`, `vsync_max_ms`, `intake_p99_ms`). The counter check pairs received frames with host rows in order (the host may skip frame numbers) and needs a constant counter offset.

# configure.exe benchmark: design (2026-10-09; S1 built and tested on the laptop, see "Status")

Goal: once per setup (PC + router + Vision Pro), find the settings that give the best picture for an acceptable
**encode + network + decode** latency, and offer three choices. About 4 minutes, headset worn for the phases marked (HMD).

## What is measured, and how
- **Latency** = ALVR's own client statistics (already captured by the host): encode, network, decode, decoder queue, plus
  frame drops and packet loss. The figure optimised is `encode_p95 + network + decode` for the frame size and bitrate.
- **Quality** = the decoded picture compared with a reference, in **display space**: the encoder's reconstructed frame (NVENC
  can output it, bit-exact with what the headset decodes; capability `recon_output` is now logged in `nvenc_config`; fallback:
  decode with NVDEC from the driver) is un-foveated with the client's own mapping (`decompressAxisAlignedCoord`, alvr-visionos),
  resampled to the reference grid and compared on luma: **foveation-weighted PSNR** (weight 1.0 in the centre falling to 0.3 at the
  edges), also reported as centre-only and periphery-only. The reference is the same frames rendered at the largest resolution
  tested (1.2x), so a resolution gain can show up, and a resolution loss costs.
- **Content** = *Littlest Tokyo* by Glen Fox (glenatron), CC-BY-4.0 (three.js examples, 4 MB glTF, 71 meshes, 4 textures, one
  keyframe animation), rendered by the host itself (D3D11) with its animation running and a scripted head motion (look-around
  yaw +-25 deg, pitch +-8 deg, slow drift): detailed textures, edges, motion; not a test pattern the encoder can cheat on.
  While the headset is connected the host renders from the real head pose instead, so the view is comfortable.

## Phases
1. **Link budget (HMD, ~70 s)**: today's random-block stream (tested: rate control reaches 100-500 Mbps targets) at
   100..500 Mbps on the current frame size. Per step: delivered Mbps, loss, network latency trend (a rising latency = queueing
   in the Wi-Fi), headset decode time and decoder queue, headset fps. Budget **B** = highest clean step - 10%. Then one decode check
   at the largest candidate frame size at B: if the Vision Pro cannot decode it in time, larger sizes are dropped.
   If steps fail, the result says why (loss = radio/router; latency climbing = bufferbloat, router QoS / 5 GHz 160 MHz; decode
   time = headset limit) instead of exposing ALVR's buffering / DSCP knobs.
2. **Local quality sweep (no network, ~90 s)**: render -> ALVR foveation -> NVENC at B (CBR) -> recon -> display-space metric,
   ~2 s of frames per configuration, pruned search:
   a. preset curve at today's settings: P1..P5 (quality vs encode ms);
   b. grid resolution {0.8, 1.0, 1.2} x foveation {off, mild 2.5/3, strong 4/5 = today} with the two best presets;
   c. at the leaders: spatial AQ on/off, split encode off / forced (5090: 3 engines; `slices_per_frame` proves it is active).
   Configurations whose encode p95 exceeds 85% of the frame time are not real-time and are dropped.
3. **Network + decode per frame size (HMD, ~60 s)**: the 3-5 best configurations streamed at B (one reconnect per distinct frame
   size, ALVR renegotiates resolution/foveation), measuring network + decode on the real headset.
4. **Proposals** (Pareto front of latency vs quality among the valid configurations):
   - *Lowest latency*: the fastest whose quality is within 2 dB of the best (states what is lost: e.g. "strong foveation, P1").
   - *Recommended*: the knee of the front (most quality per millisecond).
   - *Best quality*: the best quality within +8 ms of the fastest.
   Each with: resolution per eye, foveation, preset, AQ, split, bitrate, latency split (encode / network / decode), quality vs
   today's settings. The choice is written to `session.json` (resolution, foveation, bitrate) and `visionalvr.json` (preset, AQ,
   split: today hardcoded in `nvidia_profile.rs`, they become profile defaults that the benchmark can override).

## Build steps (each testable on the builder without the headset, except 1 and 3)
- S1: scene converter (WSL, offline: Draco + cgltf -> a simple binary `bench/littlest_tokyo.vab`, shipped with attribution),
  host scene renderer, recon/NVDEC readback, display-space metric, phase 2 headless (`alvr_host --benchmark-quality`).
- S2: phase 1 with the decode-capability check (most exists: `--benchmark-network`).
- S3: phase 3, scoring, the configure.exe wizard (three proposals, apply).
- S4: preset / AQ / split read from `visionalvr.json`.

## Third-party code and data (to confirm)
- Draco (Google, Apache-2.0) and cgltf (MIT) only in the offline converter; stb_image (public domain) in the host to decode the
  scene's PNG/JPEG textures. Littlest Tokyo (CC-BY-4.0): attribution in README.txt and in configure.exe's benchmark tab.

## Open questions
- Reconstructed-frame output: capability reported as supported by our D3D11 encoder on the builder (RTX 3080 Ti, driver 617.14:
  `recon_output: 1`); the 5090's `nvenc_config` will say the same or not. NVDEC stays the fallback.
- PSNR is a proxy, not a perceptual metric; SSIM could follow if PSNR ranks configurations implausibly.
- Scripted head motion vs real play: the scripted path keeps phase 2 repeatable; phase 3 uses the real head.

## Status (S1 done, laptop builder)
`alvr_host --benchmark-quality` (headless; `python3 harness/run.py bench-quality quick|full` runs it on the builder and checks it).
Choices made while building it, with what was measured:
- **Content**: Littlest Tokyo alone leaves most of the very wide AVP view as flat sky (free for NVENC). The scene now sits in a
  room (walls / floor / ceiling 4-6 m away, normalized space) tiled with the scene's own painted atlas: detail everywhere, with
  stereo disparity and parallax. Codec-domain PSNR at 250 Mbps went from 46-48 dB to 40-42 dB. The scene's occlusion map is not
  applied (made for ambient light, ~0.2 everywhere), single-sided materials are back-face culled (its outlines are inverted shells).
- **Reconstructed frame**: P010 surface, registered on NVENC's session; colour fit of recon vs input = BT.709 full range (gain
  0.996-0.998, offset ~0), checked per configuration. No NVDEC fallback needed on the laptop (5090 still to confirm).
- **Reference**: the display grid is the session's eye size (what the client resamples to). Reference = the native render
  (`--bq-ref-scale 1`, default). A 2x supersampled reference was tried: it adds a ~41 dB floor (the game's own anti-aliasing) that
  hides codec differences. Consequence: a 1.2x render cannot score above 1x (the client samples bilinear without mips, so extra
  resolution only aliases), and 0.8x costs ~12 dB in the centre.
- **Clocks**: encode times are only comparable at steady clocks. NVML is sampled at 5 Hz per configuration; a GPU keep-alive (60%)
  runs with a 2.5 s ramp. Without it the laptop dropped to P3 (video clock 907 vs 1567 MHz) and encodes were 1.6x slower mid-run.
  `gpu_clocks.stable` (and a general-log warning) flags a run whose NVENC clock varied > 10%.
- **Search**: as planned (a, b, c), 30 configurations in ~75 s. 1.2x without foveation (8512 wide) is refused by NVENC (HEVC
  max width 8192) and reported. Proposals compare quality at 0.1 dB (smaller = noise, faster wins). Latency = encode p95 only until
  S3 adds network + decode.
- **Laptop numbers** (RTX 3080 Ti laptop, 1 NVENC, 250 Mbps, 90 Hz, budget 9.4 ms): weighted PSNR no foveation 39.8 dB / mild 30.5 /
  strong 27.7 at 100%; P1 -> P5 +2.2 dB codec for 8 -> 17 ms. Real time only for P1 at 80% (mild/strong) and 100% strong.
- Open: the weighting (0.3 floor, sigma 0.25 of the eye height) decides how much periphery counts; it makes "80% mild" beat
  "100% strong" here. To check against what the user sees in the headset (phase 3).

# Performance opportunities (noted 2026-10-10, after the v0.2 redesign; none started)

Per frame today (7104x3200 side by side, 10-bit): the game renders two eye images -> the **shim composes** them (one full-resolution
pass into a ring slot, sRGB/gamma handling, optional warp) -> the **host's foveation pass** (ALVR's CompressAxisAligned, a second
full-resolution pass into the encoder's input texture) -> **NVENC input copy** inside ALVR's `Transmit` (a third full-frame GPU
operation) -> encode (3 strips) -> the packet is copied twice on the CPU (callback `to_vec`, ALVR shards) -> UDP. Three full-frame GPU
operations and several CPU hand-offs between the game's last draw and the first encoded byte. The machine has VRAM and RAM to
spare; CPU and GPU time, and above all their *jitter*, are the constraint (Wi-Fi and the headset's decoder are fixed).

Ranked by expected effect on judder first (pacing jitter), latency second, throughput third. Effort is a rough size.

| # | Opportunity | Effect | Effort | Notes |
|---|---|---|---|---|
| 1 | **Realtime GPU scheduling class for the host** (in v0.2, untested) | removes the encoder's queueing behind the game's GPU work (measured +5.7 ms under load) | done, needs the elevated host | the single biggest jitter source we measured; verify `gpu_scheduling` status 0 |
| 2 | **One GPU pass instead of three**: the host's foveation shader reads the game's two eye images directly (shared swapchain textures) and writes the encoder input; the shim stops composing when there is one projection layer and no warp | -2 full-resolution passes (~1-1.5 ms GPU at this size on a 5090) and two fewer queue points; frame ready for the encoder as soon as the game's last draw completes | large (shim exports the swapchain handles + per-frame indices, host-side SRGB/format handling, fallback to the compose path for quads/cubes/warp) | the best structural win; keep the compose path for multi-layer frames |
| 3 | **Zero-copy encoder input**: register the foveation output (or, with #2, the host's single pass output) directly as NVENC input instead of `Transmit`'s copy | -1 full-frame copy (~0.3-0.5 ms) | medium (NvEncoderD3D11 registration of our textures, ring of 2-3 encoder inputs) | VRAM is free, so a dedicated ring is fine |
| 4 | **NVENC asynchronous mode** (completion events) | the encoder thread no longer blocks for the whole encode; encode of frame n overlaps the copy/FFR of n+1 | medium (ALVR's encoder wrapper is synchronous; add output delay 1 or async with events) | mainly latency and thread headroom; pacing already isolated by the staged pipeline |
| 5 | **Cross-process GPU fences instead of CPU completion hops**: ID3D11Fence shared between the game process and the host; the host's pass waits on the GPU for the game's draw, the shim's publisher thread and its DXGI event disappear | -0.2-0.5 ms wake latency per frame and its jitter | medium | removes one time-critical thread and a CPU wakeup from the hot path |
| 6 | **High-resolution waitable timers** (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) in the clock and sender threads instead of sleep-then-spin | frees a CPU core's worth of spinning, same precision | small | CPU is a constraint: the spin loops cost ~1-2 ms of a core per period |
| 7 | **Slice-level streaming** (`enableSubFrameWrite`: send strips as NVENC finishes them) | -2-4 ms latency (send overlaps encode) | large (client decoder must accept partial access units; ALVR's packetizer does not) | latency only, no judder effect; later |
| 8 | **CPU copies on the send path**: reuse buffers in `on_packet`/`pending`, hand ALVR a `Vec` it already owns | -1 to -2 memcpy of ~50 KB per frame | small | negligible time, but fewer allocations on the hot path |
| 9 | **Running-start lead from GPU timestamps** instead of the CPU envelope | tighter release (less idle before the boundary) | small-medium | only once the envelope is measured on the lab |
| 10 | **Pin clocks under light load** (keepalive already exists; `nvidia-smi -lgc` or prefer-max-performance for alvr_host) | avoids NVENC/graphics downclocking when the game is light (measured 3x slower encodes on the laptop) | small, operational | not needed under a heavy game; matters for the lab's light profiles |
| 11 | **Encoder preset / strips**: P1-P2 vs the current preset at 300 Mbps, 3 strips already on | encode time -1-2 ms at some quality cost | small (settings) | measure with the quality benchmark before changing |
| 12 | Audio path: separate socket/QoS marking for audio packets | audio stutter under video bursts | medium (ALVR socket changes) | not a video pacing item |

What NOT to do: more host-side copies "for safety" (VRAM is cheap but every copy is a queue point), prediction/extrapolation on
the shim (measured to add tremble), and smoothing of frames (the headset's own re-presentation is the right fallback).

Suggested order after v0.2 is proven: 1 (verify) -> 6 -> 3 -> 2 -> 5 -> 4; 7, 8, 9, 10, 11, 12 as they come up.

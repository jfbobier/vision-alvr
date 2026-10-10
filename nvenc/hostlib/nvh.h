// C ABI around the unmodified ALVR VideoEncoderNVENC for the Rust host (alvr_host).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*nvh_packet_cb)(void* user, const uint8_t* data, int len, uint64_t ts_ns, int is_idr);
// return 1 and fill outputs when the bitrate/framerate changed (ALVR "dynamic encoder params")
typedef int (*nvh_params_cb)(void* user, uint64_t* bitrate_bps, float* framerate);

// Settings use the openvr_config key names of the ALVR session (e.g. "nvenc_quality_preset"). Call before nvh_create.
// Host-only keys: "idle_rgb" (0xRRGGBB, the frame sent while no app runs), "gpu_thread_priority" (-7..7, 0 = default).
int nvh_set(const char* key, int64_t value);
int nvh_set_f(const char* key, double value);   // foveation_center_size_x/y, foveation_center_shift_x/y, foveation_edge_ratio_x/y
// Size of the encoded frame: ALVR's foveated layout (2 * optimized eye width x optimized eye height) when
// enable_foveated_encoding is set, else 2*eye_w x eye_h. Needs eye_resolution_* and the foveation settings set first.
void nvh_encoded_size(int* w, int* h);
// width/height: encoded frame (SBS). noise: 1 = high-entropy scrolling content, 0 = smooth content.
void* nvh_create(int width, int height, int noise, nvh_packet_cb pcb, nvh_params_cb qcb, void* user, char* err, int errlen);
// Renders the test pattern (frame_index stamped as 16 blocks, bit i of the index, in the top-left 2048x128) and
// encodes it. Packets are delivered synchronously through the callback. Returns encode time in ms (<0 on error).
double nvh_encode(void* h, uint32_t frame_index, uint64_t ts_ns, int force_idr, char* err, int errlen);
void nvh_destroy(void* h);
// JSON description of what NVENC was actually configured with + the GPU's capabilities + the split-encode conditions (for logs).
int nvh_describe(void* h, char* buf, int buflen);
// Writes the current QP delta map ({"w","h","cols","rows","foveated","data":[...]}) to a file. Returns 1 if a map exists.
int nvh_qpmap_dump(const char* path);
// Number of NVENC engines of the GPU (split-frame encoding needs >= 2).
int nvh_encoder_engines(void* h);
// Starts/stops a background GPU load on a second D3D11 device (about `duty_pct` busy) so the driver keeps the GPU in a
// high performance state, as a real compositor/game would. Without it, a light synthetic load lets the laptop GPU
// fall to P8 (NVENC clock 555 MHz vs 1560 MHz), making encodes ~3x slower. Returns 1 on success.
int nvh_keepalive(void* h, int on, int duty_pct);
// The same load without an encoder (benchmarks): returns a handle for nvh_keepalive_stop.
void* nvh_keepalive_start(int duty_pct);
void nvh_keepalive_stop(void* keepalive);
// ---- OVRShim IPC (see ovrshim/ipc.h) -----------------------------------------------------------------------
void* nvh_ipc_open(void);
// Writes the static device description the shim exposes to the OpenXR runtime. fov: per eye {left,right,up,down} tangents.
// encoding_gamma: the negotiated ALVR encoding gamma (openvr_config.encoding_gamma); the shim sends frames as pow(v, 1/gamma).
void nvh_ipc_config(void* ipc, void* nvh, double rate_hz, int eye_w, int eye_h, const float* fov8, const float* eye_offset6, int bits, float encoding_gamma);
void nvh_ipc_set_connected(void* ipc, int connected);
void nvh_ipc_publish_head(void* ipc, uint64_t client_ts_ns, const float* quat_xyzw, const float* pos, const float* lin_vel, const float* ang_vel);
void nvh_ipc_publish_hand(void* ipc, int side, int valid, const float* quat_xyzw, const float* pos, const float* lin_vel, const float* ang_vel);
// interval_s: time until the next tick (ALVR's pacing grid); the shim predicts display times with it.
void nvh_ipc_vsync(void* ipc, double interval_s);
// Controller input (Quest/Touch layout, see ovrshim/ipc.h bit definitions).
void nvh_ipc_publish_input(void* ipc, int side, uint32_t clicks, uint32_t touches, float trigger, float squeeze, float stick_x, float stick_y);
// Latest haptic command the app sent for a hand (seq changes on every ovr_SetControllerVibration).
void nvh_ipc_poll_haptic(void* ipc, int side, uint32_t* seq, float* frequency, float* amplitude);
// Waits up to timeout_ms for a frame from the shim. Returns 1 and the newest frame's info, 0 on timeout.
int nvh_ipc_wait_frame(void* ipc, int timeout_ms, uint64_t* counter, uint32_t* slot, uint64_t* client_ts_ns, double* submit_time_s);
// QPC seconds, the clock the shim stamps frames with.
double nvh_qpc_seconds(void);
// Instrumentation: the shim's compose+GPU-wait time for the last frame, and the GPU time of the last foveation pass (0 if off).
float nvh_ipc_last_compose_ms(void* ipc);
float nvh_last_ffr_gpu_ms(void* nvh);
float nvh_last_ffr_wait_ms(void* nvh);   // foveation pass queued -> done on the GPU (the share of the encode time spent behind the game)
// 1 while an OVR session exists AND its heartbeat advanced within the last 500 ms (a crashed app stops beating).
int nvh_ipc_shim_alive(void* ipc);
// Encodes a solid pure-green SBS frame (what the headset chroma-keys when no OpenXR app is running).
double nvh_encode_green(void* nvh, uint32_t frame_no, uint64_t ts_ns, int force_idr, char* err, int errlen);
// Encodes the shim's shared SBS slot (same adapter) with the unmodified ALVR encoder. Returns encode ms (<0 on error).
double nvh_encode_shared(void* nvh, void* ipc, uint32_t slot, uint32_t frame_no, uint64_t ts_ns, int force_idr, char* err, int errlen);
// Drops the opened shim slots (and their foveation passes): an app that exited leaves ~4 full frames of VRAM otherwise.
void nvh_release_shared(void* nvh);
// Shim diagnostics: submit mode (0 GPU wait on the app thread, 1 publisher thread) and how frame timestamps were chosen.
void nvh_ipc_shim_stats(void* ipc, uint32_t* submit_mode, uint32_t* ts_matched, uint32_t* ts_fallback);
// Ring-slot ownership (ipc.h slotBusyMask): set while the host holds a slot (newest published frame, or being read by the encoder).
void nvh_ipc_slot_busy(void* ipc, uint32_t slot, int busy);
// Compositor pacing shared with the shim: boundary offset after the display tick, running-start margin (0 = release at the tick).
void nvh_ipc_set_pacing(void* ipc, float boundary_offset_ms, float running_start_ms);
// Hand pose offset per side (ipc.h handOffsetQuat/Pos): quat x y z w and position, applied by the shim in the hand's local frame.
void nvh_ipc_set_hand_offset(void* ipc, int side, const float* quat_xyzw, const float* pos);
// Shim pacing diagnostics (see ipc.h ShimToHost).
void nvh_ipc_shim_pacing(void* ipc, uint32_t* slot_busy_waits, float* app_frame_ms, uint64_t* releases, uint64_t* releases_late);
// Process-wide GPU scheduling class (D3DKMT): 0 idle .. 4 high, 5 realtime (needs the increase-base-priority privilege, i.e. an
// elevated host). SteamVR's compositor runs realtime, so its copy/encode never queues behind the game's GPU work. Returns the
// NTSTATUS (0 = success).
long nvh_set_gpu_scheduling(int cls);
void nvh_ipc_close(void* ipc);
// The user's display gamma and the debug session folder (UTF-16, may be null) for the shim.
void nvh_ipc_set_user(void* ipc, float user_gamma, int debug_on, const wchar_t* debug_dir);
// The user's colour correction (0 = neutral each), applied by the shim on the encoded output.
void nvh_ipc_set_color(void* ipc, float brightness, float contrast, float saturation, float sharpening);
// The app's executable name as published by the shim (UTF-8). Returns bytes written incl. the terminator.
int nvh_ipc_app_exe(void* ipc, char* buf, int len);
// NVML: GPU identity as JSON ({"nvml":1,"name","driver","arch","arch_name","vram_mb"}; arch Ada = 8, Blackwell = 10), and one CSV
// sample row (gpu%,mem%,enc%,dec%,graphics MHz,memory MHz,video MHz,power W,temp C,pstate,vram used MB,clock event reasons).
int nvh_gpu_info(char* buf, int len);
int nvh_gpu_sample(char* buf, int len);
// ---- Benchmark quality sweep (bench.cpp) -------------------------------------------------------------------------------------
// The benchmark scene (.vab, tools/bench_scene) rendered in stereo -> ALVR foveation -> NVENC with reconstructed-frame output ->
// the reconstruction un-foveated with the visionOS client's mapping and compared, in display space, with a reference render.
typedef struct NvhBenchConfig {
    int eye_w, eye_h;             // candidate resolution per eye (multiples of 32)
    int ref_w, ref_h;             // reference render per eye: an integer multiple of the display grid (2x, 4x MSAA)
    int disp_w, disp_h;           // display grid per eye the quality is measured on (the headset's recommended size)
    int foveated;                 // ALVR foveated encoding on/off, with its geometry:
    float fov_cs[2], fov_sh[2], fov_er[2];   // center size, center shift, edge ratio (x, y)
    int preset;                   // NVENC P1..P7
    int aq;                       // 0 off, 1 spatial, 2 temporal
    int split;                    // NV_ENC_SPLIT_ENCODE_MODE (0 auto, 1 auto forced, 2 two, 3 three, 15 disabled)
    int qp_map;                   // VisionALVR's foveal QP delta map
    float bitrate_mbps, fps;
    int warmup, frames;           // frames encoded before measuring, frames measured
    int metric_every;             // quality measured on every n-th measured frame
    int metric_stride;            // reference pixels sampled every n (1 = all)
    int measure_quality;          // 0: timing and size only (no recon, no reference render)
    double t0;                    // scene time of the first frame (s)
    float fov[8];                 // per eye {left, right, up, down} tangents (positive)
    float ipd;                    // m
} NvhBenchConfig;
typedef struct NvhBenchResult {
    int enc_w, enc_h;             // the encoded frame
    int recon;                    // 1 if the reconstructed frame was used
    char recon_format[16];
    int luma601;                  // NVENC's RGB->Y matrix found by calibration (0 = BT.709), with Y = gain * Y(rgb) + offset
    float y_gain, y_offset;
    double encode_avg_ms, encode_p50_ms, encode_p95_ms, encode_max_ms;   // foveation pass + NVENC, per frame
    double produced_mbps;         // measured frames' bitstream at fps
    double slices_per_frame;      // > 1 with split encode (or multiple slices)
    int metric_frames;
    double fw_psnr, center_psnr, periphery_psnr;   // display space vs the reference (dB, luma)
    double codec_psnr;            // encoded domain: encoder input vs its reconstruction
    int nvenc_engines;
    char note[256];
    char error[256];
} NvhBenchResult;
void* nvh_bench_open(const char* scene_path, char* err, int errlen);
int nvh_bench_gpu(void* bench, char* buf, int len);
int nvh_bench_preview(void* bench, int eye_w, int eye_h, double t, const float* fov8, float ipd, uint8_t* rgba, int len);
int nvh_bench_run(void* bench, const NvhBenchConfig* cfg, NvhBenchResult* result);
void nvh_bench_close(void* bench);
#ifdef __cplusplus
}
#endif

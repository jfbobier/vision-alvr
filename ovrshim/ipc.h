// IPC contract between OVRShim (LibOVRRT64_1.dll, loaded by VDXR inside the OpenXR app) and alvr_host.
// Both sides open the same named objects (create-or-open); the host fills the "host -> shim" part, the shim the
// "shim -> host" part. Everything is plain old data so both C++ sides can include this header.
//
// Objects (names are prefixed with the session id from env OVRSHIM_SESSION, default "VisionALVR"):
//   <p>_State   file mapping of IpcState
//   <p>_Vsync   auto-reset event, signaled by the host once per display period
//   <p>_Frame   auto-reset event, signaled by the shim after it published a composed frame
// The composed side-by-side frame lives in a small ring of legacy shared D3D11 textures created by the shim;
// their HANDLE values are published in IpcState::slotHandle.
#pragma once
#include <stdint.h>

namespace visionalvr_ipc {

constexpr uint32_t kMagic = 0x56414C31; // 'VAL1'
constexpr uint32_t kVersion = 13;
constexpr int kSlots = 4;

struct Pose {
    float orientation[4]; // x y z w
    float position[3];
};

// Per-hand controller input in the Quest/Touch layout ALVR emulates (after ALVR's own button mapping).
enum : uint32_t {
    kClickPrimary = 1,     // A (right) / X (left)
    kClickSecondary = 2,   // B (right) / Y (left)
    kClickThumbstick = 4,
    kClickMenu = 8,        // left menu
    kClickSystem = 16,     // right system (Apple intercepts the real Home/Meta buttons)
};
enum : uint32_t {
    kTouchPrimary = 1,
    kTouchSecondary = 2,
    kTouchThumbstick = 4,
    kTouchTrigger = 8,
    kTouchThumbrest = 16,
};
struct HandInput {
    uint32_t clicks;
    uint32_t touches;
    float trigger, squeeze, stickX, stickY;
};

struct HostToShim {
    uint32_t hostAlive;        // incremented by the host every vsync (shim detects a dead host)
    uint32_t clientConnected;  // 1 while a headset/mock client is streaming
    uint32_t encodeBits;       // 8 or 10: format of the SBS target
    uint32_t configSeq;        // incremented by the host after every config write (FoV/eye offsets can change while an app runs)
    double displayRateHz;
    int32_t eyeWidth, eyeHeight;   // recommended per-eye render size (the SBS frame is 2*eyeWidth x eyeHeight)
    float eyeFovTan[2][4];         // left, right, up, down (positive tangents) per eye
    float eyeOffset[2][3];         // eye position relative to the head
    uint32_t adapterLuidLow;       // LUID of the GPU the host encodes on (the app must render on it too)
    int32_t adapterLuidHigh;

    // Latest head pose, tagged with the client tracking timestamp it belongs to.
    uint64_t headSeq;              // seqlock: odd while the host is writing
    uint64_t headClientTsNs;       // client tracking timestamp (ALVR "target timestamp")
    double headHostTimeS;          // shim clock (QPC seconds) when the host received the sample
    Pose head;
    float headLinVel[3], headAngVel[3];
    uint32_t handValid[2];
    Pose hand[2];
    float handLinVel[2][3], handAngVel[2][3];
    double nextVsyncTimeS;         // predicted time of the next vsync (shim clock)
    HandInput handInput[2];        // left, right
    double vsyncIntervalS;         // time from the latest tick to the next one, as scheduled by ALVR's pacing clock
    float encodingGamma;           // negotiated ALVR encoding gamma: the client decodes with pow(v, gamma), so frames are sent as pow(v, 1/gamma)
    float userGamma;               // the user's display gamma (VisionALVR.exe slider): > 1 brighter; 0 or 1 = none. Read every frame
    float colorBrightness, colorContrast, colorSaturation, colorSharpening; // user colour correction (0 = neutral), on the final output
    uint32_t debugOn;              // 1: write the verbose shim log into debugDir
    wchar_t debugDir[260];         // the host's debug session folder (logs/debug/<time>), empty when debug is off
    // Frame stamping. 0: stamp each frame with the tracking sample the app rendered it with (pose matching). 1: stamp it with
    // the newest sample and rotate the projection layers from the rendered orientation to that sample's (what SteamVR's
    // compositor does): every frame carries a fresh, distinct timestamp, so the headset shows all of them.
    uint32_t stampMode;            // 2: like 1, and the pose handed to the app is extrapolated to the expected stamp time (smaller warp)
    float freshWaitMs;             // mode 1: when no sample arrived since the previous frame, wait up to this long for one (0 = never)
    float renderScale;             // recommended per-eye render size handed to the app = eyeWidth/Height * renderScale (0 = 1.0); the SBS frame keeps its size
};

struct Haptic {
    uint32_t seq;                  // incremented by the shim on every ovr_SetControllerVibration call
    float frequency, amplitude;
};

struct ShimToHost {
    uint32_t shimAlive;            // set by the shim while an OVR session exists
    uint32_t shimHeartbeat;        // incremented by the shim every display tick (stops if the app dies)
    Haptic haptic[2];              // left, right
    uint64_t frameCounter;         // frames published by the shim
    uint32_t frameSlot;            // ring slot of the latest frame
    uint32_t frameSeq;             // seqlock for the frame fields: odd while the shim is writing them
    uint64_t frameClientTsNs;      // client tracking timestamp of the pose the app rendered that frame with
    double frameSubmitTimeS;       // shim clock when the frame was submitted
    uint64_t slotHandle[kSlots];   // legacy shared texture handles (HANDLE values)
    int32_t sbsWidth, sbsHeight;
    uint32_t sbsFormat;            // DXGI_FORMAT of the SBS slots
    float lastComposeMs;           // compose start -> GPU done for the last published frame (ms)
    uint32_t submitMode;           // 0: SubmitFrame waits for the GPU on the app thread, 1: a shim thread waits and publishes
    uint32_t tsMatched;            // frames whose timestamp was found by matching the layer's RenderPose to a pose the app read
    uint32_t tsFallback;           // frames stamped with the last pose the app read (no projection layer, or no pose matched)
    wchar_t appExe[128];           // file name of the app's executable (for the logs)
};

struct IpcState {
    uint32_t magic;
    uint32_t version;
    HostToShim host;
    ShimToHost shim;
};

} // namespace visionalvr_ipc

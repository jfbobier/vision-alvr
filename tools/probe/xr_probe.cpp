// Minimal D3D11 OpenXR lifecycle probe. Logs every step (flushed) to a file; exit code 0 = PASS.
// Usage: xr_probe.exe <logfile> [frames=60] [timeout_s=30] [pose_csv] [input_csv] [layer_test=0]
// layer_test=1 cycles through composition-layer phases of 90 frames each (see the phase table below).
// Synthetic load (env): XR_PROBE_CPU_MS = busy CPU time per frame before rendering, XR_PROBE_GPU_COPIES = full copies of a
// 4096x4096 RGBA16F texture per frame (GPU time), to model a heavy game (CPU and GPU work that only fit a frame when they overlap).
// XR_PROBE_LOAD shapes both over time: steady (default) | jitter:<pct> (uniform +-pct per frame) | spikes:<every>:<factor> (every
// n-th frame costs factor x) | ramp:<s> (0 -> 2x over s seconds, then back) | burst:<on_s>:<off_s> (full load / none alternating).
// XR_PROBE_TIMING_CSV = per-frame timing (frame, t_wait_return_ms, t_end_ms, load_factor): the game side of the pacing analysis.
// With input_csv it also reads Touch-profile input actions every frame (trigger/squeeze/buttons/thumbstick/grip pose) and
// fires haptics on both hands periodically.
// Each rendered frame stamps (frame_index+1) as 16 white/black 64x64 blocks in the top-left 1024x64 of the left eye
// image plus a mid-gray (linear 0.5 -> sRGB 188) 64x64 patch at x=1024, so a harness can read them back from the
// decoded stream. frame,yaw,x,y,z,predicted_display_time_ns of the head pose go to pose_csv.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <cmath>
#include <cstdarg>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
// XR_PROBE_SCENE=<file.vab>: render the benchmark scene (Littlest Tokyo in its textured room, animated) instead of the flat stamp
// background, with the runtime's view poses: a full OpenXR scene for the pacing lab without a game.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "bench_scene.h"
#include <vector>
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static FILE* g_log;
static void L(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    vfprintf(g_log, fmt, a); fputc('\n', g_log); fflush(g_log);
    va_end(a);
}
static int fail(const char* what, XrResult r) { L("FAIL %s -> %d", what, (int)r); return 1; }
#define CK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) return fail(#x, r_); L("ok   %s", #x); } while (0)

static const char* stateName(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE"; case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED"; case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED"; case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING"; case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "?"; }
}

int main(int argc, char** argv) {
    const char* logPath = argc > 1 ? argv[1] : "xr_probe.log";
    int frames = argc > 2 ? atoi(argv[2]) : 60;
    int timeoutS = argc > 3 ? atoi(argv[3]) : 30;
    const char* poseCsv = argc > 4 ? argv[4] : nullptr;
    FILE* poseF = poseCsv ? fopen(poseCsv, "w") : nullptr;
    if (poseF) fprintf(poseF, "frame,yaw,x,y,z,display_time_ns\n");
    const char* inputCsv = (argc > 5 && strcmp(argv[5], "-") != 0) ? argv[5] : nullptr;
    FILE* inputF = inputCsv ? fopen(inputCsv, "w") : nullptr;
    if (inputF) fprintf(inputF, "frame,display_time_ns,trigger_r,squeeze_l,a_r,x_l,stick_lx,stick_ly,rx,ry,rz,lx,ly,lz,pose_l_valid,pose_r_valid,haptics_sent\n");
    g_log = fopen(logPath, "w");
    if (!g_log) return 2;
    const char* rt = getenv("XR_RUNTIME_JSON");
    L("start frames=%d timeout=%ds XR_RUNTIME_JSON=%s", frames, timeoutS, rt ? rt : "(unset)");
    ULONGLONG t0 = GetTickCount64();

    const int layerMode = argc > 6 ? atoi(argv[6]) : 0;   // 1: quads/cylinder/order, 2: also cube layers
    const bool layerTest = layerMode != 0;
    const bool cubeTest = layerMode == 2;
    std::vector<const char*> ext = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    if (layerTest) { ext.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME); if (cubeTest) ext.push_back(XR_KHR_COMPOSITION_LAYER_CUBE_EXTENSION_NAME); }
    XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy(ici.applicationInfo.applicationName, "xr_probe");
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ici.enabledExtensionCount = (uint32_t)ext.size(); ici.enabledExtensionNames = ext.data();
    XrInstance inst;
    CK(xrCreateInstance(&ici, &inst));
    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    CK(xrGetInstanceProperties(inst, &ip));
    L("runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO }; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    CK(xrGetSystem(inst, &sgi, &sys));
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    CK(xrGetSystemProperties(inst, sys, &sp));
    L("system: %s maxSwapchain %ux%u", sp.systemName, sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);

    XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq;
    CK(xrGetInstanceProcAddr(inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    CK(getReq(inst, sys, &req));

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    IDXGIFactory1* fac; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if (memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) break;
        ad->Release(); ad = nullptr;
    }
    D3D_FEATURE_LEVEL fl = req.minFeatureLevel;
    HRESULT hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, 0, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { L("FAIL D3D11CreateDevice 0x%08x", (unsigned)hr); return 1; }
    L("ok   D3D11CreateDevice");

    Scene scene; bool sceneOn = false;
    ID3D11DepthStencilView* sceneDsv[2] = {};
    UINT sceneDsvW[2] = {}, sceneDsvH[2] = {};
    if (const char* scenePath = getenv("XR_PROBE_SCENE")) {
        std::string err;
        sceneOn = LoadScene(dev, ctx, scenePath, scene, err);
        if (sceneOn && !scene.room.count) MakeRoom(dev, scene);
        L("scene %s: %s", scenePath, sceneOn ? "loaded" : err.c_str());
    }
    // the scene is normalised around the origin (~3 m); the benchmark looks at it from (0, 0.2, 1.9): put it where a standing
    // viewer at the mock headset's height (1.5 m) sees the same picture
    const XMMATRIX scenePlace = XMMatrixTranslation(0.f, 1.3f, -1.9f);

    XrGraphicsBindingD3D11KHR gb{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR }; gb.device = dev;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO }; sci.next = &gb; sci.systemId = sys;
    XrSession sess;
    CK(xrCreateSession(inst, &sci, &sess));

    XrReferenceSpaceCreateInfo rsi{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rsi.poseInReferenceSpace.orientation.w = 1;
    XrSpace space;
    CK(xrCreateReferenceSpace(sess, &rsi, &space));
    XrSpace viewSpace{};
    { XrReferenceSpaceCreateInfo vi{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO }; vi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; vi.poseInReferenceSpace.orientation.w = 1; CK(xrCreateReferenceSpace(sess, &vi, &viewSpace)); }
    int hapticsSent = 0;
    // synthetic load (see the header)
    const double loadCpuMs = getenv("XR_PROBE_CPU_MS") ? atof(getenv("XR_PROBE_CPU_MS")) : 0.0;
    const int loadGpuCopies = getenv("XR_PROBE_GPU_COPIES") ? atoi(getenv("XR_PROBE_GPU_COPIES")) : 0;
    // load pattern (see the header): a per-frame factor applied to both the CPU time and the GPU copies
    std::string loadPattern = getenv("XR_PROBE_LOAD") ? getenv("XR_PROBE_LOAD") : "steady";
    double lpA = 0, lpB = 0;
    { size_t c1 = loadPattern.find(':'); if (c1 != std::string::npos) { lpA = atof(loadPattern.c_str() + c1 + 1); size_t c2 = loadPattern.find(':', c1 + 1); if (c2 != std::string::npos) lpB = atof(loadPattern.c_str() + c2 + 1); loadPattern = loadPattern.substr(0, c1); } }
    uint64_t lpSeed = 0x9E3779B97F4A7C15ull;
    auto loadFactor = [&](int frame, double tSec) -> double {
        if (loadPattern == "jitter") { lpSeed ^= lpSeed << 13; lpSeed ^= lpSeed >> 7; lpSeed ^= lpSeed << 17; const double u = (double)(lpSeed >> 11) / 9007199254740992.0 * 2.0 - 1.0; return 1.0 + u * lpA / 100.0; }
        if (loadPattern == "spikes") { const int every = (int)std::max(1.0, lpA); return (frame % every == every - 1) ? std::max(1.0, lpB) : 1.0; }
        if (loadPattern == "ramp") { const double T = std::max(1.0, lpA); const double ph = fmod(tSec, 2 * T); return ph < T ? 1.0 + ph / T : 3.0 - ph / T; }
        if (loadPattern == "burst") { const double on = std::max(0.1, lpA), off = std::max(0.1, lpB); return fmod(tSec, on + off) < on ? 1.0 : 0.0; }
        return 1.0;
    };
    FILE* timingF = getenv("XR_PROBE_TIMING_CSV") ? fopen(getenv("XR_PROBE_TIMING_CSV"), "w") : nullptr;
    if (timingF) fprintf(timingF, "frame,display_time_ns,t_wait_return_ms,t_end_ms,load_factor\n");
    ID3D11Texture2D* loadTex[2] = {};
    if (loadGpuCopies > 0) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 4096; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&td, nullptr, &loadTex[0]); dev->CreateTexture2D(&td, nullptr, &loadTex[1]);
    }
    L("load cpu_ms=%.2f gpu_copies=%d pattern=%s (%.2f, %.2f)", loadCpuMs, loadGpuCopies, loadPattern.c_str(), lpA, lpB);
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    auto qpcMs = [&] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)qpf.QuadPart; };
    double loopStartMs = 0;

    // ---- input actions (Touch profile), only when an input log was requested
    XrActionSet aset = XR_NULL_HANDLE; XrAction aTrigger{}, aSqueeze{}, aPrimary{}, aStick{}, aPose{}, aHaptic{};
    XrPath hands[2]; XrSpace handSpace[2] = {};
    if (inputF) {
        XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO }; strcpy(asci.actionSetName, "gameplay"); strcpy(asci.localizedActionSetName, "Gameplay");
        CK(xrCreateActionSet(inst, &asci, &aset));
        xrStringToPath(inst, "/user/hand/left", &hands[0]); xrStringToPath(inst, "/user/hand/right", &hands[1]);
        auto mk = [&](const char* name, XrActionType type, XrAction* out) {
            XrActionCreateInfo ai{ XR_TYPE_ACTION_CREATE_INFO }; strcpy(ai.actionName, name); strcpy(ai.localizedActionName, name);
            ai.actionType = type; ai.countSubactionPaths = 2; ai.subactionPaths = hands; return xrCreateAction(aset, &ai, out); };
        CK(mk("trigger", XR_ACTION_TYPE_FLOAT_INPUT, &aTrigger)); CK(mk("squeeze", XR_ACTION_TYPE_FLOAT_INPUT, &aSqueeze));
        CK(mk("primary", XR_ACTION_TYPE_BOOLEAN_INPUT, &aPrimary)); CK(mk("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, &aStick));
        CK(mk("pose", XR_ACTION_TYPE_POSE_INPUT, &aPose)); CK(mk("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, &aHaptic));
        std::vector<XrActionSuggestedBinding> b;
        auto bind = [&](XrAction a, const char* path) { XrPath p; xrStringToPath(inst, path, &p); b.push_back({ a, p }); };
        bind(aTrigger, "/user/hand/left/input/trigger/value"); bind(aTrigger, "/user/hand/right/input/trigger/value");
        bind(aSqueeze, "/user/hand/left/input/squeeze/value"); bind(aSqueeze, "/user/hand/right/input/squeeze/value");
        bind(aPrimary, "/user/hand/left/input/x/click"); bind(aPrimary, "/user/hand/right/input/a/click");
        bind(aStick, "/user/hand/left/input/thumbstick"); bind(aStick, "/user/hand/right/input/thumbstick");
        bind(aPose, "/user/hand/left/input/grip/pose"); bind(aPose, "/user/hand/right/input/grip/pose");
        bind(aHaptic, "/user/hand/left/output/haptic"); bind(aHaptic, "/user/hand/right/output/haptic");
        XrPath prof; xrStringToPath(inst, "/interaction_profiles/oculus/touch_controller", &prof);
        XrInteractionProfileSuggestedBinding sb{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING }; sb.interactionProfile = prof; sb.countSuggestedBindings = (uint32_t)b.size(); sb.suggestedBindings = b.data();
        CK(xrSuggestInteractionProfileBindings(inst, &sb));
    }

    if (inputF) {
        XrSessionActionSetsAttachInfo ai{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO }; ai.countActionSets = 1; ai.actionSets = &aset;
        CK(xrAttachSessionActionSets(sess, &ai));
        for (int h = 0; h < 2; h++) {
            XrActionSpaceCreateInfo si{ XR_TYPE_ACTION_SPACE_CREATE_INFO }; si.action = aPose; si.subactionPath = hands[h]; si.poseInActionSpace.orientation.w = 1;
            CK(xrCreateActionSpace(sess, &si, &handSpace[h]));
        }
    }
    uint32_t n; CK(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr));
    std::vector<XrViewConfigurationView> vcv(n, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    CK(xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, vcv.data()));
    L("views: %u, recommended %ux%u", n, vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);

    CK(xrEnumerateSwapchainFormats(sess, 0, &n, nullptr));
    std::vector<int64_t> fmts(n);
    CK(xrEnumerateSwapchainFormats(sess, n, &n, fmts.data()));
    int64_t fmt = fmts[0];
    for (int64_t f : fmts) if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) fmt = f;
    L("swapchain formats: %u, using %lld", n, (long long)fmt);

    std::vector<XrSwapchain> sc(vcv.size());
    std::vector<std::vector<XrSwapchainImageD3D11KHR>> imgs(vcv.size());
    for (size_t i = 0; i < vcv.size(); i++) {
        XrSwapchainCreateInfo si{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        si.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        si.format = fmt; si.sampleCount = 1; si.faceCount = 1; si.arraySize = 1; si.mipCount = 1;
        si.width = vcv[i].recommendedImageRectWidth; si.height = vcv[i].recommendedImageRectHeight;
        CK(xrCreateSwapchain(sess, &si, &sc[i]));
        uint32_t c; CK(xrEnumerateSwapchainImages(sc[i], 0, &c, nullptr));
        imgs[i].assign(c, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        CK(xrEnumerateSwapchainImages(sc[i], c, &c, (XrSwapchainImageBaseHeader*)imgs[i].data()));
        L("swapchain %zu: %u images", i, c);
    }

    // ---- composition-layer test resources: phases of 90 frames
    //  0 head-locked quad   1 world-locked rotated quad   2 cylinder   3 cube (behind a transparent projection layer)
    //  4 quad for the LEFT eye only   5 alpha-blended quad over an opaque one (sub-rect of the texture)
    struct LayerSwap { XrSwapchain sc{}; std::vector<XrSwapchainImageD3D11KHR> imgs; uint32_t w = 0, h = 0; };
    LayerSwap quadSw, alphaSw, cubeSw;
    auto makeLayerSwap = [&](LayerSwap& ls, uint32_t w, uint32_t h, uint32_t faces) -> int {
        XrSwapchainCreateInfo si{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        si.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        si.format = fmt; si.sampleCount = 1; si.faceCount = faces; si.arraySize = 1; si.mipCount = 1; si.width = w; si.height = h;
        XrResult r2 = xrCreateSwapchain(sess, &si, &ls.sc); if (XR_FAILED(r2)) return fail("xrCreateSwapchain(layer)", r2);
        uint32_t c; xrEnumerateSwapchainImages(ls.sc, 0, &c, nullptr);
        ls.imgs.assign(c, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages(ls.sc, c, &c, (XrSwapchainImageBaseHeader*)ls.imgs.data());
        ls.w = w; ls.h = h; return 0;
    };
    if (layerTest) {
        if (makeLayerSwap(quadSw, 512, 512, 1) || makeLayerSwap(alphaSw, 64, 64, 1) || (cubeTest && makeLayerSwap(cubeSw, 256, 256, 6))) return 1;
        L("layer test swapchains created");
    }
    // draws into the next image of a layer swapchain; fn gets one RTV per face (1 or 6) and the context
    auto drawLayerSwap = [&](LayerSwap& ls, const std::function<void(std::vector<ID3D11RenderTargetView*>&)>& fn) -> int {
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO }; uint32_t idx;
        XrResult r2 = xrAcquireSwapchainImage(ls.sc, &ai, &idx); if (XR_FAILED(r2)) return fail("acquire(layer)", r2);
        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
        r2 = xrWaitSwapchainImage(ls.sc, &wi); if (XR_FAILED(r2)) return fail("wait(layer)", r2);
        std::vector<ID3D11RenderTargetView*> rtvs;
        D3D11_TEXTURE2D_DESC td; ls.imgs[idx].texture->GetDesc(&td);
        for (UINT f = 0; f < td.ArraySize; f++) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = (DXGI_FORMAT)fmt;
            if (td.ArraySize > 1) { rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY; rd.Texture2DArray.FirstArraySlice = f; rd.Texture2DArray.ArraySize = 1; }
            else rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* v = nullptr; dev->CreateRenderTargetView(ls.imgs[idx].texture, &rd, &v); rtvs.push_back(v);
        }
        fn(rtvs);
        for (auto* v : rtvs) if (v) v->Release();
        ctx->Flush();
        XrSwapchainImageReleaseInfo rl{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        r2 = xrReleaseSwapchainImage(ls.sc, &rl); if (XR_FAILED(r2)) return fail("release(layer)", r2);
        return 0;
    };
    auto pose = [](float x, float y, float z, float yawRad) { XrPosef p{}; p.orientation = { 0.f, sinf(yawRad / 2), 0.f, cosf(yawRad / 2) }; p.position = { x, y, z }; return p; };

    bool running = false, exiting = false; int done = 0; bool sawReady = false, sawSync = false;
    int rc = 0;
    while (!exiting && done < frames) {
        if (GetTickCount64() - t0 > (ULONGLONG)timeoutS * 1000) { L("FAIL timeout (done=%d/%d ready=%d)", done, frames, sawReady); rc = 3; break; }
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        XrResult pr = xrPollEvent(inst, &ev);
        if (pr == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* e = (XrEventDataSessionStateChanged*)&ev;
                L("event: session state -> %s", stateName(e->state));
                if (e->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO }; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    CK(xrBeginSession(sess, &bi)); running = true; sawReady = true;
                } else if (e->state == XR_SESSION_STATE_SYNCHRONIZED) sawSync = true;
                else if (e->state == XR_SESSION_STATE_STOPPING) { CK(xrEndSession(sess)); running = false; }
                else if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) exiting = true;
            } else L("event: type %d", (int)ev.type);
            continue;
        }
        if (!running) { Sleep(10); continue; }

        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        XrResult r = xrWaitFrame(sess, nullptr, &fs);
        if (XR_FAILED(r)) return fail("xrWaitFrame", r);
        r = xrBeginFrame(sess, nullptr);
        if (XR_FAILED(r)) return fail("xrBeginFrame", r);
        if (done == 0) loopStartMs = qpcMs();
        const double tWaitRet = qpcMs();
        const double lf = loadFactor(done, (tWaitRet - loopStartMs) / 1000.0);
        if (loadCpuMs > 0 && lf > 0) { const double t = qpcMs(); while (qpcMs() - t < loadCpuMs * lf) YieldProcessor(); }
        if (loadTex[0] && loadTex[1]) { const int n = (int)(loadGpuCopies * lf + 0.5); for (int k = 0; k < n; k++) ctx->CopyResource(loadTex[(k + 1) & 1], loadTex[k & 1]); }
        if (inputF) {
            XrActiveActionSet act{ aset, XR_NULL_PATH }; XrActionsSyncInfo syn{ XR_TYPE_ACTIONS_SYNC_INFO }; syn.countActiveActionSets = 1; syn.activeActionSets = &act;
            xrSyncActions(sess, &syn);
            auto getF = [&](XrAction a, int h) { XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = a; gi.subactionPath = hands[h]; XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT }; xrGetActionStateFloat(sess, &gi, &st); return st.currentState; };
            auto getB = [&](XrAction a, int h) { XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO }; gi.action = a; gi.subactionPath = hands[h]; XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN }; xrGetActionStateBoolean(sess, &gi, &st); return (int)st.currentState; };
            XrActionStateGetInfo gs{ XR_TYPE_ACTION_STATE_GET_INFO }; gs.action = aStick; gs.subactionPath = hands[0]; XrActionStateVector2f sv{ XR_TYPE_ACTION_STATE_VECTOR2F }; xrGetActionStateVector2f(sess, &gs, &sv);
            XrSpaceLocation loc[2] = { { XR_TYPE_SPACE_LOCATION }, { XR_TYPE_SPACE_LOCATION } };
            for (int h = 0; h < 2; h++) xrLocateSpace(handSpace[h], space, fs.predictedDisplayTime, &loc[h]);
            const bool vl = (loc[0].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0, vr = (loc[1].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            // periodic haptics: right hand every 90 frames, left hand offset by 45
            if (done % 90 == 10 || done % 90 == 55) {
                const int h = (done % 90 == 10) ? 1 : 0;
                XrHapticVibration hv{ XR_TYPE_HAPTIC_VIBRATION }; hv.amplitude = h ? 0.8f : 0.5f; hv.frequency = XR_FREQUENCY_UNSPECIFIED; hv.duration = 60000000;
                XrHapticActionInfo hi{ XR_TYPE_HAPTIC_ACTION_INFO }; hi.action = aHaptic; hi.subactionPath = hands[h];
                if (XR_SUCCEEDED(xrApplyHapticFeedback(sess, &hi, (XrHapticBaseHeader*)&hv))) hapticsSent++;
            }
            fprintf(inputF, "%d,%lld,%.5f,%.5f,%d,%d,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%d,%d,%d\n", done + 1, (long long)fs.predictedDisplayTime,
                getF(aTrigger, 1), getF(aSqueeze, 0), getB(aPrimary, 1), getB(aPrimary, 0), sv.currentState.x, sv.currentState.y,
                loc[1].pose.position.x, loc[1].pose.position.y, loc[1].pose.position.z, loc[0].pose.position.x, loc[0].pose.position.y, loc[0].pose.position.z,
                vl, vr, hapticsSent);
            fflush(inputF);
        }

        std::vector<XrCompositionLayerProjectionView> pv(vcv.size(), { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });
        XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        std::vector<const XrCompositionLayerBaseHeader*> lay;
        XrCompositionLayerQuad q1{ XR_TYPE_COMPOSITION_LAYER_QUAD }, q2{ XR_TYPE_COMPOSITION_LAYER_QUAD };
        XrCompositionLayerCylinderKHR cylL{ XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR };
        XrCompositionLayerCubeKHR cubeL{ XR_TYPE_COMPOSITION_LAYER_CUBE_KHR };
        uint32_t nl = 0;
        if (fs.shouldRender) {
            XrViewLocateInfo li{ XR_TYPE_VIEW_LOCATE_INFO };
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = fs.predictedDisplayTime; li.space = space;
            XrViewState vs{ XR_TYPE_VIEW_STATE }; std::vector<XrView> views(vcv.size(), { XR_TYPE_VIEW }); uint32_t vc;
            r = xrLocateViews(sess, &li, &vs, (uint32_t)views.size(), &vc, views.data());
            if (XR_FAILED(r)) return fail("xrLocateViews", r);
            if (done % 20 == 0) L("frame %d: viewState 0x%llx pos(%.3f %.3f %.3f) period %lld ns", done, (unsigned long long)vs.viewStateFlags,
                views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z, (long long)fs.predictedDisplayPeriod);
            if (poseF) {
                const XrQuaternionf q = views[0].pose.orientation;
                const double yaw = atan2(2.0 * (q.w * q.y + q.x * q.z), 1.0 - 2.0 * (q.y * q.y + q.x * q.x));
                fprintf(poseF, "%d,%.6f,%.5f,%.5f,%.5f,%lld\n", done + 1, yaw, views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z, (long long)fs.predictedDisplayTime);
                fflush(poseF);
            }
            SceneFrame sceneFrame;
            if (sceneOn) BeginSceneFrame(scene, ctx, (qpcMs() - loopStartMs) / 1000.0, sceneFrame, scenePlace);
            for (size_t i = 0; i < vcv.size(); i++) {
                XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO }; uint32_t idx;
                r = xrAcquireSwapchainImage(sc[i], &ai, &idx); if (XR_FAILED(r)) return fail("xrAcquireSwapchainImage", r);
                XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
                r = xrWaitSwapchainImage(sc[i], &wi); if (XR_FAILED(r)) return fail("xrWaitSwapchainImage", r);
                // stamp: dark background, 16 counter blocks and a gray patch in the left eye image
                ID3D11RenderTargetView* rtv = nullptr;
                D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = (DXGI_FORMAT)fmt; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                if (SUCCEEDED(dev->CreateRenderTargetView(imgs[i][idx].texture, &rd, &rtv))) {
                    float bg[4] = { 0.02f, 0.02f, 0.02f + (i ? 0.1f : 0.f), 1 };
                    if (cubeTest && (done / 90) % 6 == 3) { bg[0] = bg[1] = bg[2] = bg[3] = 0.f; }   // cube phase: transparent, cube is drawn behind
                    if (sceneOn && i < 2) {
                        D3D11_TEXTURE2D_DESC td{}; imgs[i][idx].texture->GetDesc(&td);
                        if (!sceneDsv[i] || sceneDsvW[i] != td.Width || sceneDsvH[i] != td.Height) {
                            if (sceneDsv[i]) { sceneDsv[i]->Release(); sceneDsv[i] = nullptr; }
                            D3D11_TEXTURE2D_DESC dd{}; dd.Width = td.Width; dd.Height = td.Height; dd.MipLevels = 1; dd.ArraySize = 1;
                            dd.Format = DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count = 1; dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                            ID3D11Texture2D* dt = nullptr;
                            if (SUCCEEDED(dev->CreateTexture2D(&dd, nullptr, &dt))) { dev->CreateDepthStencilView(dt, nullptr, &sceneDsv[i]); dt->Release(); }
                            sceneDsvW[i] = td.Width; sceneDsvH[i] = td.Height;
                        }
                        const float sky[4] = { 0.62f, 0.72f, 0.82f, 1 };
                        ctx->ClearRenderTargetView(rtv, sky);
                        if (sceneDsv[i]) {
                            ctx->ClearDepthStencilView(sceneDsv[i], D3D11_CLEAR_DEPTH, 1, 0);
                            ctx->OMSetRenderTargets(1, &rtv, sceneDsv[i]);
                            const XrPosef& xp = views[i].pose; const XrFovf& xf = views[i].fov;
                            const XMMATRIX pose = XMMatrixRotationQuaternion(XMVectorSet(xp.orientation.x, xp.orientation.y, xp.orientation.z, xp.orientation.w))
                                                * XMMatrixTranslation(xp.position.x, xp.position.y, xp.position.z);
                            const float tan4[4] = { -tanf(xf.angleLeft), tanf(xf.angleRight), tanf(xf.angleUp), -tanf(xf.angleDown) };
                            const XMMATRIX vp = XMMatrixInverse(nullptr, pose) * Projection(tan4, 0.05f, 100.f);
                            D3D11_VIEWPORT v{ 0, 0, (float)td.Width, (float)td.Height, 0, 1 };
                            DrawSceneEye(scene, ctx, sceneFrame, v, vp, scenePlace);
                            ID3D11RenderTargetView* none = nullptr;
                            ctx->OMSetRenderTargets(1, &none, nullptr);
                        }
                    } else {
                        ctx->ClearRenderTargetView(rtv, bg);
                    }
                    ID3D11DeviceContext1* c1 = nullptr;
                    if (i == 0 && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) {
                        const uint32_t stamp = (uint32_t)(done + 1);
                        const float white[4] = { 1, 1, 1, 1 }, black[4] = { 0, 0, 0, 1 }, gray[4] = { 0.5f, 0.5f, 0.5f, 1 };
                        for (int bi = 0; bi < 16; bi++) {
                            D3D11_RECT r{ bi * 64, 0, (bi + 1) * 64, 64 };
                            c1->ClearView(rtv, ((stamp >> bi) & 1) ? white : black, &r, 1);
                        }
                        D3D11_RECT gr{ 1024, 0, 1088, 64 };
                        c1->ClearView(rtv, gray, &gr, 1);
                        c1->Release();
                    }
                    rtv->Release();
                }
                ctx->Flush();
                XrSwapchainImageReleaseInfo rl{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                r = xrReleaseSwapchainImage(sc[i], &rl); if (XR_FAILED(r)) return fail("xrReleaseSwapchainImage", r);
                pv[i].pose = views[i].pose; pv[i].fov = views[i].fov;
                pv[i].subImage.swapchain = sc[i]; pv[i].subImage.imageRect = { {0,0}, {(int)vcv[i].recommendedImageRectWidth, (int)vcv[i].recommendedImageRectHeight} };
            }
            layer.space = space; layer.viewCount = (uint32_t)pv.size(); layer.views = pv.data();
            const XrCompositionLayerBaseHeader* proj = (XrCompositionLayerBaseHeader*)&layer;
            if (!layerTest) {
                lay.push_back(proj);
            } else {
                const int phase = (done / 90) % 6;
                auto drawQuadrants = [&](std::vector<ID3D11RenderTargetView*>& r) {
                    ID3D11DeviceContext1* c1 = nullptr;
                    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) return;
                    const float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 }, white[4] = { 1, 1, 1, 1 };
                    D3D11_RECT tl{ 0, 0, 256, 256 }, tr{ 256, 0, 512, 256 }, bl{ 0, 256, 256, 512 }, br{ 256, 256, 512, 512 };
                    c1->ClearView(r[0], red, &tl, 1); c1->ClearView(r[0], green, &tr, 1); c1->ClearView(r[0], blue, &bl, 1); c1->ClearView(r[0], white, &br, 1);
                    c1->Release();
                };
                auto fillQuad = [&](XrCompositionLayerQuad& q, XrSpace sp, XrPosef ps, float w, float h, const LayerSwap& ls, XrRect2Di rect) {
                    q.space = sp; q.pose = ps; q.size = { w, h }; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    q.subImage.swapchain = ls.sc; q.subImage.imageRect = rect; q.subImage.imageArrayIndex = 0;
                };
                const XrRect2Di full512{ {0, 0}, {512, 512} };
                if (phase == 3 && cubeTest) {
                    // cube faces +X red, -X green, +Y blue, -Y yellow, +Z magenta, -Z cyan; the cube goes first, the (transparent) projection layer on top
                    if (drawLayerSwap(cubeSw, [&](std::vector<ID3D11RenderTargetView*>& r) {
                            const float c[6][4] = { {1,0,0,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1}, {1,0,1,1}, {0,1,1,1} };
                            for (int f = 0; f < 6; f++) ctx->ClearRenderTargetView(r[f], c[f]);
                        })) return 1;
                    cubeL.space = space; cubeL.orientation = { 0, 0, 0, 1 }; cubeL.swapchain = cubeSw.sc; cubeL.imageArrayIndex = 0; cubeL.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    lay.push_back((XrCompositionLayerBaseHeader*)&cubeL);
                    lay.push_back(proj);
                } else {
                    lay.push_back(proj);
                    if (drawLayerSwap(quadSw, drawQuadrants)) return 1;
                    if (phase == 0) fillQuad(q1, viewSpace, pose(0, 0, -2, 0), 1.0f, 1.0f, quadSw, full512);
                    if (phase == 1) fillQuad(q1, space, pose(0.3f, 1.5f, -2.0f, 25.f * 3.14159265f / 180.f), 1.0f, 1.0f, quadSw, full512);
                    if (phase == 3) {   // submission order: a small green quad submitted after a red one covers its center
                        fillQuad(q1, viewSpace, pose(0, 0, -2, 0), 1.0f, 1.0f, quadSw, { {0, 0}, {256, 256} });
                        fillQuad(q2, viewSpace, pose(0, 0, -2, 0), 0.4f, 0.4f, quadSw, { {256, 0}, {256, 256} });
                    }
                    if (phase == 4) { fillQuad(q1, viewSpace, pose(0, 0, -2, 0), 1.0f, 1.0f, quadSw, full512); q1.eyeVisibility = XR_EYE_VISIBILITY_LEFT; }
                    if (phase == 5) {
                        fillQuad(q1, viewSpace, pose(0, 0, -2, 0), 1.0f, 1.0f, quadSw, { {0, 0}, {256, 256} });   // opaque red (sub-rect)
                        if (drawLayerSwap(alphaSw, [&](std::vector<ID3D11RenderTargetView*>& r) { const float c[4] = { 1, 1, 1, 0.5f }; ctx->ClearRenderTargetView(r[0], c); })) return 1;
                        fillQuad(q2, viewSpace, pose(0, 0, -1.9f, 0), 0.5f, 0.5f, alphaSw, { {0, 0}, {64, 64} });  // white, 50% straight alpha
                        q2.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    }
                    if (phase == 2) {
                        cylL.space = space; cylL.pose = pose(0, 1.5f, 0, 0); cylL.radius = 2.5f; cylL.centralAngle = 1.2f; cylL.aspectRatio = 2.0f;
                        cylL.eyeVisibility = XR_EYE_VISIBILITY_BOTH; cylL.subImage.swapchain = quadSw.sc; cylL.subImage.imageRect = full512; cylL.subImage.imageArrayIndex = 0;
                        lay.push_back((XrCompositionLayerBaseHeader*)&cylL);
                    } else {
                        lay.push_back((XrCompositionLayerBaseHeader*)&q1);
                        if (phase == 5 || phase == 3) lay.push_back((XrCompositionLayerBaseHeader*)&q2);
                    }
                }
            }
            nl = (uint32_t)lay.size();
        }
        XrFrameEndInfo fe{ XR_TYPE_FRAME_END_INFO };
        fe.displayTime = fs.predictedDisplayTime; fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; fe.layerCount = nl; fe.layers = lay.data();
        r = xrEndFrame(sess, &fe);
        if (XR_FAILED(r)) return fail("xrEndFrame", r);
        if (timingF) { fprintf(timingF, "%d,%lld,%.3f,%.3f,%.3f\n", done + 1, (long long)fs.predictedDisplayTime, tWaitRet, qpcMs(), lf); if (done % 30 == 0) fflush(timingF); }
        done++;
    }
    L("loop done: frames=%d ready=%d synchronized=%d", done, sawReady, sawSync);
    if (done > 1) L("app fps %.2f over %d frames", (done - 1) * 1000.0 / (qpcMs() - loopStartMs), done - 1);
    if (rc == 0 && done < frames) rc = 4;
    if (running) { xrRequestExitSession(sess); }
    xrDestroySession(sess); xrDestroyInstance(inst);
    L("%s (rc=%d)", rc == 0 ? "PASS" : "FAIL", rc);
    return rc;
}

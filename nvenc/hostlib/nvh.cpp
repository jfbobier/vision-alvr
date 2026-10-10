#define NOMINMAX
#include "nvh.h"
#include "VideoEncoderNVENC.h"
#include "alvr_server/Settings.h"
#include "ALVR-common/exception.h"
#include "shim.h"
#include "ipc_win.h"
#include "FFR.h"
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

static double nowMs() { using namespace std::chrono; return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count(); }

// GPU keep-alive: a second D3D11 device clears/copies 4K textures about `duty` percent of the time
struct KeepAlive {
    std::atomic<bool> keep{false};
    std::thread th;
    void start(int duty) {
        if (keep) return;
        keep = true;
        duty = std::max(5, std::min(95, duty));
        th = std::thread([this, duty]() {
            IDXGIFactory1* fac; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
            IDXGIAdapter1* ad = nullptr; IDXGIAdapter1* pick = nullptr;
            for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) { DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d); if (d.VendorId == 0x10DE && !pick) pick = ad; else ad->Release(); }
            fac->Release();
            if (!pick) return;
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
            if (FAILED(D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, 0, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { pick->Release(); return; }
            D3D11_TEXTURE2D_DESC td{}; td.Width = 4096; td.Height = 4096; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
            ID3D11Texture2D *a = nullptr, *b = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11Query* q = nullptr;
            dev->CreateTexture2D(&td, nullptr, &a); dev->CreateTexture2D(&td, nullptr, &b);
            if (a) dev->CreateRenderTargetView(a, nullptr, &rtv);
            D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 }; dev->CreateQuery(&qd, &q);
            float c = 0;
            while (keep && rtv && q) {
                double t0 = nowMs();
                for (int i = 0; i < 48; i++) { c += 0.01f; float col[4] = { c - (int)c, 0.5f, 0.25f, 1 }; ctx->ClearRenderTargetView(rtv, col); ctx->CopyResource(b, a); }
                ctx->End(q); ctx->Flush();
                BOOL done = FALSE; while (!done && keep) { if (ctx->GetData(q, &done, sizeof(done), 0) != S_OK) Sleep(0); }
                double busy = nowMs() - t0;
                double idle = busy * (100 - duty) / duty;
                if (idle > 0.5) std::this_thread::sleep_for(std::chrono::microseconds((long long)(idle * 1000)));
            }
            if (rtv) rtv->Release(); if (a) a->Release(); if (b) b->Release(); if (q) q->Release();
            ctx->Release(); dev->Release(); pick->Release();
        });
    }
    void stop() { keep = false; if (th.joinable()) th.join(); }
    ~KeepAlive() { stop(); }
};

struct Nvh {
    std::shared_ptr<CD3DRender> render;
    std::unique_ptr<VideoEncoderNVENC> enc;
    ID3D11Texture2D* tex = nullptr;   // encoder source, WxH
    ID3D11Texture2D* base = nullptr;  // (W+PAD)xH scrolling pattern, uploaded once
    int w = 0, h = 0, bits = 8;
    static const int PAD = 512;
    std::vector<uint32_t> counter;    // 2048x128 stamped region
    LUID luid{};
    ID3D11Texture2D* green = nullptr;
    ID3D11Query* qDisjoint = nullptr; ID3D11Query* qT0 = nullptr; ID3D11Query* qT1 = nullptr;
    float lastFfrMs = 0;
    float lastFfrWaitMs = 0;          // CPU time from queuing the foveation pass to its GPU completion (queueing behind the game)
    ID3D11Query* qDone = nullptr;
    std::unique_ptr<FFR> ffr[visionalvr_ipc::kSlots];   // foveated compression of the shim slot (output = encoder input)
    std::string gpuName;
    ID3D11Texture2D* shared[visionalvr_ipc::kSlots] = {};   // opened shim slots (by slot index)
    uint64_t sharedHandle[visionalvr_ipc::kSlots] = {};
    std::unique_ptr<KeepAlive> keepAlive;
    int gpuPriority = 0; long gpuPriorityHr = 0;   // IDXGIDevice::SetGPUThreadPriority on the encoder device (0 = not set)
    std::vector<ID3D11Texture2D*> noiseTex;          // noise == 2: independent random frames (nothing for motion search to reuse)
};

// host options that are not ALVR settings
static uint32_t g_idleRgb = 0x00FF00;   // solid frame sent while no OpenXR app runs (8-bit coded values)
static int g_gpuThreadPriority = 0;     // -7..7, 0 = leave the default
static int g_noiseBlock = 16;           // noise == 2: side of the random blocks (the floor: what is left at the coarsest quantizer)
static int g_noiseAmp = 48;             // noise == 2: per-pixel noise on top, in 10-bit levels (the ceiling: kept at fine quantizers)

int nvh_set(const char* key, int64_t v) {
    auto& S = Settings::Instance();
    static const std::map<std::string, std::function<void(int64_t)>> m = {
        {"refresh_rate", [&](int64_t x) { S.m_refreshRate = (int)x; }},
        {"eye_resolution_width", [&](int64_t x) { S.m_renderWidth = (uint32_t)x * 2; }},
        {"eye_resolution_height", [&](int64_t x) { S.m_renderHeight = (uint32_t)x; }},
        {"enable_foveated_encoding", [&](int64_t x) { S.m_enableFoveatedEncoding = x != 0; }},
        {"codec", [&](int64_t x) { S.m_codec = (int)x; }},
        {"use_10bit_encoder", [&](int64_t x) { S.m_use10bitEncoder = x != 0; }},
        {"use_full_range_encoding", [&](int64_t x) { S.m_useFullRangeEncoding = x != 0; }},
        {"enable_hdr", [&](int64_t x) { S.m_enableHdr = x != 0; }},
        {"nvenc_quality_preset", [&](int64_t x) { S.m_nvencQualityPreset = (uint32_t)x; }},
        {"rate_control_mode", [&](int64_t x) { S.m_rateControlMode = (uint32_t)x; }},
        {"filler_data", [&](int64_t x) { S.m_fillerData = x != 0; }},
        {"entropy_coding", [&](int64_t x) { S.m_entropyCoding = (uint32_t)x; }},
        {"nvenc_tuning_preset", [&](int64_t x) { S.m_nvencTuningPreset = (uint32_t)x; }},
        {"nvenc_multi_pass", [&](int64_t x) { S.m_nvencMultiPass = (uint32_t)x; }},
        {"nvenc_adaptive_quantization_mode", [&](int64_t x) { S.m_nvencAdaptiveQuantizationMode = (uint32_t)x; }},
        {"nvenc_low_delay_key_frame_scale", [&](int64_t x) { S.m_nvencLowDelayKeyFrameScale = x; }},
        {"nvenc_refresh_rate", [&](int64_t x) { S.m_nvencRefreshRate = x; }},
        {"enable_intra_refresh", [&](int64_t x) { S.m_nvencEnableIntraRefresh = x != 0; }},
        {"intra_refresh_period", [&](int64_t x) { S.m_nvencIntraRefreshPeriod = x; }},
        {"intra_refresh_count", [&](int64_t x) { S.m_nvencIntraRefreshCount = x; }},
        {"max_num_ref_frames", [&](int64_t x) { S.m_nvencMaxNumRefFrames = x; }},
        {"gop_length", [&](int64_t x) { S.m_nvencGopLength = x; }},
        {"p_frame_strategy", [&](int64_t x) { S.m_nvencPFrameStrategy = x; }},
        {"nvenc_rate_control_mode", [&](int64_t x) { S.m_nvencRateControlMode = x; }},
        {"rc_buffer_size", [&](int64_t x) { S.m_nvencRcBufferSize = x; }},
        {"rc_initial_delay", [&](int64_t x) { S.m_nvencRcInitialDelay = x; }},
        {"rc_max_bitrate", [&](int64_t x) { S.m_nvencRcMaxBitrate = x; }},
        {"rc_average_bitrate", [&](int64_t x) { S.m_nvencRcAverageBitrate = x; }},
        {"nvenc_enable_weighted_prediction", [&](int64_t x) { S.m_nvencEnableWeightedPrediction = x != 0; }},
        {"nvenc_split_encode_mode", [&](int64_t x) { S.m_nvencSplitEncodeMode = (uint32_t)x; }},
        {"nvenc_qp_map", [&](int64_t x) { S.m_qpMapEnabled = x != 0; }},
        {"qp_map_center_delta", [&](int64_t x) { S.m_qpMapCenterDelta = (int)x; }},
        {"qp_map_edge_delta", [&](int64_t x) { S.m_qpMapEdgeDelta = (int)x; }},
        {"idle_rgb", [&](int64_t x) { g_idleRgb = (uint32_t)x & 0xFFFFFF; }},
        {"gpu_thread_priority", [&](int64_t x) { g_gpuThreadPriority = (int)std::max<int64_t>(-7, std::min<int64_t>(7, x)); }},
        {"noise_block", [&](int64_t x) { g_noiseBlock = (int)std::max<int64_t>(1, std::min<int64_t>(64, x)); }},
        {"noise_amp", [&](int64_t x) { g_noiseAmp = (int)std::max<int64_t>(0, std::min<int64_t>(512, x)); }},
    };
    auto it = m.find(key);
    if (it == m.end()) return 0;
    it->second(v);
    return 1;
}

int nvh_set_f(const char* key, double v) {
    auto& S = Settings::Instance();
    std::string k = key;
    if (k == "foveation_center_size_x") S.m_foveationCenterSizeX = (float)v;
    else if (k == "foveation_center_size_y") S.m_foveationCenterSizeY = (float)v;
    else if (k == "foveation_center_shift_x") S.m_foveationCenterShiftX = (float)v;
    else if (k == "foveation_center_shift_y") S.m_foveationCenterShiftY = (float)v;
    else if (k == "foveation_edge_ratio_x") S.m_foveationEdgeRatioX = (float)v;
    else if (k == "foveation_edge_ratio_y") S.m_foveationEdgeRatioY = (float)v;
    else if (k == "qp_map_transition") S.m_qpMapTransition = (float)v;
    else return 0;
    return 1;
}

void nvh_encoded_size(int* w, int* h) {
    auto& S = Settings::Instance();
    if (S.m_enableFoveatedEncoding) {
        uint32_t ow, oh;
        FFR(nullptr).GetOptimizedResolution(&ow, &oh);
        *w = (int)ow; *h = (int)oh;
    } else {
        *w = (int)S.m_renderWidth; *h = (int)S.m_renderHeight;
    }
}

static void seterr(char* err, int n, const char* s) { if (err && n > 0) { strncpy(err, s, n - 1); err[n - 1] = 0; } }


// ---- QP delta map ------------------------------------------------------------------------------------------------------------
// One int8 per 32x32 CTB (HEVC), raster order. The delta follows the distance from the foveal region measured in SOURCE space (i.e.
// through ALVR's foveation warp when it is on), so the periphery gets a higher QP and the center a lower one.
std::vector<int8_t> g_qpMap;
static int g_qpCols = 0, g_qpRows = 0, g_qpW = 0, g_qpH = 0; static bool g_qpFoveated = false;
extern "C" { int8_t* g_nvh_qp_map = nullptr; uint32_t g_nvh_qp_map_size = 0; }

// the compress shader's output->source mapping for one axis (same as harness/lib.py _fov_f)
static float foveF(float e, float cs, float sh, float er) {
    float c0 = (1.f - cs) / 2.f, c1 = (er - 1.f) * c0 * (sh + 1.f) / er, c2 = (er - 1.f) * cs + 1.f;
    float lo = c0 * (sh + 1.f) / c2, hi = c0 * (sh - 1.f) / c2 + 1.f;
    float center = e * c2 / er + c1, d2 = e * c2, d3 = (e - 1.f) * c2 + 1.f;
    if (e < lo) { float g1 = e / lo; return g1 * center + (1.f - g1) * d2; }
    if (e > hi) { float g2 = (1.f - e) / (1.f - hi); return g2 * center + (1.f - g2) * d3; }
    return center;
}

struct QpAxis { float cs, sh, er, ratio, lo, hi, scMid, scHalf; };
static QpAxis qpAxis(float cs, float sh, float er, float ratio) {
    QpAxis a{ cs, sh, er, ratio, 0, 0, 0, 0 };
    float c0 = (1.f - cs) / 2.f, c2 = (er - 1.f) * cs + 1.f;
    a.lo = c0 * (sh + 1.f) / c2; a.hi = c0 * (sh - 1.f) / c2 + 1.f;
    float s0 = foveF(a.lo, cs, sh, er), s1 = foveF(a.hi, cs, sh, er);
    a.scMid = (s0 + s1) / 2.f; a.scHalf = std::max(1e-4f, (s1 - s0) / 2.f);
    return a;
}

void nvh_build_qp_map(int W, int H) {
    auto& S = Settings::Instance();
    g_qpMap.clear(); g_nvh_qp_map = nullptr; g_nvh_qp_map_size = 0;
    if (!S.m_qpMapEnabled || W <= 0 || H <= 0) return;
    g_qpW = W; g_qpH = H; g_qpCols = (W + 31) / 32; g_qpRows = (H + 31) / 32;
    g_qpFoveated = S.m_enableFoveatedEncoding;
    float cx = S.m_foveationCenterSizeX, cy = S.m_foveationCenterSizeY, sx = S.m_foveationCenterShiftX, sy = S.m_foveationCenterShiftY;
    float rx = 1.f, ry = 1.f, ratioX = 1.f, ratioY = 1.f;
    if (g_qpFoveated) {   // ALVR's FFR.cpp CalculateFoveationVars, copied verbatim (its float/double mix decides the frame size)
        float targetEyeWidth = (float)S.m_renderWidth / 2;
        float targetEyeHeight = (float)S.m_renderHeight;
        float centerSizeX = (float)S.m_foveationCenterSizeX, centerSizeY = (float)S.m_foveationCenterSizeY;
        float centerShiftX = (float)S.m_foveationCenterShiftX, centerShiftY = (float)S.m_foveationCenterShiftY;
        float edgeRatioX = (float)S.m_foveationEdgeRatioX, edgeRatioY = (float)S.m_foveationEdgeRatioY;
        float edgeSizeX = targetEyeWidth - centerSizeX * targetEyeWidth;
        float edgeSizeY = targetEyeHeight - centerSizeY * targetEyeHeight;
        float centerSizeXAligned = 1. - ceil(edgeSizeX / (edgeRatioX * 2.)) * (edgeRatioX * 2.) / targetEyeWidth;
        float centerSizeYAligned = 1. - ceil(edgeSizeY / (edgeRatioY * 2.)) * (edgeRatioY * 2.) / targetEyeHeight;
        float edgeSizeXAligned = targetEyeWidth - centerSizeXAligned * targetEyeWidth;
        float edgeSizeYAligned = targetEyeHeight - centerSizeYAligned * targetEyeHeight;
        float centerShiftXAligned = ceil(centerShiftX * edgeSizeXAligned / (edgeRatioX * 2.)) * (edgeRatioX * 2.) / edgeSizeXAligned;
        float centerShiftYAligned = ceil(centerShiftY * edgeSizeYAligned / (edgeRatioY * 2.)) * (edgeRatioY * 2.) / edgeSizeYAligned;
        float foveationScaleX = (centerSizeXAligned + (1. - centerSizeXAligned) / edgeRatioX);
        float foveationScaleY = (centerSizeYAligned + (1. - centerSizeYAligned) / edgeRatioY);
        float optimizedEyeWidth = foveationScaleX * targetEyeWidth;
        float optimizedEyeHeight = foveationScaleY * targetEyeHeight;
        auto optimizedEyeWidthAligned = (uint32_t)ceil(optimizedEyeWidth / 32.f) * 32;
        auto optimizedEyeHeightAligned = (uint32_t)ceil(optimizedEyeHeight / 32.f) * 32;
        float eyeWidthRatioAligned = optimizedEyeWidth / optimizedEyeWidthAligned;
        float eyeHeightRatioAligned = optimizedEyeHeight / optimizedEyeHeightAligned;
        rx = edgeRatioX; ry = edgeRatioY; cx = centerSizeXAligned; cy = centerSizeYAligned; sx = centerShiftXAligned; sy = centerShiftYAligned;
        ratioX = eyeWidthRatioAligned; ratioY = eyeHeightRatioAligned;
    }
    QpAxis ax = qpAxis(cx, sx, rx, ratioX), ay = qpAxis(cy, sy, ry, ratioY);
    const float eyeW = W / 2.f;
    const int C = S.m_qpMapCenterDelta, E = S.m_qpMapEdgeDelta; const float T = std::max(1e-3f, S.m_qpMapTransition);
    g_qpMap.assign((size_t)g_qpCols * g_qpRows, 0);
    for (int j = 0; j < g_qpRows; j++) for (int i = 0; i < g_qpCols; i++) {
        float px = std::min((i + 0.5f) * 32.f, (float)W - 0.5f), py = std::min((j + 0.5f) * 32.f, (float)H - 0.5f);
        float pe = px < eyeW ? px / eyeW : (W - px) / eyeW;     // eye-local x (the right eye is mirrored in the frame)
        float ex = pe / ratioX, ey = (py / H) / ratioY;
        float sxs = foveF(ex, ax.cs, ax.sh, ax.er), sys = foveF(ey, ay.cs, ay.sh, ay.er);
        float dx = (sxs - ax.scMid) / ax.scHalf, dy = (sys - ay.scMid) / ay.scHalf;
        float d = sqrtf(dx * dx + dy * dy);
        float t = std::min(1.f, std::max(0.f, (d - 1.f) / T)); t = t * t * (3.f - 2.f * t);
        int v = (int)lroundf((float)C + (float)(E - C) * t);
        g_qpMap[(size_t)j * g_qpCols + i] = (int8_t)std::max(-51, std::min(51, v));
    }
    g_nvh_qp_map = g_qpMap.data(); g_nvh_qp_map_size = (uint32_t)g_qpMap.size();
}

int nvh_qpmap_dump(const char* path) {
    if (g_qpMap.empty()) return 0;
    FILE* f = nullptr; if (fopen_s(&f, path, "wb") || !f) return 0;
    fprintf(f, "{\"w\":%d,\"h\":%d,\"cols\":%d,\"rows\":%d,\"foveated\":%d,\"data\":[", g_qpW, g_qpH, g_qpCols, g_qpRows, g_qpFoveated ? 1 : 0);
    for (size_t k = 0; k < g_qpMap.size(); k++) fprintf(f, "%s%d", k ? "," : "", (int)g_qpMap[k]);
    fprintf(f, "]}\n"); fclose(f); return 1;
}

void* nvh_create(int W, int H, int noise, nvh_packet_cb pcb, nvh_params_cb qcb, void* user, char* err, int errlen) {
    g_shim.packet_cb = (decltype(g_shim.packet_cb))nullptr;
    g_shim.user = user;
    static nvh_packet_cb s_pcb; static nvh_params_cb s_qcb;
    s_pcb = pcb; s_qcb = qcb;
    g_shim.packet_cb = [](void* u, const unsigned char* d, int l, unsigned long long ts, bool idr) { s_pcb(u, d, l, ts, idr ? 1 : 0); };
    g_shim.params_cb = [](void* u, unsigned long long* br, float* fr) -> bool { uint64_t b = 0; float f = 0; int r = s_qcb ? s_qcb(u, &b, &f) : 0; *br = b; *fr = f; return r != 0; };

    auto* n = new Nvh();
    n->w = W; n->h = H; n->bits = Settings::Instance().m_use10bitEncoder ? 10 : 8;

    IDXGIFactory1* fac; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
    int adapter = -1; IDXGIAdapter1* ad;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) { DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d); ad->Release(); if (d.VendorId == 0x10DE && adapter < 0) { adapter = (int)i; n->luid = d.AdapterLuid; char nm[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, sizeof nm, nullptr, nullptr); n->gpuName = nm; } }
    fac->Release();
    if (adapter < 0) { seterr(err, errlen, "no NVIDIA adapter"); delete n; return nullptr; }
    n->render = std::make_shared<CD3DRender>();
    if (!n->render->Initialize((uint32_t)adapter)) { seterr(err, errlen, "CD3DRender init failed"); delete n; return nullptr; }
    if (g_gpuThreadPriority != 0) {
        // GPU scheduling priority of the encoder device (FFR pass + NVENC input copy), so a GPU-bound game does not queue ahead of it
        IDXGIDevice* dx = nullptr;
        if (SUCCEEDED(n->render->GetDevice()->QueryInterface(__uuidof(IDXGIDevice), (void**)&dx))) {
            n->gpuPriorityHr = (long)dx->SetGPUThreadPriority(g_gpuThreadPriority);
            dx->Release();
        }
        n->gpuPriority = g_gpuThreadPriority;
    }

    DXGI_FORMAT fmt = n->bits == 10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    auto dev = n->render->GetDevice();
    D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &n->tex))) { seterr(err, errlen, "CreateTexture2D(tex) failed"); delete n; return nullptr; }

    // scrolling base image: gradient + checker (+ optional hash noise)
    const int BW = W + Nvh::PAD;
    std::vector<uint32_t> img((size_t)BW * H);
    for (int y = 0; y < H; y++) for (int x = 0; x < BW; x++) {
        int nz = 0;
        if (noise) { uint32_t hh = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u; hh = (hh ^ (hh >> 13)) * 1274126177u; hh ^= hh >> 16; nz = (hh & 0xff) >> 3; }
        int r = std::min(255, x * 255 / BW + nz), g = std::min(255, y * 255 / H + nz), b = std::min(255, (((x / 64 + y / 64) & 1) ? 200 : 60) + nz);
        img[(size_t)y * BW + x] = n->bits == 10 ? ((r * 4) | ((g * 4) << 10) | ((b * 4) << 20) | (3u << 30)) : (r | (g << 8) | (b << 16) | (255u << 24));
    }
    td.Width = BW;
    D3D11_SUBRESOURCE_DATA sd{ img.data(), (UINT)(BW * 4), 0 };
    if (FAILED(dev->CreateTexture2D(&td, &sd, &n->base))) { seterr(err, errlen, "CreateTexture2D(base) failed"); delete n; return nullptr; }
    n->counter.assign(2048 * 128, 0);
    if (noise == 2) {
        // Benchmarks: frames of independent random noise, cycled. Inter prediction finds nothing to reuse, so every frame costs
        // the full bitrate (a scrolling pattern compresses to almost nothing with motion compensation, even if it is noisy).
        td.Width = W;
        std::vector<uint32_t> px((size_t)W * H);
        uint32_t st = 0x9E3779B9u;
        const int B = g_noiseBlock;
        const int bw = (W + B - 1) / B, bh = (H + B - 1) / B;
        std::vector<uint32_t> blocks((size_t)bw * bh);
        for (int k = 0; k < 6; k++) {
            for (auto& p : blocks) {
                st ^= st << 13; st ^= st >> 17; st ^= st << 5;
                p = n->bits == 10 ? ((st & 0x3FF) | (((st >> 10) & 0x3FF) << 10) | (((st >> 20) & 0x3FF) << 20) | (3u << 30)) : (st | 0xFF000000u);
            }
            // coarse random blocks + fine per-pixel noise: a frame the encoder cannot predict from the previous one, whose cost spans
            // a wide bitrate range (rate control can hit any target from ~tens to hundreds of Mbps)
            const int A = n->bits == 10 ? g_noiseAmp : std::max(1, g_noiseAmp / 4), M = n->bits == 10 ? 1023 : 255, S = n->bits == 10 ? 10 : 8;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                const uint32_t b = blocks[(size_t)(y / B) * bw + x / B];
                uint32_t out = 0;
                for (int c = 0; c < 3; c++) {
                    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
                    int v = (int)((b >> (c * S)) & (uint32_t)M) + (A ? (int)(st % (2 * A + 1)) - A : 0);
                    out |= (uint32_t)std::max(0, std::min(M, v)) << (c * S);
                }
                px[(size_t)y * W + x] = out | (n->bits == 10 ? (3u << 30) : 0xFF000000u);
            }
            D3D11_SUBRESOURCE_DATA nd{ px.data(), (UINT)(W * 4), 0 };
            ID3D11Texture2D* t = nullptr;
            if (FAILED(dev->CreateTexture2D(&td, &nd, &t))) { seterr(err, errlen, "CreateTexture2D(noise) failed"); delete n; return nullptr; }
            n->noiseTex.push_back(t);
        }
    }

    nvh_build_qp_map(W, H);
    n->enc = std::make_unique<VideoEncoderNVENC>(n->render, W, H);
    try { n->enc->Initialize(); } catch (Exception& e) { seterr(err, errlen, e.what()); delete n; return nullptr; }
    catch (std::exception& e) { seterr(err, errlen, e.what()); delete n; return nullptr; }
    return n;
}

double nvh_encode(void* hh, uint32_t idx, uint64_t ts_ns, int force_idr, char* err, int errlen) {
    auto* n = (Nvh*)hh;
    auto ctx = n->render->GetContext();
    if (!n->noiseTex.empty()) {
        ctx->CopyResource(n->tex, n->noiseTex[idx % n->noiseTex.size()]);
    } else {
        int off = (int)((idx * 7u) % Nvh::PAD);
        D3D11_BOX box{ (UINT)off, 0, 0, (UINT)(off + n->w), (UINT)n->h, 1 };
        ctx->CopySubresourceRegion(n->tex, 0, 0, 0, 0, n->base, 0, &box);
    }
    const uint32_t white = n->bits == 10 ? (1023u | (1023u << 10) | (1023u << 20) | (3u << 30)) : 0xffffffffu;
    const uint32_t black = n->bits == 10 ? (3u << 30) : 0xff000000u;
    for (int b = 0; b < 16; b++) {
        uint32_t c = ((idx >> b) & 1) ? white : black;
        for (int y = 0; y < 128; y++) for (int x = b * 128; x < (b + 1) * 128; x++) n->counter[(size_t)y * 2048 + x] = c;
    }
    D3D11_BOX cb{ 0, 0, 0, 2048, 128, 1 };
    ctx->UpdateSubresource(n->tex, 0, &cb, n->counter.data(), 2048 * 4, 0);
    double a = nowMs();
    try { n->enc->Transmit(n->tex, idx, ts_ns, force_idr != 0); }
    catch (Exception& e) { seterr(err, errlen, e.what()); return -1; }
    catch (std::exception& e) { seterr(err, errlen, e.what()); return -1; }
    return nowMs() - a;
}

int nvh_keepalive(void* hh, int on, int duty) {
    auto* n = (Nvh*)hh;
    if (!n->keepAlive) n->keepAlive = std::make_unique<KeepAlive>();
    if (on) n->keepAlive->start(duty); else n->keepAlive->stop();
    return 1;
}

void* nvh_keepalive_start(int duty) { auto* k = new KeepAlive(); k->start(duty); return k; }
void nvh_keepalive_stop(void* k) { delete (KeepAlive*)k; }

// ---------------------------------------------------------------------------------------------------------
using visionalvr_ipc::Ipc;

void* nvh_ipc_open(void) {
    auto* ipc = new Ipc();
    if (!visionalvr_ipc::OpenIpc(*ipc)) { delete ipc; return nullptr; }
    return ipc;
}

void nvh_ipc_config(void* i, void* hh, double rate, int ew, int eh, const float* fov8, const float* off6, int bits, float encoding_gamma) {
    auto* ipc = (Ipc*)i; auto* n = (Nvh*)hh;
    auto& h = ipc->state->host;
    h.displayRateHz = rate; h.eyeWidth = ew; h.eyeHeight = eh; h.encodeBits = bits; h.encodingGamma = encoding_gamma;
    for (int e = 0; e < 2; e++) {
        for (int k = 0; k < 4; k++) h.eyeFovTan[e][k] = fov8[e * 4 + k];
        for (int k = 0; k < 3; k++) h.eyeOffset[e][k] = off6[e * 3 + k];
    }
    if (n) { h.adapterLuidLow = n->luid.LowPart; h.adapterLuidHigh = n->luid.HighPart; }
    MemoryBarrier();
    h.configSeq++;
}

void nvh_ipc_set_connected(void* i, int c) { ((Ipc*)i)->state->host.clientConnected = c ? 1 : 0; }

void nvh_ipc_publish_head(void* i, uint64_t ts, const float* q, const float* p, const float* lv, const float* av) {
    auto& h = ((Ipc*)i)->state->host;
    h.headSeq++; MemoryBarrier();                      // odd: writing
    h.headClientTsNs = ts; h.headHostTimeS = visionalvr_ipc::QpcSeconds();
    memcpy(h.head.orientation, q, 16); memcpy(h.head.position, p, 12); memcpy(h.headLinVel, lv, 12); memcpy(h.headAngVel, av, 12);
    MemoryBarrier(); h.headSeq++;                      // even: stable
}

void nvh_ipc_publish_hand(void* i, int side, int valid, const float* q, const float* p, const float* lv, const float* av) {
    auto& h = ((Ipc*)i)->state->host;
    h.headSeq++; MemoryBarrier();                      // same seqlock as the head: the shim never sees a torn hand pose
    if (valid) { memcpy(h.hand[side].orientation, q, 16); memcpy(h.hand[side].position, p, 12); memcpy(h.handLinVel[side], lv, 12); memcpy(h.handAngVel[side], av, 12); }
    h.handValid[side] = valid ? 1 : 0;
    MemoryBarrier(); h.headSeq++;
}

void nvh_ipc_vsync(void* i, double interval_s) {
    auto* ipc = (Ipc*)i;
    ipc->state->host.vsyncIntervalS = interval_s;
    ipc->state->host.nextVsyncTimeS = visionalvr_ipc::QpcSeconds();
    ipc->state->host.hostAlive++;
    SetEvent(ipc->vsync);
}

void nvh_ipc_publish_input(void* i, int side, uint32_t clicks, uint32_t touches, float trig, float sq, float sx, float sy) {
    auto& in = ((Ipc*)i)->state->host.handInput[side];
    in.clicks = clicks; in.touches = touches; in.trigger = trig; in.squeeze = sq; in.stickX = sx; in.stickY = sy;
}

void nvh_ipc_poll_haptic(void* i, int side, uint32_t* seq, float* freq, float* amp) {
    const auto& h = ((Ipc*)i)->state->shim.haptic[side];
    *seq = h.seq; MemoryBarrier(); *freq = h.frequency; *amp = h.amplitude;
}

double nvh_qpc_seconds(void) { return visionalvr_ipc::QpcSeconds(); }
float nvh_ipc_last_compose_ms(void* i) { return ((Ipc*)i)->state->shim.lastComposeMs; }
float nvh_last_ffr_gpu_ms(void* hh) { return ((Nvh*)hh)->lastFfrMs; }
float nvh_last_ffr_wait_ms(void* hh) { return ((Nvh*)hh)->lastFfrWaitMs; }

int nvh_ipc_wait_frame(void* i, int timeout_ms, uint64_t* counter, uint32_t* slot, uint64_t* ts, double* submit) {
    auto* ipc = (Ipc*)i;
    static uint64_t last = 0;
    auto& s = ipc->state->shim;
    auto now = [&] { return *(volatile uint64_t*)&s.frameCounter; };
    // A new app's shim resets the counter to 0 when it creates its ring; until its first frame the other fields still describe
    // the previous app's last frame (slot, timestamp), so 0 means "nothing yet", and the next app's frames count from 1 again.
    if (now() == 0) last = 0;
    if (now() == last || now() == 0) {
        if (WaitForSingleObject(ipc->frame, timeout_ms < 0 ? 0 : timeout_ms) != WAIT_OBJECT_0 && (now() == last || now() == 0)) return 0;
    }
    MemoryBarrier();
    if (now() == last || now() == 0) return 0;
    // seqlock: the shim may start publishing the next frame while we read; all fields must belong to one frame
    for (;;) {
        uint32_t q1 = *(volatile uint32_t*)&s.frameSeq;
        if (q1 & 1) { YieldProcessor(); continue; }
        MemoryBarrier();
        *counter = s.frameCounter; *slot = s.frameSlot; *ts = s.frameClientTsNs; *submit = s.frameSubmitTimeS;
        MemoryBarrier();
        if (*(volatile uint32_t*)&s.frameSeq == q1) break;
    }
    if (*counter == 0) return 0;
    last = *counter;
    return 1;
}

int nvh_ipc_shim_alive(void* i) {
    auto& s = ((Ipc*)i)->state->shim;
    static uint32_t lastBeat = 0; static double lastChange = 0;
    const double now = visionalvr_ipc::QpcSeconds();
    if (s.shimHeartbeat != lastBeat) { lastBeat = s.shimHeartbeat; lastChange = now; }
    return (s.shimAlive && now - lastChange < 0.5) ? 1 : 0;
}

double nvh_encode_green(void* hh, uint32_t frame_no, uint64_t ts_ns, int force_idr, char* err, int errlen) {
    auto* n = (Nvh*)hh;
    if (!n->green) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = n->w; td.Height = n->h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = n->bits == 10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        auto dev = n->render->GetDevice();
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &n->green)) || FAILED(dev->CreateRenderTargetView(n->green, nullptr, &rtv))) { seterr(err, errlen, "green texture"); return -1; }
        const float g[4] = { ((g_idleRgb >> 16) & 0xFF) / 255.f, ((g_idleRgb >> 8) & 0xFF) / 255.f, (g_idleRgb & 0xFF) / 255.f, 1.f };
        n->render->GetContext()->ClearRenderTargetView(rtv, g);
        rtv->Release();
    }
    double a = nowMs();
    try { n->enc->Transmit(n->green, frame_no, ts_ns, force_idr != 0); }
    catch (Exception& e) { seterr(err, errlen, e.what()); return -1; }
    catch (std::exception& e) { seterr(err, errlen, e.what()); return -1; }
    return nowMs() - a;
}

double nvh_encode_shared(void* hh, void* i, uint32_t slot, uint32_t frame_no, uint64_t ts_ns, int force_idr, char* err, int errlen) {
    auto* n = (Nvh*)hh; auto* ipc = (Ipc*)i;
    if (slot >= (uint32_t)visionalvr_ipc::kSlots) { seterr(err, errlen, "bad slot"); return -1; }
    uint64_t handle = ipc->state->shim.slotHandle[slot];
    if (!handle) { seterr(err, errlen, "no shared handle"); return -1; }
    if (n->sharedHandle[slot] != handle || !n->shared[slot]) {
        if (n->shared[slot]) { n->shared[slot]->Release(); n->shared[slot] = nullptr; }
        HRESULT hr = n->render->GetDevice()->OpenSharedResource((HANDLE)(uintptr_t)handle, __uuidof(ID3D11Texture2D), (void**)&n->shared[slot]);
        if (FAILED(hr)) { char b[96]; snprintf(b, sizeof b, "OpenSharedResource failed 0x%08x", (unsigned)hr); seterr(err, errlen, b); return -1; }
        n->sharedHandle[slot] = handle;
        n->ffr[slot].reset();
    }
    // ALVR's own foveation pass (CompressAxisAligned) when negotiated; otherwise the slot goes to the encoder as is.
    // The pass renders into its own texture on this device, in order with the encoder's input copy.
    if (!n->ffr[slot]) {
        try { n->ffr[slot] = std::make_unique<FFR>(n->render->GetDevice()); n->ffr[slot]->Initialize(n->shared[slot]); }
        catch (Exception& e) { n->ffr[slot].reset(); seterr(err, errlen, e.what()); return -1; }
        catch (std::exception& e) { n->ffr[slot].reset(); seterr(err, errlen, e.what()); return -1; }
    }
    auto ctx = n->render->GetContext();
    if (!n->qDisjoint) {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt{ D3D11_QUERY_TIMESTAMP, 0 }, qe{ D3D11_QUERY_EVENT, 0 };
        n->render->GetDevice()->CreateQuery(&qd, &n->qDisjoint); n->render->GetDevice()->CreateQuery(&qt, &n->qT0); n->render->GetDevice()->CreateQuery(&qt, &n->qT1);
        n->render->GetDevice()->CreateQuery(&qe, &n->qDone);
    }
    double a = nowMs();
    try {
        ctx->Begin(n->qDisjoint); ctx->End(n->qT0);
        n->ffr[slot]->Render();
        ctx->End(n->qT1); ctx->End(n->qDisjoint);
        // (No CPU wait here to measure queueing: waiting for this pass before Transmit makes its input copy queue behind other
        // GPU work a second time; measured +5.7 ms per frame under the harness's GPU keep-alive load.)
        n->enc->Transmit(n->ffr[slot]->GetOutputTexture(), frame_no, ts_ns, force_idr != 0);
        // Transmit returns after the packet is out, so the queries are resolved
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 t0 = 0, t1 = 0;
        if (ctx->GetData(n->qDisjoint, &dj, sizeof(dj), 0) == S_OK && !dj.Disjoint && ctx->GetData(n->qT0, &t0, 8, 0) == S_OK && ctx->GetData(n->qT1, &t1, 8, 0) == S_OK)
            n->lastFfrMs = (float)((double)(t1 - t0) * 1000.0 / (double)dj.Frequency);
    }
    catch (Exception& e) { seterr(err, errlen, e.what()); return -1; }
    catch (std::exception& e) { seterr(err, errlen, e.what()); return -1; }
    return nowMs() - a;
}

void nvh_ipc_set_user(void* i, float user_gamma, int debug_on, const wchar_t* debug_dir) {
    auto& h = ((Ipc*)i)->state->host;
    h.userGamma = user_gamma;
    wcsncpy_s(h.debugDir, debug_dir ? debug_dir : L"", _TRUNCATE);
    MemoryBarrier();
    h.debugOn = debug_on ? 1 : 0;
}

void nvh_ipc_set_color(void* i, float brightness, float contrast, float saturation, float sharpening) {
    auto& h = ((Ipc*)i)->state->host;
    h.colorBrightness = brightness; h.colorContrast = contrast; h.colorSaturation = saturation; h.colorSharpening = sharpening;
}

int nvh_ipc_app_exe(void* i, char* buf, int len) {
    wchar_t w[128];
    memcpy(w, ((Ipc*)i)->state->shim.appExe, sizeof w);
    w[127] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, len, nullptr, nullptr);
}

void nvh_release_shared(void* hh) {
    auto* n = (Nvh*)hh; if (!n) return;
    for (auto& f : n->ffr) f.reset();
    for (int k = 0; k < visionalvr_ipc::kSlots; k++) { if (n->shared[k]) { n->shared[k]->Release(); n->shared[k] = nullptr; } n->sharedHandle[k] = 0; }
}

void nvh_ipc_shim_stats(void* i, uint32_t* submit_mode, uint32_t* ts_matched, uint32_t* ts_fallback) {
    const auto& s = ((Ipc*)i)->state->shim;
    *submit_mode = s.submitMode; *ts_matched = s.tsMatched; *ts_fallback = s.tsFallback;
}

void nvh_ipc_slot_busy(void* i, uint32_t slot, int busy) {
    if (slot >= (uint32_t)visionalvr_ipc::kSlots) return;
    auto* mask = (volatile LONG*)&((Ipc*)i)->state->host.slotBusyMask;
    if (busy) InterlockedOr(mask, (LONG)(1u << slot)); else InterlockedAnd(mask, (LONG)~(1u << slot));
}

void nvh_ipc_set_pacing(void* i, float boundary_offset_ms, float running_start_ms) {
    auto& h = ((Ipc*)i)->state->host;
    h.boundaryOffsetMs = boundary_offset_ms; MemoryBarrier(); h.runningStartMs = running_start_ms;
}

void nvh_ipc_set_hand_offset(void* i, int side, const float* q, const float* p) {
    if (side < 0 || side > 1) return;
    auto& h = ((Ipc*)i)->state->host;
    memcpy(h.handOffsetPos[side], p, 12); MemoryBarrier(); memcpy(h.handOffsetQuat[side], q, 16);
}

void nvh_ipc_shim_pacing(void* i, uint32_t* slot_busy_waits, float* app_frame_ms, uint64_t* releases, uint64_t* releases_late) {
    const auto& s = ((Ipc*)i)->state->shim;
    *slot_busy_waits = s.slotBusyWaits; *app_frame_ms = s.appFrameMs; *releases = s.releases; *releases_late = s.releasesLate;
}

long nvh_set_gpu_scheduling(int cls) {
    typedef long(APIENTRY * PFN)(HANDLE, int);
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (!gdi) return -1;
    PFN fn = (PFN)GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass");
    if (!fn) return -2;
    return fn(GetCurrentProcess(), std::max(0, std::min(5, cls)));
}

void nvh_ipc_close(void* i) { if (i) { visionalvr_ipc::CloseIpc(*(Ipc*)i); delete (Ipc*)i; } }

int nvh_encoder_engines(void* hh) { auto* n = (Nvh*)hh; return n && n->enc ? n->enc->GetEncoderEngineCount() : 0; }


int nvh_describe(void* hh, char* buf, int buflen) {
    auto* n = (Nvh*)hh; if (!n || !buf || buflen < 2) return 0;
    auto& S = Settings::Instance();
    int mn = 0, mx = 0; double mean = 0;
    if (!g_qpMap.empty()) { mn = 127; mx = -128; for (int8_t v : g_qpMap) { mn = std::min<int>(mn, v); mx = std::max<int>(mx, v); mean += v; } mean /= g_qpMap.size(); }
    std::string enc = n->enc ? n->enc->DescribeConfig() : "{}";
    char b[768];
    snprintf(b, sizeof b, "{\"gpu\":\"%s\",\"gpu_thread_priority\":{\"requested\":%d,\"hr\":%ld},\"idle_rgb\":\"%06x\",\"frame\":[%d,%d],\"foveated\":%d,\"qp_map\":{\"enabled\":%d,\"center_delta\":%d,\"edge_delta\":%d,\"transition\":%.2f,\"ctbs\":%zu,\"min\":%d,\"max\":%d,\"mean\":%.2f},\"nvenc\":",
             n->gpuName.c_str(), n->gpuPriority, n->gpuPriorityHr, (unsigned)g_idleRgb, n->w, n->h, S.m_enableFoveatedEncoding ? 1 : 0, S.m_qpMapEnabled ? 1 : 0, S.m_qpMapCenterDelta, S.m_qpMapEdgeDelta, S.m_qpMapTransition, g_qpMap.size(), mn, mx, mean);
    std::string out = std::string(b) + enc + "}";
    if ((int)out.size() >= buflen) return 0;
    memcpy(buf, out.c_str(), out.size() + 1); return (int)out.size();
}

void nvh_destroy(void* hh) {
    auto* n = (Nvh*)hh;
    if (!n) return;
    for (auto& f : n->ffr) f.reset();
    for (auto& t : n->shared) if (t) { t->Release(); t = nullptr; }
    if (n->green) n->green->Release();
    if (n->qDone) n->qDone->Release();
    nvh_keepalive(hh, 0, 0);
    if (n->enc) n->enc->Shutdown();
    if (n->tex) n->tex->Release();
    if (n->base) n->base->Release();
    for (auto* t : n->noiseTex) t->Release();
    delete n;
}

// ---- NVML (nvml.dll ships with the NVIDIA driver): GPU identity for the system check, 1 Hz samples for the debug log ------------
namespace {
typedef int (*FnVoid)();
typedef int (*FnHandle)(unsigned, void**);
typedef int (*FnStr)(void*, char*, unsigned);
typedef int (*FnSysStr)(char*, unsigned);
typedef int (*FnU)(void*, unsigned*);
typedef int (*FnU2)(void*, unsigned*, unsigned*);
typedef int (*FnIU)(void*, int, unsigned*);
typedef int (*FnI)(void*, int*);
typedef int (*FnULL)(void*, unsigned long long*);
struct NvmlUtil { unsigned gpu, memory; };
struct NvmlMem { unsigned long long total, free, used; };
typedef int (*FnUtil)(void*, NvmlUtil*);
typedef int (*FnMem)(void*, NvmlMem*);
struct Nvml {
    bool tried = false, ok = false; void* dev = nullptr;
    FnStr name = nullptr; FnSysStr driver = nullptr; FnU arch = nullptr; FnUtil util = nullptr; FnU2 enc = nullptr, dec = nullptr;
    FnIU clock = nullptr; FnU power = nullptr; FnIU temp = nullptr; FnI pstate = nullptr; FnMem mem = nullptr; FnULL reasons = nullptr;
} g_nvml;
bool nvml() {
    if (g_nvml.tried) return g_nvml.ok;
    g_nvml.tried = true;
    HMODULE h = LoadLibraryA("nvml.dll");
    if (!h) { char p[MAX_PATH]; if (GetEnvironmentVariableA("ProgramW6432", p, MAX_PATH)) { strcat_s(p, "\\NVIDIA Corporation\\NVSMI\\nvml.dll"); h = LoadLibraryA(p); } }
    if (!h) return false;
    auto init = (FnVoid)GetProcAddress(h, "nvmlInit_v2"); auto get = (FnHandle)GetProcAddress(h, "nvmlDeviceGetHandleByIndex_v2");
    if (!init || !get || init() != 0 || get(0, &g_nvml.dev) != 0) return false;
    g_nvml.name = (FnStr)GetProcAddress(h, "nvmlDeviceGetName"); g_nvml.driver = (FnSysStr)GetProcAddress(h, "nvmlSystemGetDriverVersion");
    g_nvml.arch = (FnU)GetProcAddress(h, "nvmlDeviceGetArchitecture"); g_nvml.util = (FnUtil)GetProcAddress(h, "nvmlDeviceGetUtilizationRates");
    g_nvml.enc = (FnU2)GetProcAddress(h, "nvmlDeviceGetEncoderUtilization"); g_nvml.dec = (FnU2)GetProcAddress(h, "nvmlDeviceGetDecoderUtilization");
    g_nvml.clock = (FnIU)GetProcAddress(h, "nvmlDeviceGetClockInfo"); g_nvml.power = (FnU)GetProcAddress(h, "nvmlDeviceGetPowerUsage");
    g_nvml.temp = (FnIU)GetProcAddress(h, "nvmlDeviceGetTemperature"); g_nvml.pstate = (FnI)GetProcAddress(h, "nvmlDeviceGetPerformanceState");
    g_nvml.mem = (FnMem)GetProcAddress(h, "nvmlDeviceGetMemoryInfo");
    g_nvml.reasons = (FnULL)GetProcAddress(h, "nvmlDeviceGetCurrentClocksEventReasons");
    if (!g_nvml.reasons) g_nvml.reasons = (FnULL)GetProcAddress(h, "nvmlDeviceGetCurrentClocksThrottleReasons");
    g_nvml.ok = true;
    return true;
}
const char* arch_name(unsigned a) {
    switch (a) { case 2: return "Kepler"; case 3: return "Maxwell"; case 4: return "Pascal"; case 5: return "Volta"; case 6: return "Turing";
                 case 7: return "Ampere"; case 8: return "Ada"; case 9: return "Hopper"; case 10: return "Blackwell"; default: return "unknown"; }
}
} // namespace

// {"nvml":1,"name":..,"driver":..,"arch":8,"arch_name":"Ada","vram_mb":..} (arch: NVML architecture number, Ada = 8, Blackwell = 10)
int nvh_gpu_info(char* buf, int len) {
    if (!nvml()) { snprintf(buf, len, "{\"nvml\":0}"); return 0; }
    char name[96] = "?", drv[64] = "?"; unsigned arch = 0xFFFFFFFF; NvmlMem m{};
    if (g_nvml.name) g_nvml.name(g_nvml.dev, name, sizeof name);
    if (g_nvml.driver) g_nvml.driver(drv, sizeof drv);
    if (g_nvml.arch) g_nvml.arch(g_nvml.dev, &arch);
    if (g_nvml.mem) g_nvml.mem(g_nvml.dev, &m);
    for (char* c = name; *c; c++) if (*c == '"' || *c == '\\') *c = ' ';
    return snprintf(buf, len, "{\"nvml\":1,\"name\":\"%s\",\"driver\":\"%s\",\"arch\":%d,\"arch_name\":\"%s\",\"vram_mb\":%llu}",
                    name, drv, arch == 0xFFFFFFFF ? -1 : (int)arch, arch_name(arch), m.total >> 20);
}

// One CSV row: gpu%,mem%,enc%,dec%,graphics MHz,memory MHz,video MHz,power W,temp C,pstate,vram used MB,clock event reasons
int nvh_gpu_sample(char* buf, int len) {
    if (!nvml()) return 0;
    NvmlUtil u{}; unsigned enc = 0, dec = 0, sp = 0, cg = 0, cm = 0, cv = 0, pw = 0, t = 0; int ps = -1; NvmlMem m{}; unsigned long long r = 0;
    if (g_nvml.util) g_nvml.util(g_nvml.dev, &u);
    if (g_nvml.enc) g_nvml.enc(g_nvml.dev, &enc, &sp);
    if (g_nvml.dec) g_nvml.dec(g_nvml.dev, &dec, &sp);
    if (g_nvml.clock) { g_nvml.clock(g_nvml.dev, 0, &cg); g_nvml.clock(g_nvml.dev, 2, &cm); g_nvml.clock(g_nvml.dev, 3, &cv); }
    if (g_nvml.power) g_nvml.power(g_nvml.dev, &pw);
    if (g_nvml.temp) g_nvml.temp(g_nvml.dev, 0, &t);
    if (g_nvml.pstate) g_nvml.pstate(g_nvml.dev, &ps);
    if (g_nvml.mem) g_nvml.mem(g_nvml.dev, &m);
    if (g_nvml.reasons) g_nvml.reasons(g_nvml.dev, &r);
    return snprintf(buf, len, "%u,%u,%u,%u,%u,%u,%u,%.1f,%u,P%d,%llu,0x%llx", u.gpu, u.memory, enc, dec, cg, cm, cv, pw / 1000.0, t, ps, m.used >> 20, r);
}

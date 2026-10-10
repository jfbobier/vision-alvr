// Benchmark quality sweep (configure.exe's benchmark, phase 2): renders the benchmark scene (Littlest Tokyo, converted by
// tools/bench_scene to .vab) in stereo, runs it through ALVR's own foveation pass and NVENC encoder exactly as streaming does,
// and measures, per configuration: encode time, produced bitrate, slices per frame (split encode), and the picture the headset
// would show. The decoded picture is NVENC's reconstructed frame (bit-exact with a decoder), un-foveated with the visionOS
// client's own mapping (decompressAxisAlignedCoord, alvr-visionos Shaders.metal) and compared on luma, in display space, with the
// same frame rendered at the reference resolution: a foveation-weighted PSNR (weight 1 at the eye centre, 0.3 at the edges),
// plus centre-only, periphery-only and codec-only (encoded domain) PSNR.
#define NOMINMAX
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <sstream>
#include <stdexcept>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include "bench_scene.h"
// VideoEncoderNVENC.h / NvEncoder.h are build-time copies with two accessors added (GetNvEncoder, GetSessionHandle/GetApi; see
// tools/host/build.rs): the reconstructed-frame surface has to be registered on NVENC's own session.
#include "VideoEncoderNVENC.h"
#include "FFR.h"
#include "alvr_server/Settings.h"
#include "ALVR-common/exception.h"
#include "shim.h"
#include "nvh.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern "C" { void* g_nvh_recon_ptr = nullptr; int g_nvh_recon_enable = 0; }
void nvh_build_qp_map(int W, int H);   // nvh.cpp

namespace {
double nowMs() { using namespace std::chrono; return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count(); }

// (scene loader and renderer: bench_scene.h, shared with the OpenXR probe app)

// ---------------------------------------------------------------- display-space metric
const char* kMetricHlsl = R"(
cbuffer M : register(b0) {
    float2 refSize; float2 eyeSizeRatio; float2 centerSize; float2 centerShift; float2 edgeRatio; float2 eyeCenterL;
    float2 eyeCenterR; float foveated; float yScale; float3 yCoef; float sigma; float mode; uint groupsX; uint stride;
    float uvScaleY; float yA; float yB; float refScale; float pad;
};
Texture2D<float4> refTex : register(t0); Texture2D<float> reconY : register(t1); SamplerState lin : register(s0);
RWStructuredBuffer<float4> partial : register(u0);
groupshared float4 gA[256]; groupshared float4 gB[256]; groupshared float4 gC[256];
float2 TextureToEyeUV(float2 t, bool r) { return float2((t.x + (r ? 1 : 0) * (1. - 2. * t.x)) * 2., t.y); }
float2 EyeToTextureUV(float2 e, bool r) { return float2(e.x * 0.5 + (r ? 1 : 0) * (1. - e.x), e.y); }
// the visionOS client's decompressAxisAlignedCoord (alvr-visionos Shaders.metal), verbatim
float2 decompress(float2 uv) {
    bool isRightEye = uv.x > 0.5;
    float2 eyeUV = TextureToEyeUV(uv, isRightEye);
    const float2 c0 = (1. - centerSize) * 0.5;
    const float2 c1 = (edgeRatio - 1.) * c0 * (centerShift + 1.) / edgeRatio;
    const float2 c2 = (edgeRatio - 1.) * centerSize + 1.;
    const float2 loBound = c0 * (centerShift + 1.);
    const float2 hiBound = c0 * (centerShift - 1.) + 1.;
    float2 underBound = float2(eyeUV.x < loBound.x, eyeUV.y < loBound.y);
    float2 inBound = float2(loBound.x < eyeUV.x && eyeUV.x < hiBound.x, loBound.y < eyeUV.y && eyeUV.y < hiBound.y);
    float2 overBound = float2(eyeUV.x > hiBound.x, eyeUV.y > hiBound.y);
    float2 center = (eyeUV - c1) * edgeRatio / c2;
    const float2 loBoundC = c0 * (centerShift + 1.) / c2;
    const float2 hiBoundC = c0 * (centerShift - 1.) / c2 + 1.;
    float2 leftEdge = (-(c1 + c2 * loBoundC) / loBoundC + sqrt(((c1 + c2 * loBoundC) / loBoundC) * ((c1 + c2 * loBoundC) / loBoundC) +
                      4. * c2 * (1. - edgeRatio) / (edgeRatio * loBoundC) * eyeUV)) / (2. * c2 * (1. - edgeRatio)) * (edgeRatio * loBoundC);
    float2 k = (c2 - edgeRatio * c1 - 2. * edgeRatio * c2 + c2 * edgeRatio * (1. - hiBoundC) + edgeRatio) / (edgeRatio * (1. - hiBoundC));
    float2 rightEdge = (-k + sqrt(k * k - 4. * ((c2 * edgeRatio - c2) * (c1 - hiBoundC + hiBoundC * c2) / (edgeRatio * (1. - hiBoundC) * (1. - hiBoundC)) -
                       eyeUV * (c2 * edgeRatio - c2) / (edgeRatio * (1. - hiBoundC))))) / (2. * c2 * (edgeRatio - 1.)) * (edgeRatio * (1. - hiBoundC));
    float2 u = clamp(underBound * leftEdge, 0, 1) + clamp(inBound * center, 0, 1) + clamp(overBound * rightEdge, 0, 1);
    return EyeToTextureUV(u * eyeSizeRatio, isRightEye);
}
// mode 0: display space (reference grid, un-foveated recon), weighted / centre / periphery error sums.
// mode 1: calibration in the encoded domain: least-squares sums of recon Y against both luma matrices (finds NVENC's matrix and
//         range: y = a * Y(rgb) + b).
[numthreads(16, 16, 1)]
void cs(uint3 gid : SV_GroupID, uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    float4 a = 0, b = 0, c = 0;
    uint2 p = tid.xy * stride;
    if (p.x < (uint)refSize.x && p.y < (uint)refSize.y) {
        float2 uv = (p + 0.5) / refSize;
        // the reference: refScale x refScale texels per grid pixel, box filtered (the display grid from a supersampled render)
        float3 rgb = 0;
        uint k = (uint)refScale;
        for (uint j = 0; j < k; j++) for (uint i = 0; i < k; i++) rgb += refTex.Load(int3(p * k + uint2(i, j), 0)).rgb;
        rgb /= (float)(k * k);
        float2 suv = foveated > 0.5 ? decompress(uv) : uv;
        float yr = reconY.SampleLevel(lin, suv * float2(1, uvScaleY), 0) * yScale;
        if (mode > 0.5) {
            float x7 = dot(rgb, float3(0.2126, 0.7152, 0.0722)), x6 = dot(rgb, float3(0.299, 0.587, 0.114));
            a = float4(x7, yr, x7 * x7, x7 * yr);
            b = float4(x6, x6 * x6, x6 * yr, yr * yr);
            c = float4(1, 0, 0, 0);
        } else {
            float e = dot(rgb, yCoef) - (yr - yB) / yA, se = e * e;
            bool right = uv.x > 0.5;
            float2 eu = float2(right ? (uv.x - 0.5) * 2 : uv.x * 2, uv.y);
            float2 d = (eu - (right ? eyeCenterR : eyeCenterL)) * float2(refSize.x * 0.5 / refSize.y, 1); // isotropic
            float r2 = dot(d, d);
            float w = 0.3 + 0.7 * exp(-r2 / (2 * sigma * sigma));
            bool cn = r2 < 0.15 * 0.15;
            a = float4(w * se, w, cn ? se : 0, cn ? 1 : 0);
            b = float4(cn ? 0 : se, cn ? 0 : 1, 0, 0);
        }
    }
    gA[gi] = a; gB[gi] = b; gC[gi] = c;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1) { if (gi < s) { gA[gi] += gA[gi + s]; gB[gi] += gB[gi + s]; gC[gi] += gC[gi + s]; } GroupMemoryBarrierWithGroupSync(); }
    if (gi == 0) { uint idx = gid.y * groupsX + gid.x; partial[idx * 3] = gA[0]; partial[idx * 3 + 1] = gB[0]; partial[idx * 3 + 2] = gC[0]; }
}
)";

struct MetricCB {
    float refSize[2], eyeSizeRatio[2], centerSize[2], centerShift[2], edgeRatio[2], eyeCenterL[2], eyeCenterR[2];
    float foveated, yScale; float yCoef[3]; float sigma, mode; uint32_t groupsX, stride; float uvScaleY, yA, yB, refScale, pad;
};
static_assert(sizeof(MetricCB) % 16 == 0, "cbuffer size");

struct Target { ComPtr<ID3D11Texture2D> msaa, resolved, depth; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11DepthStencilView> dsv; ComPtr<ID3D11ShaderResourceView> srv; int w = 0, h = 0; };

// What the game hands the shim (and the shim the encoder): 10-bit R10G10B10A2 for the 10-bit encoder, else RGBA8.
DXGI_FORMAT FrameFormat() { return Settings::Instance().m_use10bitEncoder ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM; }

bool MakeTarget(ID3D11Device* dev, int w, int h, Target& t, std::string& err) {
    t = {}; t.w = w; t.h = h;
    D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = FrameFormat();
    td.SampleDesc.Count = 4; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &t.msaa))) { err = "MSAA target"; return false; }
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &t.depth))) { err = "depth target"; return false; }
    td.SampleDesc.Count = 1; td.Format = FrameFormat(); td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &t.resolved))) { err = "resolve target"; return false; }
    dev->CreateRenderTargetView(t.msaa.Get(), nullptr, &t.rtv);
    dev->CreateDepthStencilView(t.depth.Get(), nullptr, &t.dsv);
    dev->CreateShaderResourceView(t.resolved.Get(), nullptr, &t.srv);
    return true;
}

// ALVR's FFR.cpp CalculateFoveationVars (float/double mix kept), for the client mapping's constants
struct FovVars { float centerSize[2], centerShift[2], edgeRatio[2], eyeRatio[2]; uint32_t optW, optH; };
FovVars CalcFov(float eyeW, float eyeH, float csx, float csy, float shx, float shy, float erx, float ery) {
    FovVars v{};
    float edgeSizeX = eyeW - csx * eyeW, edgeSizeY = eyeH - csy * eyeH;
    float centerSizeXAligned = 1. - ceil(edgeSizeX / (erx * 2.)) * (erx * 2.) / eyeW;
    float centerSizeYAligned = 1. - ceil(edgeSizeY / (ery * 2.)) * (ery * 2.) / eyeH;
    float edgeSizeXAligned = eyeW - centerSizeXAligned * eyeW, edgeSizeYAligned = eyeH - centerSizeYAligned * eyeH;
    float centerShiftXAligned = ceil(shx * edgeSizeXAligned / (erx * 2.)) * (erx * 2.) / edgeSizeXAligned;
    float centerShiftYAligned = ceil(shy * edgeSizeYAligned / (ery * 2.)) * (ery * 2.) / edgeSizeYAligned;
    float fsx = (centerSizeXAligned + (1. - centerSizeXAligned) / erx), fsy = (centerSizeYAligned + (1. - centerSizeYAligned) / ery);
    float optW = fsx * eyeW, optH = fsy * eyeH;
    v.optW = (uint32_t)ceil(optW / 32.f) * 32; v.optH = (uint32_t)ceil(optH / 32.f) * 32;
    v.centerSize[0] = centerSizeXAligned; v.centerSize[1] = centerSizeYAligned;
    v.centerShift[0] = centerShiftXAligned; v.centerShift[1] = centerShiftYAligned;
    v.edgeRatio[0] = erx; v.edgeRatio[1] = ery;
    v.eyeRatio[0] = optW / v.optW; v.eyeRatio[1] = optH / v.optH;
    return v;
}


struct Bench {
    std::shared_ptr<CD3DRender> render;
    Scene scene;
    ComPtr<ID3D11ComputeShader> metric;
    ComPtr<ID3D11Buffer> metricCb;
    ComPtr<ID3D11SamplerState> lin;
    ComPtr<ID3D11Query> done;
    std::map<std::pair<int, int>, Target> targets;   // render targets by size, kept across configurations (the reference is ~1 GB)
    std::string gpu;
    Target* target(int w, int h, std::string& err) {
        auto it = targets.find({ w, h });
        if (it != targets.end() && it->second.msaa) return &it->second;
        Target t;
        if (!MakeTarget(render->GetDevice(), w, h, t, err)) return nullptr;
        return &(targets[{ w, h }] = t);
    }
    void trim(std::pair<int, int> keepA, std::pair<int, int> keepB) {   // VRAM: only the current candidate and the reference stay
        for (auto it = targets.begin(); it != targets.end();) it = (it->first == keepA || it->first == keepB) ? std::next(it) : targets.erase(it);
    }
    void finish() {   // CPU wait for the GPU queue (so a timing excludes the scene render queued before it)
        auto ctx = render->GetContext();
        ctx->End(done.Get());
        while (ctx->GetData(done.Get(), nullptr, 0, 0) == S_FALSE) std::this_thread::yield();
    }
};

struct PacketTally { uint64_t bytes = 0; uint64_t packets = 0; uint64_t slices = 0; };
PacketTally g_tally;
uint64_t g_bitrate = 0; float g_fps = 90;
bool g_paramsSent = false;

// HEVC VCL NAL units (= slices) in an Annex B packet
int CountVcl(const unsigned char* b, int n) {
    int c = 0;
    for (int i = 0; i + 3 < n;) {
        if (b[i + 2] > 1) { i += 3; continue; }
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) { if (((b[i + 3] >> 1) & 0x3f) < 32) c++; i += 3; } else i++;
    }
    return c;
}

double Psnr(double mse) { return mse <= 1e-12 ? 99.0 : 10.0 * log10(1.0 / mse); }

// Reconstructed-frame surface candidates, in order: planar YUV textures, then a single-plane texture of 1.5x height (luma on top),
// in case the driver wants the latter for D3D11 (its registration reports a chroma offset).
struct ReconFmt { DXGI_FORMAT tex, srv; NV_ENC_BUFFER_FORMAT nv; bool tall; const char* name; };
const ReconFmt kReconFmts10[] = {
    { DXGI_FORMAT_P010, DXGI_FORMAT_R16_UNORM, NV_ENC_BUFFER_FORMAT_YUV420_10BIT, false, "P010" },
    { DXGI_FORMAT_NV12, DXGI_FORMAT_R8_UNORM, NV_ENC_BUFFER_FORMAT_NV12, false, "NV12" },
    { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, NV_ENC_BUFFER_FORMAT_YUV420_10BIT, true, "R16x1.5" },
    { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, NV_ENC_BUFFER_FORMAT_NV12, true, "R8x1.5" },
};
const ReconFmt kReconFmts8[] = {
    { DXGI_FORMAT_NV12, DXGI_FORMAT_R8_UNORM, NV_ENC_BUFFER_FORMAT_NV12, false, "NV12" },
    { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, NV_ENC_BUFFER_FORMAT_NV12, true, "R8x1.5" },
};

struct Recon {
    NvEncoder* nv = nullptr;
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> y;
    NV_ENC_REGISTERED_PTR reg = nullptr; NV_ENC_INPUT_PTR mapped = nullptr;
    float uvScaleY = 1;
    std::string fmt, err;
    bool open(ID3D11Device* dev, NvEncoder* enc, int w, int h, bool ten) {
        nv = enc;
        const auto& api = nv->GetApi();
        void* s = nv->GetSessionHandle();
        const ReconFmt* list = ten ? kReconFmts10 : kReconFmts8;
        const int n = ten ? 4 : 2;
        for (int k = 0; k < n && !mapped; k++) {
            const ReconFmt& f = list[k];
            tex.Reset(); y.Reset();
            D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = f.tall ? h * 3 / 2 : h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
            td.Format = f.tex; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) { td.BindFlags = D3D11_BIND_SHADER_RESOURCE; if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) { err += std::string(f.name) + ": texture; "; continue; } }
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = f.srv; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            if (FAILED(dev->CreateShaderResourceView(tex.Get(), &sd, &y))) { err += std::string(f.name) + ": view; "; continue; }
            NV_ENC_REGISTER_RESOURCE rr{ NV_ENC_REGISTER_RESOURCE_VER };
            rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX; rr.resourceToRegister = tex.Get(); rr.width = w; rr.height = h;
            rr.bufferFormat = f.nv; rr.bufferUsage = NV_ENC_OUTPUT_RECON;
            NVENCSTATUS st = api.nvEncRegisterResource(s, &rr);
            if (st != NV_ENC_SUCCESS) { const char* le = api.nvEncGetLastErrorString(s); err += std::string(f.name) + ": register " + std::to_string((int)st) + " " + (le ? le : "") + "; "; continue; }
            NV_ENC_MAP_INPUT_RESOURCE mr{ NV_ENC_MAP_INPUT_RESOURCE_VER }; mr.registeredResource = rr.registeredResource;
            st = api.nvEncMapInputResource(s, &mr);
            if (st != NV_ENC_SUCCESS) { api.nvEncUnregisterResource(s, rr.registeredResource); err += std::string(f.name) + ": map " + std::to_string((int)st) + "; "; continue; }
            reg = rr.registeredResource; mapped = mr.mappedResource; fmt = f.name; uvScaleY = f.tall ? 2.f / 3.f : 1.f;
        }
        return mapped != nullptr;
    }
    void close() {
        if (!nv) return;
        const auto& api = nv->GetApi();
        void* s = nv->GetSessionHandle();
        if (mapped) api.nvEncUnmapInputResource(s, mapped);
        if (reg) api.nvEncUnregisterResource(s, reg);
        mapped = nullptr; reg = nullptr; nv = nullptr;
    }
};
} // namespace

// ---------------------------------------------------------------- C ABI

void* nvh_bench_open(const char* scene, char* err, int errlen) {
    auto* b = new Bench();
    auto fail = [&](const std::string& e) { if (err && errlen > 0) { strncpy_s(err, errlen, e.c_str(), _TRUNCATE); } delete b; return (void*)nullptr; };
    IDXGIFactory1* fac = nullptr; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
    int adapter = -1; IDXGIAdapter1* ad;
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d); ad->Release();
        if (d.VendorId == 0x10DE && adapter < 0) { adapter = (int)i; char nm[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, sizeof nm, nullptr, nullptr); b->gpu = nm; }
    }
    if (fac) fac->Release();
    if (adapter < 0) return fail("no NVIDIA adapter");
    b->render = std::make_shared<CD3DRender>();
    if (!b->render->Initialize((uint32_t)adapter)) return fail("D3D11 init failed");
    std::string e;
    if (!LoadScene(b->render->GetDevice(), b->render->GetContext(), scene, b->scene, e)) return fail(e);
    ComPtr<ID3DBlob> csb;
    if (!compile(kMetricHlsl, "cs", "cs_5_0", csb, e)) return fail(e);
    auto dev = b->render->GetDevice();
    dev->CreateComputeShader(csb->GetBufferPointer(), csb->GetBufferSize(), nullptr, &b->metric);
    D3D11_BUFFER_DESC cbd{ (UINT)sizeof(MetricCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    dev->CreateBuffer(&cbd, nullptr, &b->metricCb);
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &b->lin);
    D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
    dev->CreateQuery(&qd, &b->done);
    return b;
}

void nvh_bench_close(void* h) { delete (Bench*)h; }

int nvh_bench_gpu(void* h, char* buf, int len) {
    auto* b = (Bench*)h;
    if (!buf || len <= 0) return 0;
    strncpy_s(buf, len, b->gpu.c_str(), _TRUNCATE);
    return (int)strlen(buf);
}

// Renders the scene (2*eye_w x eye_h SBS) at time t into an RGBA8 buffer (preview / checks). Returns the bytes written.
int nvh_bench_preview(void* h, int eye_w, int eye_h, double t, const float* fov8, float ipd, uint8_t* rgba, int len) {
    auto* b = (Bench*)h;
    std::string e;
    if (len < eye_w * 2 * eye_h * 4) return 0;
    Target* tg = b->target(eye_w * 2, eye_h, e);
    if (!tg) return 0;
    auto ctx = b->render->GetContext();
    RenderScene(b->scene, ctx, tg->rtv.Get(), tg->dsv.Get(), tg->w, tg->h, t, fov8, ipd);
    ctx->ResolveSubresource(tg->resolved.Get(), 0, tg->msaa.Get(), 0, FrameFormat());
    D3D11_TEXTURE2D_DESC td; tg->resolved->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(b->render->GetDevice()->CreateTexture2D(&td, nullptr, &st))) return 0;
    ctx->CopyResource(st.Get(), tg->resolved.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return 0;
    const bool ten = td.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
    for (int y = 0; y < tg->h; y++) {
        const uint32_t* row = (const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
        for (int x = 0; x < tg->w; x++) {
            const uint32_t v = row[x];
            uint8_t* o = rgba + ((size_t)y * tg->w + x) * 4;
            if (ten) { o[0] = (uint8_t)(((v >> 0) & 1023) >> 2); o[1] = (uint8_t)(((v >> 10) & 1023) >> 2); o[2] = (uint8_t)(((v >> 20) & 1023) >> 2); }
            else { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)(v >> 16); }
            o[3] = 255;
        }
    }
    ctx->Unmap(st.Get(), 0);
    return tg->w * tg->h * 4;
}

// One configuration of the quality sweep: the scene at eye_w x eye_h through ALVR's foveation pass and NVENC (the Settings singleton
// is set from the config; everything else stays as the host configured it), the reconstructed frame compared, in display space,
// with the scene rendered at ref_w x ref_h.
int nvh_bench_run(void* h, const NvhBenchConfig* c, NvhBenchResult* r) {
    auto* b = (Bench*)h;
    *r = {};
    auto dev = b->render->GetDevice();
    auto ctx = b->render->GetContext();
    auto& S = Settings::Instance();
    std::string e;
    std::unique_ptr<VideoEncoderNVENC> enc;
    Recon rc;
    auto cleanup = [&]() {
        rc.close();
        g_nvh_recon_ptr = nullptr; g_nvh_recon_enable = 0;
        if (enc) { try { enc->Shutdown(); } catch (...) {} enc.reset(); }
        g_shim.packet_cb = nullptr; g_shim.params_cb = nullptr;
    };
    try {
        S.m_renderWidth = (uint32_t)c->eye_w * 2; S.m_renderHeight = (uint32_t)c->eye_h;
        S.m_enableFoveatedEncoding = c->foveated != 0;
        S.m_foveationCenterSizeX = c->fov_cs[0]; S.m_foveationCenterSizeY = c->fov_cs[1];
        S.m_foveationCenterShiftX = c->fov_sh[0]; S.m_foveationCenterShiftY = c->fov_sh[1];
        S.m_foveationEdgeRatioX = c->fov_er[0]; S.m_foveationEdgeRatioY = c->fov_er[1];
        S.m_nvencQualityPreset = (uint32_t)c->preset;
        S.m_nvencAdaptiveQuantizationMode = (uint32_t)c->aq;
        S.m_nvencSplitEncodeMode = (uint32_t)c->split;
        S.m_qpMapEnabled = c->qp_map != 0;
        S.m_refreshRate = (int)lround(c->fps);
        const bool ten = S.m_use10bitEncoder;
        const FovVars fv = CalcFov((float)c->eye_w, (float)c->eye_h, c->fov_cs[0], c->fov_cs[1], c->fov_sh[0], c->fov_sh[1], c->fov_er[0], c->fov_er[1]);
        const int encW = c->foveated ? (int)fv.optW * 2 : c->eye_w * 2, encH = c->foveated ? (int)fv.optH : c->eye_h;
        r->enc_w = encW; r->enc_h = encH;
        Target* cand = b->target(c->eye_w * 2, c->eye_h, e);
        if (!cand) throw std::runtime_error("candidate target: " + e);
        // the reference is an integer multiple of the display grid (box filtered by the metric)
        const int refScale = c->disp_w > 0 ? std::max(1, c->ref_w / c->disp_w) : 1;
        if (c->measure_quality && (c->disp_w <= 0 || c->ref_w != c->disp_w * refScale || c->ref_h != c->disp_h * refScale))
            throw std::runtime_error("reference size must be an integer multiple of the display size");
        b->trim({ c->eye_w * 2, c->eye_h }, { c->ref_w * 2, c->ref_h });
        Target* ref = c->measure_quality ? b->target(c->ref_w * 2, c->ref_h, e) : nullptr;
        if (c->measure_quality && !ref) throw std::runtime_error("reference target: " + e);
        FFR ffr(dev);
        ffr.Initialize(cand->resolved.Get());
        nvh_build_qp_map(encW, encH);
        g_tally = {}; g_bitrate = (uint64_t)(c->bitrate_mbps * 1e6); g_fps = c->fps; g_paramsSent = false;
        g_shim.user = nullptr;
        g_shim.packet_cb = [](void*, const unsigned char* d, int l, unsigned long long, bool) { g_tally.bytes += l; g_tally.packets++; g_tally.slices += CountVcl(d, l); };
        g_shim.params_cb = [](void*, unsigned long long* br, float* fr) -> bool { if (g_paramsSent) return false; g_paramsSent = true; *br = g_bitrate; *fr = g_fps; return true; };
        // the encoder, with reconstructed-frame output when the quality is measured (retried without it if NVENC refuses the
        // combination, e.g. with split encode: timing and size are still worth having)
        g_nvh_recon_enable = c->measure_quality ? 1 : 0;
        for (int attempt = 0; attempt < 2; attempt++) {
            try {
                enc = std::make_unique<VideoEncoderNVENC>(b->render, encW, encH);
                enc->Initialize();
                break;
            } catch (Exception& x) {
                enc.reset();
                if (!g_nvh_recon_enable) throw;
                rc.err = std::string("encoder refused recon output: ") + x.what();
                g_nvh_recon_enable = 0;
            }
        }
        if (g_nvh_recon_enable && enc->GetNvEncoder()) {
            if (rc.open(dev, enc->GetNvEncoder(), encW, encH, ten)) g_nvh_recon_ptr = rc.mapped;
        }
        r->recon = g_nvh_recon_ptr ? 1 : 0;
        r->nvenc_engines = enc->GetEncoderEngineCount();
        strncpy_s(r->recon_format, rc.fmt.c_str(), _TRUNCATE);
        // metric buffers
        const uint32_t stride = (uint32_t)std::max(1, c->metric_stride);
        const uint32_t gx = ((uint32_t)c->disp_w * 2 / stride + 15) / 16, gy = ((uint32_t)c->disp_h / stride + 15) / 16;
        const uint32_t cgx = ((uint32_t)encW + 15) / 16, cgy = ((uint32_t)encH + 15) / 16;
        const uint32_t maxGroups = std::max(gx * gy, cgx * cgy);
        ComPtr<ID3D11Buffer> part, partRead; ComPtr<ID3D11UnorderedAccessView> partUav;
        D3D11_BUFFER_DESC pd{ maxGroups * 3 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
        dev->CreateBuffer(&pd, nullptr, &part);
        pd.Usage = D3D11_USAGE_STAGING; pd.BindFlags = 0; pd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; pd.MiscFlags = 0; pd.StructureByteStride = 0;
        dev->CreateBuffer(&pd, nullptr, &partRead);
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = maxGroups * 3;
        dev->CreateUnorderedAccessView(part.Get(), &ud, &partUav);
        if (!part || !partRead || !partUav) throw std::runtime_error("metric buffers");
        ComPtr<ID3D11ShaderResourceView> encInSrv;
        dev->CreateShaderResourceView(ffr.GetOutputTexture(), nullptr, &encInSrv);
        float yA = 1, yB = 0; bool y601 = false;
        // mode 0: display space (sums[0..5]); mode 1: calibration regression (sums[0..8])
        auto runMetric = [&](ID3D11ShaderResourceView* refSrv, int rw, int rh, int mode, bool fov, uint32_t st, uint32_t refScale, double sums[12]) {
            const uint32_t ngx = ((uint32_t)rw / st + 15) / 16, ngy = ((uint32_t)rh / st + 15) / 16;
            MetricCB m{};
            m.refSize[0] = (float)rw; m.refSize[1] = (float)rh;
            m.eyeSizeRatio[0] = fv.eyeRatio[0]; m.eyeSizeRatio[1] = fv.eyeRatio[1];
            m.centerSize[0] = fv.centerSize[0]; m.centerSize[1] = fv.centerSize[1];
            m.centerShift[0] = fv.centerShift[0]; m.centerShift[1] = fv.centerShift[1];
            m.edgeRatio[0] = fv.edgeRatio[0]; m.edgeRatio[1] = fv.edgeRatio[1];
            const float* f = c->fov;
            m.eyeCenterL[0] = f[0] / (f[0] + f[1]); m.eyeCenterL[1] = f[2] / (f[2] + f[3]);
            m.eyeCenterR[0] = f[4] / (f[4] + f[5]); m.eyeCenterR[1] = f[6] / (f[6] + f[7]);
            m.foveated = fov ? 1.f : 0.f;
            m.yScale = 1.f;
            m.yCoef[0] = y601 ? 0.299f : 0.2126f; m.yCoef[1] = y601 ? 0.587f : 0.7152f; m.yCoef[2] = y601 ? 0.114f : 0.0722f;
            m.sigma = 0.25f; m.mode = (float)mode; m.groupsX = ngx; m.stride = st;
            m.uvScaleY = rc.uvScaleY; m.yA = yA; m.yB = yB; m.refScale = (float)refScale;
            D3D11_MAPPED_SUBRESOURCE ms;
            ctx->Map(b->metricCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms); memcpy(ms.pData, &m, sizeof m); ctx->Unmap(b->metricCb.Get(), 0);
            // the foveation pass leaves its output bound as a render target: D3D11 would null an SRV of it (reads as zeros)
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ID3D11ShaderResourceView* srv[2] = { refSrv, rc.y.Get() };
            ctx->CSSetShader(b->metric.Get(), nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, b->metricCb.GetAddressOf());
            ctx->CSSetShaderResources(0, 2, srv);
            ctx->CSSetSamplers(0, 1, b->lin.GetAddressOf());
            ctx->CSSetUnorderedAccessViews(0, 1, partUav.GetAddressOf(), nullptr);
            ctx->Dispatch(ngx, ngy, 1);
            ID3D11ShaderResourceView* nsrv[2] = {}; ID3D11UnorderedAccessView* nuav = nullptr;
            ctx->CSSetShaderResources(0, 2, nsrv); ctx->CSSetUnorderedAccessViews(0, 1, &nuav, nullptr);
            ctx->CopyResource(partRead.Get(), part.Get());
            if (SUCCEEDED(ctx->Map(partRead.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
                const float* p = (const float*)ms.pData;
                for (uint32_t g = 0; g < ngx * ngy; g++) for (int k = 0; k < 12; k++) sums[k] += p[(size_t)g * 12 + k];
                ctx->Unmap(partRead.Get(), 0);
            }
        };
        std::vector<double> ms;
        double q[12] = {}, codec[12] = {};
        int metricFrames = 0;
        PacketTally atWarm{};
        const int total = c->warmup + c->frames;
        for (int i = 0; i < total; i++) {
            const double t = c->t0 + i / (double)c->fps;
            if (i == c->warmup) atWarm = g_tally;
            RenderScene(b->scene, ctx, cand->rtv.Get(), cand->dsv.Get(), cand->w, cand->h, t, c->fov, c->ipd);
            ctx->ResolveSubresource(cand->resolved.Get(), 0, cand->msaa.Get(), 0, FrameFormat());
            b->finish();
            const double a = nowMs();
            ffr.Render();
            enc->Transmit(ffr.GetOutputTexture(), (uint64_t)i, (uint64_t)i * 11111111ull, i == 0);
            const double dt = nowMs() - a;
            if (i < c->warmup) continue;
            ms.push_back(dt);
            if (r->recon && (i - c->warmup) % std::max(1, c->metric_every) == 0) {
                if (metricFrames == 0) {
                    // which luma matrix and range NVENC's RGB->YUV conversion uses: the fit of its own reconstruction against the input
                    double cal[12] = {};
                    runMetric(encInSrv.Get(), encW, encH, 1, false, 1, 1, cal);
                    struct Fit { double a, b, sse; };
                    auto fit = [&](double sx, double sxx, double sxy) {
                        const double n = cal[8], sy = cal[1], syy = cal[7];
                        const double den = n * sxx - sx * sx;
                        Fit f{ 0, 0, 1e30 };
                        if (n <= 0 || fabs(den) < 1e-12) return f;
                        f.a = (n * sxy - sx * sy) / den; f.b = (sy - f.a * sx) / n;
                        f.sse = syy - 2 * f.a * sxy - 2 * f.b * sy + f.a * f.a * sxx + 2 * f.a * f.b * sx + n * f.b * f.b;
                        return f;
                    };
                    const Fit f7 = fit(cal[0], cal[2], cal[3]), f6 = fit(cal[4], cal[5], cal[6]);
                    y601 = f6.sse < f7.sse;
                    const Fit& best = y601 ? f6 : f7;
                    yA = (float)best.a; yB = (float)best.b;
                    r->luma601 = y601 ? 1 : 0; r->y_gain = yA; r->y_offset = yB;
                    if (!(yA > 0.2f && yA < 5.f)) {   // an all-black / garbage reconstruction: the surface is not written
                        r->recon = 0;
                        rc.err += "reconstruction not written (fit gain " + std::to_string(yA) + ", n " + std::to_string(cal[8]) + ", mean Y(rgb) " +
                            std::to_string(cal[8] > 0 ? cal[0] / cal[8] : 0) + ", mean recon " + std::to_string(cal[8] > 0 ? cal[1] / cal[8] : 0) + "); ";
                        continue;
                    }
                }
                runMetric(encInSrv.Get(), encW, encH, 0, false, 1, 1, codec);
                if (ref) {
                    RenderScene(b->scene, ctx, ref->rtv.Get(), ref->dsv.Get(), ref->w, ref->h, t, c->fov, c->ipd);
                    ctx->ResolveSubresource(ref->resolved.Get(), 0, ref->msaa.Get(), 0, FrameFormat());
                    runMetric(ref->srv.Get(), c->disp_w * 2, c->disp_h, 0, c->foveated != 0, stride, (uint32_t)refScale, q);
                }
                metricFrames++;
            }
        }
        std::sort(ms.begin(), ms.end());
        auto qt = [&](double p) { return ms.empty() ? 0.0 : ms[std::min(ms.size() - 1, (size_t)(ms.size() * p))]; };
        double sum = 0; for (double v : ms) sum += v;
        r->encode_avg_ms = ms.empty() ? 0 : sum / ms.size();
        r->encode_p50_ms = qt(0.5); r->encode_p95_ms = qt(0.95); r->encode_max_ms = ms.empty() ? 0 : ms.back();
        const uint64_t bytes = g_tally.bytes - atWarm.bytes, pk = g_tally.packets - atWarm.packets, sl = g_tally.slices - atWarm.slices;
        r->produced_mbps = c->frames > 0 ? bytes * 8.0 / c->frames * c->fps / 1e6 : 0;
        r->slices_per_frame = pk ? (double)sl / pk : 0;
        r->metric_frames = metricFrames;
        if (metricFrames > 0) {
            // errors are in luma units of the reference (Y in [0, 1])
            r->codec_psnr = (codec[3] + codec[5]) > 0 ? Psnr((codec[2] + codec[4]) / (codec[3] + codec[5])) : 0;
            if (ref) {
                r->fw_psnr = q[1] > 0 ? Psnr(q[0] / q[1]) : 0;
                r->center_psnr = q[3] > 0 ? Psnr(q[2] / q[3]) : 0;
                r->periphery_psnr = q[5] > 0 ? Psnr(q[4] / q[5]) : 0;
            }
        }
        strncpy_s(r->note, rc.err.c_str(), _TRUNCATE);
        cleanup();
        return 1;
    } catch (Exception& x) { e = x.what(); } catch (std::exception& x) { e = x.what(); }
    cleanup();
    strncpy_s(r->error, e.c_str(), _TRUNCATE);
    return 0;
}

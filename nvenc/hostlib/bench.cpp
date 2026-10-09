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

// ---------------------------------------------------------------- scene (.vab, see tools/bench_scene/convert.cpp)
struct Material { float base[4]; int baseTex, baseUv, aoTex, aoUv; float aoStrength; float emissive[3]; int emTex, emUv; uint32_t alphaMode; float cutoff; uint32_t doubleSided; };
struct Prim { int material; ComPtr<ID3D11Buffer> vb[4], ib; uint32_t count; };
struct Node { int parent, mesh; float t[3], r[4], s[3]; uint32_t hasMatrix; float m[16]; };
struct Channel { int node; uint32_t path, interp, keys; std::vector<float> times, values; };

struct Reader {
    const uint8_t* p; const uint8_t* end;
    bool ok = true;
    template <typename T> T get() { T v{}; take(&v, sizeof v); return v; }
    void take(void* dst, size_t n) { if ((size_t)(end - p) < n) { ok = false; return; } memcpy(dst, p, n); p += (n + 3) & ~size_t(3); }
    const uint8_t* skip(size_t n) { const uint8_t* q = p; if ((size_t)(end - p) < n) { ok = false; return q; } p += (n + 3) & ~size_t(3); return q; }
};

const char* kSceneHlsl = R"(
cbuffer C : register(b0) { float4x4 world; float4x4 viewProj; float4 base; float4 emissive; float4 p; float4 q; float4 dbg; };
Texture2D tBase : register(t0); Texture2D tAo : register(t1); Texture2D tEm : register(t2); SamplerState s0 : register(s0);
struct VI { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };
struct VO { float4 pos : SV_Position; float3 n : NORMAL; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };
VO vs(VI i) { VO o; float4 w = mul(float4(i.pos, 1), world); o.pos = mul(w, viewProj); o.n = mul(float4(i.nrm, 0), world).xyz; o.uv0 = i.uv0; o.uv1 = i.uv1; return o; }
float3 enc(float3 c) { c = saturate(c); return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308); }
float4 ps(VO i) : SV_Target {
    float4 b = base;
    if (dbg.x == 1) return float4(frac(i.uv0), 0, 1);
    if (dbg.x == 2 && p.w > 0.5) return float4(enc(tBase.SampleLevel(s0, i.uv0, 0).rgb), 1);
    if (p.w > 0.5) b *= tBase.Sample(s0, q.y > 0.5 ? i.uv1 : i.uv0);
    if (p.y == 1 && b.a < p.z) discard;
    float ao = 1;
    if (q.x > 0.5) ao = lerp(1, tAo.Sample(s0, q.z > 0.5 ? i.uv1 : i.uv0).r, p.x);
    float3 n = dot(i.n, i.n) > 1e-12 ? normalize(i.n) : float3(0, 1, 0);
    float l = 0.55 + 0.6 * abs(dot(n, normalize(float3(0.4, 0.8, 0.35))));
    float3 e = emissive.rgb;
    if (q.w > 0.5) e *= tEm.Sample(s0, emissive.w > 0.5 ? i.uv1 : i.uv0).rgb;
    return float4(enc(b.rgb * ao * l + e), p.y == 2 ? b.a : 1);
}
)";

struct DrawCB { XMFLOAT4X4 world, viewProj; float base[4], emissive[4], p[4], q[4], dbg[4]; };

struct Scene {
    std::vector<ComPtr<ID3D11ShaderResourceView>> srgb, linear;
    std::vector<Material> mats;
    std::vector<std::vector<Prim>> meshes;
    std::vector<Node> nodes;
    std::vector<Channel> channels;
    float lo[3], hi[3], animLen = 0;
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> cb; ComPtr<ID3D11SamplerState> sampler; ComPtr<ID3D11RasterizerState> raster, rasterCull;
    ComPtr<ID3D11DepthStencilState> depthWrite, depthRead; ComPtr<ID3D11BlendState> blendAlpha;
    ComPtr<ID3D11ShaderResourceView> white;
    Prim room{}; Material roomMat{};   // the benchmark's background (see MakeRoom)
};

bool compile(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& err) {
    ComPtr<ID3DBlob> e;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &e))) {
        err = std::string("shader ") + entry + ": " + (e ? (const char*)e->GetBufferPointer() : "?");
        return false;
    }
    return true;
}

// The diorama alone leaves most of the (very wide) Vision Pro view as flat sky, which NVENC encodes for free. The benchmark puts it
// in a room (walls, floor and ceiling 4-6 m away, in the normalized scene space) tiled with the scene's own painted atlas: detail
// everywhere, at a depth that gives stereo disparity and parallax as the head moves.
void MakeRoom(ID3D11Device* dev, Scene& s) {
    std::map<int, int> uses;
    for (const auto& m : s.mats) if (m.baseTex >= 0 && m.alphaMode == 0) uses[m.baseTex]++;   // the opaque materials' atlas
    int atlas = -1, best = 0;
    for (auto& [t, n] : uses) if (n > best) { best = n; atlas = t; }
    if (atlas < 0) return;
    const float X = 5.f, Y0 = -1.25f, Y1 = 4.f, Z = 5.f, tile = 2.5f;
    std::vector<float> pos, nrm, uv0;
    std::vector<uint32_t> idx;
    // each face: 8 x 8 quads (so the shading varies a little across it), normals pointing inwards
    auto face = [&](XMFLOAT3 o, XMFLOAT3 u, XMFLOAT3 v, XMFLOAT3 n, float lu, float lv) {
        const int N = 8; const uint32_t b = (uint32_t)(pos.size() / 3);
        for (int j = 0; j <= N; j++) for (int i = 0; i <= N; i++) {
            const float a = i / (float)N, c = j / (float)N;
            pos.insert(pos.end(), { o.x + u.x * a + v.x * c, o.y + u.y * a + v.y * c, o.z + u.z * a + v.z * c });
            nrm.insert(nrm.end(), { n.x, n.y, n.z });
            uv0.insert(uv0.end(), { a * lu / tile, c * lv / tile });
        }
        for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
            const uint32_t k = b + j * (N + 1) + i;
            idx.insert(idx.end(), { k, k + 1, k + N + 1, k + 1, k + N + 2, k + N + 1 });
        }
    };
    const float W = 2 * X, H = Y1 - Y0, D = 2 * Z;
    face({ -X, Y1, -Z }, { W, 0, 0 }, { 0, -H, 0 }, { 0, 0, 1 }, W, H);    // back wall
    face({ X, Y1, Z }, { -W, 0, 0 }, { 0, -H, 0 }, { 0, 0, -1 }, W, H);    // wall behind the viewer
    face({ -X, Y1, Z }, { 0, 0, -D }, { 0, -H, 0 }, { 1, 0, 0 }, D, H);    // left
    face({ X, Y1, -Z }, { 0, 0, D }, { 0, -H, 0 }, { -1, 0, 0 }, D, H);    // right
    face({ -X, Y0, -Z }, { W, 0, 0 }, { 0, 0, D }, { 0, 1, 0 }, W, D);     // floor
    face({ -X, Y1, Z }, { W, 0, 0 }, { 0, 0, -D }, { 0, -1, 0 }, W, D);    // ceiling
    const uint32_t nv = (uint32_t)(pos.size() / 3);
    std::vector<float> uv1(uv0.size(), 0.f);
    const float* src[4] = { pos.data(), nrm.data(), uv0.data(), uv1.data() };
    const size_t sizes[4] = { nv * 12u, nv * 12u, nv * 8u, nv * 8u };
    for (int a = 0; a < 4; a++) {
        D3D11_BUFFER_DESC bd{ (UINT)sizes[a], D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
        D3D11_SUBRESOURCE_DATA sd{ src[a] };
        dev->CreateBuffer(&bd, &sd, &s.room.vb[a]);
    }
    D3D11_BUFFER_DESC bd{ (UINT)(idx.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
    D3D11_SUBRESOURCE_DATA sd{ idx.data() };
    dev->CreateBuffer(&bd, &sd, &s.room.ib);
    s.room.count = (uint32_t)idx.size();
    s.roomMat = Material{ { 0.85f, 0.85f, 0.85f, 1 }, atlas, 0, -1, 0, 0, { 0, 0, 0 }, -1, 0, 0, 0.5f, 1 };
}

bool LoadScene(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char* path, Scene& s, std::string& err) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) { err = std::string("cannot open ") + path; return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(n);
    fread(data.data(), 1, n, f); fclose(f);
    Reader r{ data.data(), data.data() + data.size() };
    char magic[4]; r.take(magic, 4);
    if (memcmp(magic, "VAB1", 4) != 0 || r.get<uint32_t>() != 1) { err = "not a VAB1 scene"; return false; }
    // textures: RGBA8, typeless with an sRGB view (base colour, emissive) and a linear one (occlusion), full mip chain
    const uint32_t nImg = r.get<uint32_t>();
    for (uint32_t i = 0; i < nImg && r.ok; i++) {
        r.get<uint32_t>();
        const uint32_t bytes = r.get<uint32_t>();
        const uint8_t* src = r.skip(bytes);
        int w = 0, h = 0, c = 0;
        stbi_uc* px = stbi_load_from_memory(src, (int)bytes, &w, &h, &c, 4);
        if (!px) { err = "texture decode failed"; return false; }
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 0; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &t))) { stbi_image_free(px); err = "texture create failed"; return false; }
        ctx->UpdateSubresource(t.Get(), 0, nullptr, px, w * 4, 0);
        stbi_image_free(px);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = (UINT)-1;
        ComPtr<ID3D11ShaderResourceView> lin, sr;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dev->CreateShaderResourceView(t.Get(), &sd, &lin);
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; dev->CreateShaderResourceView(t.Get(), &sd, &sr);
        ctx->GenerateMips(lin.Get());
        s.linear.push_back(lin); s.srgb.push_back(sr);
    }
    const uint32_t nMat = r.get<uint32_t>();
    s.mats.resize(nMat);
    for (auto& m : s.mats) r.take(&m, sizeof(Material));
    const uint32_t nMesh = r.get<uint32_t>();
    s.meshes.resize(nMesh);
    for (auto& mesh : s.meshes) {
        const uint32_t np = r.get<uint32_t>();
        for (uint32_t k = 0; k < np && r.ok; k++) {
            Prim p{};
            p.material = r.get<int32_t>();
            const uint32_t nv = r.get<uint32_t>(), ni = r.get<uint32_t>();
            const size_t sizes[4] = { nv * 12u, nv * 12u, nv * 8u, nv * 8u };
            for (int a = 0; a < 4; a++) {
                const uint8_t* src = r.skip(sizes[a]);
                D3D11_BUFFER_DESC bd{ (UINT)std::max<size_t>(sizes[a], 16), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
                D3D11_SUBRESOURCE_DATA sd{ src };
                if (nv) dev->CreateBuffer(&bd, &sd, &p.vb[a]);
            }
            const uint8_t* idx = r.skip(ni * 4u);
            D3D11_BUFFER_DESC bd{ std::max<UINT>(ni * 4u, 16), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
            D3D11_SUBRESOURCE_DATA sd{ idx };
            if (ni) dev->CreateBuffer(&bd, &sd, &p.ib);
            p.count = ni;
            mesh.push_back(p);
        }
    }
    const uint32_t nNode = r.get<uint32_t>();
    s.nodes.resize(nNode);
    for (auto& nd : s.nodes) {
        nd.parent = r.get<int32_t>(); nd.mesh = r.get<int32_t>();
        r.take(nd.t, 12); r.take(nd.r, 16); r.take(nd.s, 12); nd.hasMatrix = r.get<uint32_t>(); r.take(nd.m, 64);
    }
    const uint32_t nCh = r.get<uint32_t>();
    s.channels.resize(nCh);
    for (auto& c : s.channels) {
        c.node = r.get<int32_t>(); c.path = r.get<uint32_t>(); c.interp = r.get<uint32_t>(); c.keys = r.get<uint32_t>();
        const int comps = c.path == 1 ? 4 : 3;
        c.times.resize(c.keys); r.take(c.times.data(), c.keys * 4u);
        c.values.resize((size_t)c.keys * comps * (c.interp == 2 ? 3 : 1)); r.take(c.values.data(), c.values.size() * 4);
    }
    r.take(s.lo, 12); r.take(s.hi, 12); s.animLen = r.get<float>();
    if (!r.ok) { err = "truncated scene file"; return false; }

    ComPtr<ID3DBlob> vsb, psb;
    if (!compile(kSceneHlsl, "vs", "vs_5_0", vsb, err) || !compile(kSceneHlsl, "ps", "ps_5_0", psb, err)) return false;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s.vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s.ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 2, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 3, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    dev->CreateInputLayout(il, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &s.layout);
    D3D11_BUFFER_DESC cbd{ sizeof(DrawCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    dev->CreateBuffer(&cbd, nullptr, &s.cb);
    D3D11_SAMPLER_DESC smp{}; smp.Filter = D3D11_FILTER_ANISOTROPIC; smp.MaxAnisotropy = 8;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&smp, &s.sampler);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = TRUE;
    dev->CreateRasterizerState(&rd, &s.raster);
    // single-sided materials: glTF front faces are counter-clockwise (the scene's toon outlines are inverted shells that only
    // work with back faces culled)
    rd.CullMode = D3D11_CULL_BACK; rd.FrontCounterClockwise = TRUE;
    dev->CreateRasterizerState(&rd, &s.rasterCull);
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
    dev->CreateDepthStencilState(&dd, &s.depthWrite);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&dd, &s.depthRead);
    D3D11_BLEND_DESC bl{}; auto& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE; rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD; rt.RenderTargetWriteMask = 0xF;
    dev->CreateBlendState(&bl, &s.blendAlpha);
    MakeRoom(dev, s);
    return true;
}

// animation at time t (looped), then world matrices (row-vector convention: v * local * parentWorld)
void NodeWorlds(const Scene& s, double t, std::vector<XMMATRIX>& world) {
    std::vector<Node> nd = s.nodes;
    const float tt = s.animLen > 0 ? (float)fmod(t, (double)s.animLen) : 0.f;
    for (const auto& c : s.channels) {
        if (c.keys == 0 || c.node < 0 || c.node >= (int)nd.size()) continue;
        const int comps = c.path == 1 ? 4 : 3, stride = comps * (c.interp == 2 ? 3 : 1), off = c.interp == 2 ? comps : 0;
        uint32_t k = 0;
        while (k + 1 < c.keys && c.times[k + 1] <= tt) k++;
        float v[4];
        const float* a = &c.values[(size_t)k * stride + off];
        if (k + 1 >= c.keys || c.interp == 1 || tt <= c.times[0]) {
            memcpy(v, a, comps * 4);
        } else {
            const float t0 = c.times[k], t1 = c.times[k + 1], dt = t1 - t0, u = dt > 0 ? (tt - t0) / dt : 0.f;
            const float* b = &c.values[(size_t)(k + 1) * stride + off];
            if (c.interp == 2) {
                const float* m0 = &c.values[(size_t)k * stride + 2 * comps];
                const float* m1 = &c.values[(size_t)(k + 1) * stride];
                const float u2 = u * u, u3 = u2 * u;
                for (int i = 0; i < comps; i++) v[i] = (2 * u3 - 3 * u2 + 1) * a[i] + (u3 - 2 * u2 + u) * dt * m0[i] + (-2 * u3 + 3 * u2) * b[i] + (u3 - u2) * dt * m1[i];
            } else if (c.path == 1) {
                XMStoreFloat4((XMFLOAT4*)v, XMQuaternionSlerp(XMLoadFloat4((const XMFLOAT4*)a), XMLoadFloat4((const XMFLOAT4*)b), u));
            } else {
                for (int i = 0; i < comps; i++) v[i] = a[i] + (b[i] - a[i]) * u;
            }
            if (c.path == 1) XMStoreFloat4((XMFLOAT4*)v, XMQuaternionNormalize(XMLoadFloat4((const XMFLOAT4*)v)));
        }
        Node& n = nd[c.node];
        n.hasMatrix = 0;
        memcpy(c.path == 0 ? n.t : c.path == 1 ? n.r : n.s, v, comps * 4);
    }
    world.assign(nd.size(), XMMatrixIdentity());
    std::vector<int> done(nd.size(), 0);
    std::function<XMMATRIX(int)> get = [&](int i) -> XMMATRIX {
        if (done[i]) return world[i];
        const Node& n = nd[i];
        XMMATRIX local = n.hasMatrix ? XMMATRIX(n.m)
            : XMMatrixScaling(n.s[0], n.s[1], n.s[2]) * XMMatrixRotationQuaternion(XMLoadFloat4((const XMFLOAT4*)n.r)) * XMMatrixTranslation(n.t[0], n.t[1], n.t[2]);
        world[i] = n.parent >= 0 && n.parent < (int)nd.size() ? local * get(n.parent) : local;
        done[i] = 1;
        return world[i];
    };
    for (size_t i = 0; i < nd.size(); i++) get((int)i);
}

// OpenXR-style projection from positive tangents (left, right, up, down), right handed, D3D depth [0, 1]
XMMATRIX Projection(const float* tan, float n, float f) {
    const float l = tan[0], r = tan[1], u = tan[2], d = tan[3];
    return XMMATRIX(2 / (r + l), 0, 0, 0,
                    0, 2 / (u + d), 0, 0,
                    (r - l) / (r + l), (u - d) / (u + d), f / (n - f), -1,
                    0, 0, n * f / (n - f), 0);
}

// The scripted head: looking around the diorama (scene normalized to ~3 m wide), deterministic in t
void HeadPose(double t, XMVECTOR& pos, XMVECTOR& rot) {
    const double tau = 6.283185307179586;
    const float yaw = (float)((25.0 * sin(tau * 0.13 * t) + 8.0 * sin(tau * 0.31 * t)) * XM_PI / 180.0);
    const float pitch = (float)((-6.0 + 7.0 * sin(tau * 0.23 * t)) * XM_PI / 180.0);
    pos = XMVectorSet((float)(0.15 * sin(tau * 0.07 * t)), 0.2f, (float)(1.9 + 0.1 * sin(tau * 0.05 * t)), 1);
    rot = XMQuaternionRotationRollPitchYaw(pitch, yaw, 0);
}

void RenderScene(Scene& s, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int W, int H, double t,
                 const float* fov8, float ipd) {
    const float clear[4] = { 0.62f, 0.72f, 0.82f, 1 }; // sky-ish, not black: nothing the encoder gets for free
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1, 0);
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    ctx->IASetInputLayout(s.layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(s.vs.Get(), nullptr, 0); ctx->PSSetShader(s.ps.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, s.cb.GetAddressOf()); ctx->PSSetConstantBuffers(0, 1, s.cb.GetAddressOf());
    ctx->PSSetSamplers(0, 1, s.sampler.GetAddressOf());
    ctx->RSSetState(s.raster.Get());
    std::vector<XMMATRIX> world;
    NodeWorlds(s, t, world);
    const float ext = std::max({ s.hi[0] - s.lo[0], s.hi[1] - s.lo[1], s.hi[2] - s.lo[2] });
    const float k = ext > 0 ? 3.0f / ext : 1.f;
    const XMMATRIX norm = XMMatrixTranslation(-(s.lo[0] + s.hi[0]) / 2, -(s.lo[1] + s.hi[1]) / 2, -(s.lo[2] + s.hi[2]) / 2) * XMMatrixScaling(k, k, k);
    XMVECTOR hp, hr;
    HeadPose(t, hp, hr);
    for (int eye = 0; eye < 2; eye++) {
        const XMVECTOR ep = XMVectorAdd(hp, XMVector3Rotate(XMVectorSet(eye ? ipd / 2 : -ipd / 2, 0, 0, 0), hr));
        const XMMATRIX view = XMMatrixLookToRH(ep, XMVector3Rotate(XMVectorSet(0, 0, -1, 0), hr), XMVector3Rotate(XMVectorSet(0, 1, 0, 0), hr));
        const XMMATRIX vp = view * Projection(fov8 + eye * 4, 0.05f, 100.f);
        D3D11_VIEWPORT v{ (float)(eye * W / 2), 0, (float)(W / 2), (float)H, 0, 1 };
        ctx->RSSetViewports(1, &v);
        auto draw = [&](const Prim& p, const Material* mat, const XMMATRIX& w) {
            DrawCB cb{};
            static const float dbgMode = [] { const char* v = getenv("VISIONALVR_BENCH_DEBUG"); return v ? (float)atof(v) : 0.f; }();
            cb.dbg[0] = dbgMode;
            XMStoreFloat4x4(&cb.world, XMMatrixTranspose(w));
            XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(vp));
            ID3D11ShaderResourceView* srv[3] = { nullptr, nullptr, nullptr };
            if (mat) {
                memcpy(cb.base, mat->base, 16);
                memcpy(cb.emissive, mat->emissive, 12);
                cb.emissive[3] = (float)mat->emUv;
                cb.p[0] = mat->aoStrength; cb.p[1] = (float)mat->alphaMode; cb.p[2] = mat->cutoff;
                if (mat->baseTex >= 0 && mat->baseTex < (int)s.srgb.size()) { srv[0] = s.srgb[mat->baseTex].Get(); cb.p[3] = 1; }
                // (occlusion not applied: this asset's occlusion map is ~0.2 almost everywhere, made for ambient light only;
                // multiplied into the whole shading it darkens the scene 5x)
                if (mat->emTex >= 0 && mat->emTex < (int)s.srgb.size()) { srv[2] = s.srgb[mat->emTex].Get(); cb.q[3] = 1; }
                cb.q[1] = (float)mat->baseUv; cb.q[2] = (float)mat->aoUv;
            } else {
                cb.base[0] = cb.base[1] = cb.base[2] = cb.base[3] = 0.8f;
            }
            ctx->RSSetState(mat && !mat->doubleSided ? s.rasterCull.Get() : s.raster.Get());
            D3D11_MAPPED_SUBRESOURCE ms;
            ctx->Map(s.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
            memcpy(ms.pData, &cb, sizeof cb);
            ctx->Unmap(s.cb.Get(), 0);
            ctx->PSSetShaderResources(0, 3, srv);
            ID3D11Buffer* vbs[4] = { p.vb[0].Get(), p.vb[1].Get(), p.vb[2].Get(), p.vb[3].Get() };
            const UINT strides[4] = { 12, 12, 8, 8 }, offs[4] = { 0, 0, 0, 0 };
            ctx->IASetVertexBuffers(0, 4, vbs, strides, offs);
            ctx->IASetIndexBuffer(p.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed(p.count, 0, 0);
        };
        for (int pass = 0; pass < 2; pass++) { // opaque/mask, then blended
            ctx->OMSetDepthStencilState(pass ? s.depthRead.Get() : s.depthWrite.Get(), 0);
            ctx->OMSetBlendState(pass ? s.blendAlpha.Get() : nullptr, nullptr, 0xFFFFFFFF);
            if (pass == 0 && s.room.count) draw(s.room, &s.roomMat, XMMatrixIdentity());
            for (size_t i = 0; i < s.nodes.size(); i++) {
                const int m = s.nodes[i].mesh;
                if (m < 0 || m >= (int)s.meshes.size()) continue;
                for (const Prim& p : s.meshes[m]) {
                    if (!p.count || !p.vb[0]) continue;
                    const Material* mat = p.material >= 0 && p.material < (int)s.mats.size() ? &s.mats[p.material] : nullptr;
                    if (((mat && mat->alphaMode == 2) ? 1 : 0) != pass) continue;
                    draw(p, mat, world[i] * norm);
                }
            }
        }
    }
    ID3D11RenderTargetView* none = nullptr;
    ctx->OMSetRenderTargets(1, &none, nullptr);
}

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

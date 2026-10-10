// The benchmark scene (.vab from tools/bench_scene/convert.cpp): loader, textured room, node animation and a D3D11 renderer.
// Shared by the quality benchmark (bench.cpp, stereo side by side with its scripted camera) and the OpenXR probe app
// (tools/probe/xr_probe.cpp, XR_PROBE_SCENE: one eye per swapchain image with the runtime's view poses), so the harness can
// stream a full scene without a game. Header-only on purpose: both are single-translation-unit builds. The including file
// defines STB_IMAGE_IMPLEMENTATION once before this header (stb_image decodes the scene's textures).
#pragma once
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#ifndef STBI_INCLUDE_STB_IMAGE_H
#include "stb_image.h"   // (bench.cpp includes it first, with the implementation)
#endif

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {
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

// Per-frame state of the scene: node transforms at time t and the placement (normalisation to ~3 m, plus an optional offset).
struct SceneFrame { std::vector<XMMATRIX> world; XMMATRIX norm; };

void BeginSceneFrame(Scene& s, ID3D11DeviceContext* ctx, double t, SceneFrame& f, const XMMATRIX& place = XMMatrixIdentity()) {
    ctx->IASetInputLayout(s.layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(s.vs.Get(), nullptr, 0); ctx->PSSetShader(s.ps.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, s.cb.GetAddressOf()); ctx->PSSetConstantBuffers(0, 1, s.cb.GetAddressOf());
    ctx->PSSetSamplers(0, 1, s.sampler.GetAddressOf());
    ctx->RSSetState(s.raster.Get());
    NodeWorlds(s, t, f.world);
    const float ext = std::max({ s.hi[0] - s.lo[0], s.hi[1] - s.lo[1], s.hi[2] - s.lo[2] });
    const float k = ext > 0 ? 3.0f / ext : 1.f;
    f.norm = XMMatrixTranslation(-(s.lo[0] + s.hi[0]) / 2, -(s.lo[1] + s.hi[1]) / 2, -(s.lo[2] + s.hi[2]) / 2) * XMMatrixScaling(k, k, k) * place;
}

// One eye: viewport + view-projection into the bound targets (the caller clears and binds them).
void DrawSceneEye(Scene& s, ID3D11DeviceContext* ctx, const SceneFrame& f, const D3D11_VIEWPORT& v, const XMMATRIX& vp, const XMMATRIX& placeRoom = XMMatrixIdentity()) {
    const std::vector<XMMATRIX>& world = f.world;
    const XMMATRIX& norm = f.norm;
    {
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
            if (pass == 0 && s.room.count) draw(s.room, &s.roomMat, placeRoom);
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
}

// The benchmark's stereo render: both eyes side by side into one target, the scripted HeadPose camera.
void RenderScene(Scene& s, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int W, int H, double t,
                 const float* fov8, float ipd) {
    const float clear[4] = { 0.62f, 0.72f, 0.82f, 1 }; // sky-ish, not black: nothing the encoder gets for free
    ctx->ClearRenderTargetView(rtv, clear);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1, 0);
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    SceneFrame f;
    BeginSceneFrame(s, ctx, t, f);
    XMVECTOR hp, hr;
    HeadPose(t, hp, hr);
    for (int eye = 0; eye < 2; eye++) {
        const XMVECTOR ep = XMVectorAdd(hp, XMVector3Rotate(XMVectorSet(eye ? ipd / 2 : -ipd / 2, 0, 0, 0), hr));
        const XMMATRIX view = XMMatrixLookToRH(ep, XMVector3Rotate(XMVectorSet(0, 0, -1, 0), hr), XMVector3Rotate(XMVectorSet(0, 1, 0, 0), hr));
        const XMMATRIX vp = view * Projection(fov8 + eye * 4, 0.05f, 100.f);
        D3D11_VIEWPORT v{ (float)(eye * W / 2), 0, (float)(W / 2), (float)H, 0, 1 };
        DrawSceneEye(s, ctx, f, v, vp);
    }
    ID3D11RenderTargetView* none = nullptr;
    ctx->OMSetRenderTargets(1, &none, nullptr);
}

} // namespace

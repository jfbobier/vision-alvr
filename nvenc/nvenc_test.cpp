// Standalone NVENC test: synthetic SBS stereo texture -> unmodified ALVR VideoEncoderNVENC -> .hevc + stats.
// nvenc_test.exe --out f.hevc [--w 7104 --h 3200 --frames 120 --fps 90 --mbps 250 --bits 10|8 --codec hevc|h264]
#define NOMINMAX
#include "VideoEncoderNVENC.h"
#include "alvr_server/Settings.h"
#include "shim.h"
#include "ALVR-common/exception.h"
#include <dxgi.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <numeric>

static double nowMs() { using namespace std::chrono; return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char** argv) {
    int W = 7104, H = 3200, frames = 120, fps = 90, bits = 10, mbps = 250, idrAt = 60;
    std::string out = "nvenc_test.hevc", codec = "hevc";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i]; auto nx = [&]() { return i + 1 < argc ? argv[++i] : (char*)""; };
        if (a == "--out") out = nx(); else if (a == "--w") W = atoi(nx()); else if (a == "--h") H = atoi(nx());
        else if (a == "--frames") frames = atoi(nx()); else if (a == "--fps") fps = atoi(nx());
        else if (a == "--bits") bits = atoi(nx()); else if (a == "--mbps") mbps = atoi(nx());
        else if (a == "--codec") codec = nx(); else if (a == "--idr-at") idrAt = atoi(nx()); else if (a == "-v") g_shim.verbose = true;
    }
    auto& S = Settings::Instance();
    S.m_refreshRate = fps; S.m_use10bitEncoder = bits == 10; S.m_codec = codec == "h264" ? ALVR_CODEC_H264 : ALVR_CODEC_HEVC;
    g_shim.bitrateBps = (unsigned long long)mbps * 1000000ULL; g_shim.framerate = (float)fps;
    g_shim.out = fopen(out.c_str(), "wb");
    if (!g_shim.out) { fprintf(stderr, "cannot open %s\n", out.c_str()); return 2; }

    // pick the NVIDIA adapter
    IDXGIFactory1* fac; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
    int adapter = -1; IDXGIAdapter1* ad;
    for (UINT i = 0; fac->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d); ad->Release();
        if (d.VendorId == 0x10DE && adapter < 0) { adapter = (int)i; wprintf(L"adapter %d: %s\n", i, d.Description); }
    }
    if (adapter < 0) { fprintf(stderr, "no NVIDIA adapter\n"); return 2; }

    auto render = std::make_shared<CD3DRender>();
    if (!render->Initialize((uint32_t)adapter)) { fprintf(stderr, "CD3DRender init failed\n"); return 2; }

    DXGI_FORMAT fmt = bits == 10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(render->GetDevice()->CreateTexture2D(&td, nullptr, &tex))) { fprintf(stderr, "CreateTexture2D failed\n"); return 2; }

    // scrolling base image: smooth gradient + hash noise, wider than W so each frame is a shifted window
    const int PAD = 512; const int BW = W + PAD;
    std::vector<uint32_t> base((size_t)BW * H), frame((size_t)W * H);
    for (int y = 0; y < H; y++) for (int x = 0; x < BW; x++) {
        uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
        int n = (h & 0xff) >> 3;                       // 0..31 noise
        int gx = x * 255 / BW, gy = y * 255 / H;
        int r = std::min(255, gx + n), g = std::min(255, gy + n), b = std::min(255, ((x / 64 + y / 64) & 1) ? 200 + n : 60 + n);
        if (bits == 10) base[(size_t)y * BW + x] = (r * 4) | ((g * 4) << 10) | ((b * 4) << 20) | (3u << 30);
        else            base[(size_t)y * BW + x] = r | (g << 8) | (b << 16) | (255u << 24);
    }

    auto enc = std::make_unique<VideoEncoderNVENC>(render, W, H);
    try { enc->Initialize(); } catch (Exception& e) { fprintf(stderr, "Initialize failed: %s\n", e.what()); return 3; }

    std::vector<double> encMs;
    double t0 = nowMs();
    for (int f = 0; f < frames; f++) {
        int off = (f * 7) % PAD;
        for (int y = 0; y < H; y++) memcpy(&frame[(size_t)y * W], &base[(size_t)y * BW + off], (size_t)W * 4);
        render->GetContext()->UpdateSubresource(tex, 0, nullptr, frame.data(), W * 4, 0);
        double a = nowMs();
        try { enc->Transmit(tex, f, 1000000ULL + f, f == 0 || f == idrAt); }
        catch (Exception& e) { fprintf(stderr, "Transmit failed at frame %d: %s\n", f, e.what()); return 4; }
        encMs.push_back(nowMs() - a);
    }
    double totalMs = nowMs() - t0;
    enc->Shutdown();
    fclose(g_shim.out);

    size_t bytes = 0, idrs = 0, idrBytes = 0; size_t maxP = 0;
    for (auto& p : g_shim.packets) { bytes += p.len; if (p.idr) { idrs++; idrBytes += p.len; } else maxP = std::max(maxP, p.len); }
    auto sorted = encMs; std::sort(sorted.begin(), sorted.end());
    double avg = std::accumulate(encMs.begin(), encMs.end(), 0.0) / encMs.size();
    printf("{\"w\":%d,\"h\":%d,\"bits\":%d,\"codec\":\"%s\",\"target_mbps\":%d,\"fps\":%d,\"frames_in\":%d,\"packets_out\":%zu,"
           "\"bytes\":%zu,\"idr_packets\":%zu,\"idr_bytes\":%zu,\"max_non_idr_bytes\":%zu,\"avg_mbps_at_target_fps\":%.1f,"
           "\"first_packet_bytes\":%zu,\"encode_ms_avg\":%.2f,\"encode_ms_p50\":%.2f,\"encode_ms_p95\":%.2f,\"encode_ms_max\":%.2f,\"wall_ms\":%.0f}\n",
           W, H, bits, codec.c_str(), mbps, fps, frames, g_shim.packets.size(), bytes, idrs, idrBytes, maxP,
           bytes * 8.0 * fps / frames / 1e6, g_shim.packets.empty() ? 0 : g_shim.packets[0].len,
           avg, sorted[sorted.size() / 2], sorted[(size_t)(sorted.size() * 0.95)], sorted.back(), totalMs);
    return g_shim.packets.size() == (size_t)frames ? 0 : 5;
}

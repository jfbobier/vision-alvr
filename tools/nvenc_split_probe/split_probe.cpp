// NVENC split-frame probe: which configurations make NVENC actually split a frame across its engines.
// Opens a D3D11 NVENC session, encodes synthetic frames with one configuration and prints one JSON line: whether the
// encoder initialised, the engine count, slices per frame (split-frame encoding writes one slice per strip, so > 1 means
// split is active) and encode time. One configuration per run; a driver script sweeps them.
//
//   split_probe.exe --w 4224 --h 1664 --codec hevc|av1 --preset 1..7 --tuning hq|ll|ull --split 0|1|2|3|15 --bits 8|10
//                   [--rc cbr|vbr|cqp] [--mbps 200] [--frames 90] [--wp 0|1]
//
// Build (VS x64 developer prompt): cl /nologo /O2 /EHsc /std:c++17 /I<dir of nvEncodeAPI.h> split_probe.cpp d3d11.lib dxgi.lib
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "nvEncodeAPI.h"

static double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static void Fail(const char* step, int code, const std::string& cfg) {
    printf("{%s, \"ok\": false, \"step\": \"%s\", \"nvenc_status\": %d}\n", cfg.c_str(), step, code);
    exit(1);
}

// Slices (VCL NAL units) in an Annex-B HEVC access unit.
static int CountHevcSlices(const uint8_t* p, size_t n) {
    int slices = 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            const int type = (p[i + 3] >> 1) & 0x3f;
            if (type <= 31) {
                slices++;
            }
            i += 2;
        }
    }
    return slices;
}

int main(int argc, char** argv) {
    int W = 4224, H = 1664, preset = 3, split = 0, bits = 10, mbps = 200, frames = 90, wp = 0;
    std::string tuning = "ll", rc = "cbr", codec = "hevc";
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--w") W = std::stoi(nx());
        else if (a == "--h") H = std::stoi(nx());
        else if (a == "--preset") preset = std::stoi(nx());
        else if (a == "--split") split = std::stoi(nx());
        else if (a == "--bits") bits = std::stoi(nx());
        else if (a == "--mbps") mbps = std::stoi(nx());
        else if (a == "--frames") frames = std::stoi(nx());
        else if (a == "--wp") wp = std::stoi(nx());
        else if (a == "--tuning") tuning = nx();
        else if (a == "--rc") rc = nx();
        else if (a == "--codec") codec = nx();
    }
    char cfgBuf[512];
    snprintf(cfgBuf, sizeof(cfgBuf),
             "\"w\": %d, \"h\": %d, \"codec\": \"%s\", \"preset\": %d, \"tuning\": \"%s\", \"rc\": \"%s\", \"split\": %d, "
             "\"bits\": %d, \"wp\": %d",
             W, H, codec.c_str(), preset, tuning.c_str(), rc.c_str(), split, bits, wp);
    const std::string cfg = cfgBuf;

    // D3D11 device on the first NVIDIA adapter
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (d.VendorId == 0x10DE) {
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    if (!adapter) Fail("no NVIDIA adapter", 0, cfg);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
        Fail("D3D11CreateDevice", 0, cfg);

    HMODULE lib = LoadLibraryA("nvEncodeAPI64.dll");
    if (!lib) Fail("load nvEncodeAPI64.dll", 0, cfg);
    auto create = (NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*))GetProcAddress(lib, "NvEncodeAPICreateInstance");
    NV_ENCODE_API_FUNCTION_LIST nv = {NV_ENCODE_API_FUNCTION_LIST_VER};
    if (!create || create(&nv) != NV_ENC_SUCCESS) Fail("NvEncodeAPICreateInstance", 0, cfg);

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op = {NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
    op.device = dev;
    op.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    op.apiVersion = NVENCAPI_VERSION;
    void* enc = nullptr;
    NVENCSTATUS st = nv.nvEncOpenEncodeSessionEx(&op, &enc);
    if (st != NV_ENC_SUCCESS) Fail("nvEncOpenEncodeSessionEx", st, cfg);

    const GUID codecGuid = codec == "av1" ? NV_ENC_CODEC_AV1_GUID : NV_ENC_CODEC_HEVC_GUID;
    NV_ENC_CAPS_PARAM cp = {NV_ENC_CAPS_PARAM_VER};
    cp.capsToQuery = NV_ENC_CAPS_NUM_ENCODER_ENGINES;
    int engines = 0;
    nv.nvEncGetEncodeCaps(enc, codecGuid, &cp, &engines);

    const GUID presets[] = {NV_ENC_PRESET_P1_GUID, NV_ENC_PRESET_P2_GUID, NV_ENC_PRESET_P3_GUID, NV_ENC_PRESET_P4_GUID,
                            NV_ENC_PRESET_P5_GUID, NV_ENC_PRESET_P6_GUID, NV_ENC_PRESET_P7_GUID};
    const GUID presetGuid = presets[std::clamp(preset, 1, 7) - 1];
    const NV_ENC_TUNING_INFO tune = tuning == "hq" ? NV_ENC_TUNING_INFO_HIGH_QUALITY
                                    : tuning == "ull" ? NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY
                                                      : NV_ENC_TUNING_INFO_LOW_LATENCY;
    NV_ENC_PRESET_CONFIG pc = {}; // SDK 12.2 added a `reserved` field after version: no positional initialiser
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    st = nv.nvEncGetEncodePresetConfigEx(enc, codecGuid, presetGuid, tune, &pc);
    if (st != NV_ENC_SUCCESS) Fail("nvEncGetEncodePresetConfigEx", st, cfg);
    NV_ENC_CONFIG ec = pc.presetCfg;
    // ALVR's low-latency shape: infinite GOP, P frames only
    ec.gopLength = NVENC_INFINITE_GOPLENGTH;
    ec.frameIntervalP = 1;
    if (rc == "cqp") {
        ec.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
    } else {
        ec.rcParams.rateControlMode = rc == "vbr" ? NV_ENC_PARAMS_RC_VBR : NV_ENC_PARAMS_RC_CBR;
        ec.rcParams.averageBitRate = ec.rcParams.maxBitRate = (uint32_t)mbps * 1000000u;
        ec.rcParams.vbvBufferSize = ec.rcParams.vbvInitialDelay = (uint32_t)mbps * 1000000u / 90;
    }
    const NV_ENC_BIT_DEPTH depth = bits == 10 ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
    if (codec == "av1") {
        ec.encodeCodecConfig.av1Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        ec.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
        ec.encodeCodecConfig.av1Config.inputBitDepth = ec.encodeCodecConfig.av1Config.outputBitDepth = depth;
    } else {
        ec.encodeCodecConfig.hevcConfig.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        ec.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        ec.encodeCodecConfig.hevcConfig.inputBitDepth = ec.encodeCodecConfig.hevcConfig.outputBitDepth = depth;
    }

    NV_ENC_INITIALIZE_PARAMS ip = {NV_ENC_INITIALIZE_PARAMS_VER};
    ip.encodeGUID = codecGuid;
    ip.presetGUID = presetGuid;
    ip.tuningInfo = tune;
    ip.encodeWidth = ip.darWidth = (uint32_t)W;
    ip.encodeHeight = ip.darHeight = (uint32_t)H;
    ip.frameRateNum = 90;
    ip.frameRateDen = 1;
    ip.enablePTD = 1;
    ip.encodeConfig = &ec;
    ip.splitEncodeMode = (uint32_t)split;
    ip.enableWeightedPrediction = (uint32_t)wp;
    const double t0 = NowMs();
    st = nv.nvEncInitializeEncoder(enc, &ip);
    const double initMs = NowMs() - t0;
    if (st != NV_ENC_SUCCESS) {
        printf("{%s, \"ok\": false, \"step\": \"nvEncInitializeEncoder\", \"nvenc_status\": %d, \"engines\": %d, \"error\": \"%s\"}\n",
               cfg.c_str(), st, engines, nv.nvEncGetLastErrorString ? nv.nvEncGetLastErrorString(enc) : "");
        return 1;
    }

    // a few textures of noise, cycled (motion for the encoder; constant content would encode in no time)
    const DXGI_FORMAT fmt = bits == 10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    const NV_ENC_BUFFER_FORMAT bfmt = bits == 10 ? NV_ENC_BUFFER_FORMAT_ABGR10 : NV_ENC_BUFFER_FORMAT_ARGB;
    const int kTex = 4;
    std::vector<uint32_t> pixels((size_t)W * H);
    uint32_t seed = 12345;
    std::vector<NV_ENC_REGISTERED_PTR> reg(kTex);
    for (int t = 0; t < kTex; t++) {
        for (size_t i = 0; i < pixels.size(); i++) {
            // smooth gradient plus noise in 8x8 blocks: compressible like a game frame, not like white noise
            seed = seed * 1664525u + 1013904223u;
            const uint32_t x = (uint32_t)(i % W), y = (uint32_t)(i / W);
            const uint32_t n = ((x / 8 + y / 8 * 977 + (uint32_t)t * 31) * 2654435761u) >> 26;
            const uint32_t v = ((x + y + (uint32_t)t * 16) & 0xff) ^ (n & 0x3f) ^ (seed >> 30);
            pixels[i] = bits == 10 ? (0x3u << 30) | ((v * 4) << 20) | (((255 - v) * 4) << 10) | (v * 2)
                                   : 0xff000000u | (v << 16) | ((255 - v) << 8) | (v / 2);
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)W;
        td.Height = (UINT)H;
        td.MipLevels = td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA sd = {pixels.data(), (UINT)W * 4, 0};
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(dev->CreateTexture2D(&td, &sd, &tex))) Fail("CreateTexture2D", 0, cfg);
        NV_ENC_REGISTER_RESOURCE rr = {NV_ENC_REGISTER_RESOURCE_VER};
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.resourceToRegister = tex;
        rr.width = (uint32_t)W;
        rr.height = (uint32_t)H;
        rr.bufferFormat = bfmt;
        rr.bufferUsage = NV_ENC_INPUT_IMAGE;
        st = nv.nvEncRegisterResource(enc, &rr);
        if (st != NV_ENC_SUCCESS) Fail("nvEncRegisterResource", st, cfg);
        reg[t] = rr.registeredResource;
    }
    NV_ENC_CREATE_BITSTREAM_BUFFER bb = {NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
    st = nv.nvEncCreateBitstreamBuffer(enc, &bb);
    if (st != NV_ENC_SUCCESS) Fail("nvEncCreateBitstreamBuffer", st, cfg);

    std::vector<double> ms;
    std::vector<int> slices;
    double bytes = 0;
    for (int f = 0; f < frames; f++) {
        NV_ENC_MAP_INPUT_RESOURCE mr = {NV_ENC_MAP_INPUT_RESOURCE_VER};
        mr.registeredResource = reg[f % kTex];
        st = nv.nvEncMapInputResource(enc, &mr);
        if (st != NV_ENC_SUCCESS) Fail("nvEncMapInputResource", st, cfg);
        NV_ENC_PIC_PARAMS pp = {NV_ENC_PIC_PARAMS_VER};
        pp.inputBuffer = mr.mappedResource;
        pp.bufferFmt = mr.mappedBufferFmt;
        pp.inputWidth = (uint32_t)W;
        pp.inputHeight = (uint32_t)H;
        pp.outputBitstream = bb.bitstreamBuffer;
        pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pp.inputTimeStamp = (uint64_t)f;
        const double s = NowMs();
        st = nv.nvEncEncodePicture(enc, &pp);
        if (st != NV_ENC_SUCCESS) Fail("nvEncEncodePicture", st, cfg);
        NV_ENC_LOCK_BITSTREAM lb = {NV_ENC_LOCK_BITSTREAM_VER};
        lb.outputBitstream = bb.bitstreamBuffer;
        st = nv.nvEncLockBitstream(enc, &lb);
        if (st != NV_ENC_SUCCESS) Fail("nvEncLockBitstream", st, cfg);
        const double e = NowMs() - s;
        if (f >= 10) { // skip the warm-up (IDR, rate control settling, clocks)
            ms.push_back(e);
            bytes += lb.bitstreamSizeInBytes;
            slices.push_back(codec == "av1" ? 0 : CountHevcSlices((const uint8_t*)lb.bitstreamBufferPtr, lb.bitstreamSizeInBytes));
        }
        nv.nvEncUnlockBitstream(enc, bb.bitstreamBuffer);
        nv.nvEncUnmapInputResource(enc, mr.mappedResource);
        Sleep(5); // paced a little like a stream (and so the GPU clocks behave like they do there)
    }
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double q) { return sorted.empty() ? 0.0 : sorted[(size_t)std::min<double>(sorted.size() - 1, q * sorted.size())]; };
    double sliceAvg = 0;
    int sliceMin = 1 << 30, sliceMax = 0;
    for (int s : slices) {
        sliceAvg += s;
        sliceMin = std::min(sliceMin, s);
        sliceMax = std::max(sliceMax, s);
    }
    if (!slices.empty()) sliceAvg /= slices.size();
    printf("{%s, \"ok\": true, \"engines\": %d, \"init_ms\": %.1f, \"slices_avg\": %.2f, \"slices_min\": %d, \"slices_max\": %d, "
           "\"enc_ms_p50\": %.2f, \"enc_ms_p95\": %.2f, \"kbytes_per_frame\": %.1f}\n",
           cfg.c_str(), engines, initMs, sliceAvg, slices.empty() ? 0 : sliceMin, sliceMax, pct(0.5), pct(0.95),
           ms.empty() ? 0.0 : bytes / ms.size() / 1024.0);
    nv.nvEncDestroyBitstreamBuffer(enc, bb.bitstreamBuffer);
    for (auto r : reg) nv.nvEncUnregisterResource(enc, r);
    nv.nvEncDestroyEncoder(enc);
    return 0;
}

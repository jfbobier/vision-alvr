// Compiles the unmodified ALVR NVENC sources + our shims/wrapper (nvenc/ in the harness repo, on the builder
// at %USERPROFILE%\openxr\nvenc) into a static library linked into alvr_host.
// NvEncoderD3D11.cpp is compiled from a build-time copy with ABGR10 -> DXGI_FORMAT_R10G10B10A2_UNORM
// (upstream maps it to R8G8B8A8_UNORM, which makes 10-bit encodes black; see docs/progress.md).
use std::{env, fs, path::{Path, PathBuf}};

/// A vendored source as text with LF line endings: a Windows clone with git's default core.autocrlf=true checks the files out
/// with CRLF, and the patch anchors below are written with "\n".
fn read_lf(path: &Path, what: &str) -> String {
    fs::read_to_string(path).expect(what).replace("\r\n", "\n")
}

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    // <openxr>/src/ALVR-v20.14.1/alvr/host_harness -> <openxr>/nvenc
    let nvenc = env::var("NVENC_DIR").map(PathBuf::from).unwrap_or_else(|_| manifest.join("../../../../nvenc"));
    let up = nvenc.join("upstream");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());

    let orig = read_lf(&up.join("platform/win32/NvEncoderD3D11.cpp"), "read NvEncoderD3D11.cpp");
    let needle = "case NV_ENC_BUFFER_FORMAT_ABGR10:";
    let pos = orig.find(needle).expect("ABGR10 case");
    let tail = &orig[pos..];
    let r = tail.find("DXGI_FORMAT_R8G8B8A8_UNORM").expect("R8G8B8A8 after ABGR10");
    let mut patched = String::new();
    patched.push_str(&orig[..pos]);
    patched.push_str(&tail[..r]);
    patched.push_str("DXGI_FORMAT_R10G10B10A2_UNORM");
    patched.push_str(&tail[r + "DXGI_FORMAT_R8G8B8A8_UNORM".len()..]);
    let patched_path = out.join("NvEncoderD3D11_fix10.cpp");
    fs::write(&patched_path, patched).unwrap();

    // FFR (ALVR's foveated encoding pass), compiled from a build-time copy whose output texture follows the encoder's bit
    // depth: upstream hard-codes R8G8B8A8_UNORM_SRGB, which would silently turn a 10-bit encode into an 8-bit one.
    let ffr = read_lf(&up.join("platform/win32/FFR.cpp"), "read FFR.cpp");
    assert!(ffr.contains("DXGI_FORMAT_R8G8B8A8_UNORM_SRGB"));
    let ffr = ffr.replace(
        "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB",
        "(Settings::Instance().m_use10bitEncoder ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM)",
    );
    let ffr_path = out.join("FFR_fix10.cpp");
    fs::write(&ffr_path, ffr).unwrap();

    // shader bytecode (normally include_bytes! on the Rust side) from the upstream .cso files
    let mut cso = String::from("extern \"C\" {\n");
    for (name, file) in [("QUAD_SHADER", "QuadVertexShader.cso"), ("COMPRESS_AXIS_ALIGNED", "CompressAxisAlignedPixelShader.cso")] {
        let bytes = fs::read(up.join("platform/win32").join(file)).expect("read cso");
        cso.push_str(&format!("static const unsigned char {name}_DATA[] = {{{}}};\n", bytes.iter().map(|b| b.to_string()).collect::<Vec<_>>().join(",")));
        cso.push_str(&format!("const unsigned char* {name}_CSO_PTR = {name}_DATA;\nunsigned int {name}_CSO_LEN = {};\n", bytes.len()));
    }
    cso.push_str("}\n");
    let cso_path = out.join("cso_data.cpp");
    fs::write(&cso_path, cso).unwrap();

    // VideoEncoderNVENC: set NVENC's split-frame mode (SDK >= 12.1) from our setting, and expose the engine count.
    // Build-time copies; the vendored upstream files stay unmodified.
    let enc_cpp = read_lf(&up.join("platform/win32/VideoEncoderNVENC.cpp"), "read VideoEncoderNVENC.cpp");
    let anchor = "    initializeParams.encodeWidth = initializeParams.darWidth = renderWidth;";
    assert!(enc_cpp.contains(anchor));
    let enc_cpp = bitdepth_compat(&enc_cpp);
    // upstream turns every NV_ENC_ERR_INVALID_PARAM into a misleading "GPU does not support H.265": keep the real message
    let enc_cpp = enc_cpp.replace(
        "\"This GPU does not support H.265 encoding. (NvEncoderCuda NV_ENC_ERR_INVALID_PARAM)\"",
        "\"NVENC rejected the encoder parameters (NV_ENC_ERR_INVALID_PARAM): %hs\", e.what()",
    );
    let enc_cpp = enc_cpp.replace(
        anchor,
        "    initializeParams.splitEncodeMode = (NV_ENC_SPLIT_ENCODE_MODE)Settings::Instance().m_nvencSplitEncodeMode;\n    initializeParams.encodeWidth = initializeParams.darWidth = renderWidth;",
    );
    // QP delta map (nvenc/hostlib/nvh.cpp builds it): enable the per-CTB delta mode in the rate-control params and hand the
    // map to every frame.
    let rc_anchor = "    encodeConfig.rcParams.lowDelayKeyFrameScale = 1;\n";
    assert!(enc_cpp.contains(rc_anchor), "lowDelayKeyFrameScale anchor not found");
    let enc_cpp = enc_cpp.replacen(rc_anchor, &format!("{rc_anchor}    if (Settings::Instance().m_qpMapEnabled) {{\n        encodeConfig.rcParams.qpMapMode = NV_ENC_QP_MAP_DELTA;\n    }}\n"), 1);
    let idr_anchor = "    m_NvNecoder->EncodeFrame(vPacket, &picParams);";
    assert!(enc_cpp.contains(idr_anchor), "EncodeFrame anchor not found");
    let enc_cpp = enc_cpp.replacen(idr_anchor, "    if (g_nvh_qp_map && Settings::Instance().m_qpMapEnabled) {\n        picParams.qpDeltaMap = g_nvh_qp_map;\n        picParams.qpDeltaMapSize = g_nvh_qp_map_size;\n    }\n    if (g_nvh_recon_ptr) {\n        picParams.outputReconBuffer = g_nvh_recon_ptr;\n        picParams.encodePicFlags |= NV_ENC_PIC_FLAG_OUTPUT_RECON_FRAME;\n    }\n    m_NvNecoder->EncodeFrame(vPacket, &picParams);", 1);
    // benchmark (nvenc/hostlib/bench.cpp): NVENC's reconstructed frame, i.e. the picture a decoder gets, for the quality metric
    let enc_cpp = enc_cpp.replacen(anchor, &format!("    initializeParams.enableReconFrameOutput = g_nvh_recon_enable ? 1 : 0;\n{anchor}"), 1);
    let inc_anchor = "#include \"VideoEncoderNVENC.h\"";
    assert!(enc_cpp.contains(inc_anchor), "include anchor not found");
    let enc_cpp = enc_cpp.replacen(inc_anchor, &format!("{inc_anchor}\n#include <cstdio>\n#include <string>\nextern \"C\" {{ extern int8_t* g_nvh_qp_map; extern uint32_t g_nvh_qp_map_size; extern void* g_nvh_recon_ptr; extern int g_nvh_recon_enable; }}"), 1);
    let mut enc_cpp = enc_cpp;
    enc_cpp.push_str(DESCRIBE_CPP);
    enc_cpp.push_str("\nint VideoEncoderNVENC::GetEncoderEngineCount() {\n    return m_NvNecoder ? m_NvNecoder->GetCapabilityValue(NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_NUM_ENCODER_ENGINES) : 0;\n}\n");
    let enc_cpp_path = out.join("VideoEncoderNVENC_split.cpp");
    fs::write(&enc_cpp_path, enc_cpp).unwrap();
    let enc_h = read_lf(&up.join("platform/win32/VideoEncoderNVENC.h"), "read VideoEncoderNVENC.h");
    assert!(enc_h.contains("    void Shutdown();"));
    fs::write(
        out.join("VideoEncoderNVENC.h"),
        enc_h.replace("    void Shutdown();", "    void Shutdown();\n    int GetEncoderEngineCount();\n    std::string DescribeConfig();\n    NvEncoder* GetNvEncoder() { return m_NvNecoder.get(); }"),
    )
    .unwrap();
    // NvEncoder.h: public accessors for the session handle and API table (the benchmark registers the reconstructed-frame surface
    // on NVENC's own session). Its includers are copied next to it so every quoted include resolves to the same copies.
    let nv_h = read_lf(&up.join("platform/win32/NvEncoder.h"), "read NvEncoder.h");
    let acc_anchor = "    uint32_t GetEncoderBufferCount() const { return m_nEncoderBuffer; }";
    assert!(nv_h.contains(acc_anchor), "NvEncoder.h accessor anchor not found");
    fs::write(
        out.join("NvEncoder.h"),
        nv_h.replace(acc_anchor, &format!("{acc_anchor}\n    void* GetSessionHandle() const {{ return m_hEncoder; }}\n    const NV_ENCODE_API_FUNCTION_LIST& GetApi() const {{ return m_nvenc; }}")),
    )
    .unwrap();
    for f in ["NvEncoderD3D11.h", "VideoEncoder.h"] {
        fs::copy(up.join("platform/win32").join(f), out.join(f)).expect("copy encoder header");
    }

    // SDK 12.2 replaced pixelBitDepthMinus8 / inputPixelBitDepthMinus8 with outputBitDepth / inputBitDepth.
    let nv = read_lf(&up.join("platform/win32/NvEncoder.cpp"), "read NvEncoder.cpp");
    let nv = bitdepth_compat(&nv);
    // include the driver's own explanation (nvEncGetLastErrorString) when initialization is rejected
    let init_call = "NVENC_API_CALL(m_nvenc.nvEncInitializeEncoder(m_hEncoder, &m_initializeParams));";
    assert!(nv.contains(init_call), "nvEncInitializeEncoder call not found in NvEncoder.cpp");
    // upstream ignores the result of the preset query; if it fails the whole config stays zeroed. Make it loud.
    let preset_call = "m_nvenc.nvEncGetEncodePresetConfigEx(m_hEncoder, codecGuid, presetGuid, tuningInfo, &presetConfig);";
    assert!(nv.contains(preset_call), "nvEncGetEncodePresetConfigEx call not found in NvEncoder.cpp");
    let nv = nv.replace(
        preset_call,
        "{ NVENCSTATUS st_ = m_nvenc.nvEncGetEncodePresetConfigEx(m_hEncoder, codecGuid, presetGuid, tuningInfo, &presetConfig); if (st_ != NV_ENC_SUCCESS) { const char* le_ = m_nvenc.nvEncGetLastErrorString(m_hEncoder); std::ostringstream o_; o_ << \"nvEncGetEncodePresetConfigEx returned \" << st_ << \": \" << (le_ ? le_ : \"(no driver message)\"); throw NVENCException::makeNVENCException(o_.str(), st_, __FUNCTION__, __FILE__, __LINE__); } }",
    );
    let reconf_call = "NVENC_API_CALL(m_nvenc.nvEncReconfigureEncoder(m_hEncoder, const_cast<NV_ENC_RECONFIGURE_PARAMS*>(pReconfigureParams)));";
    assert!(nv.contains(reconf_call), "nvEncReconfigureEncoder call not found in NvEncoder.cpp");
    let nv = nv.replace(
        reconf_call,
        "{ NVENCSTATUS st_ = m_nvenc.nvEncReconfigureEncoder(m_hEncoder, const_cast<NV_ENC_RECONFIGURE_PARAMS*>(pReconfigureParams)); if (st_ != NV_ENC_SUCCESS) { const char* le_ = m_nvenc.nvEncGetLastErrorString(m_hEncoder); std::ostringstream o_; o_ << \"nvEncReconfigureEncoder returned \" << st_ << \": \" << (le_ ? le_ : \"(no driver message)\"); throw NVENCException::makeNVENCException(o_.str(), st_, __FUNCTION__, __FILE__, __LINE__); } }",
    );
    let nv = nv.replace(
        init_call,
        "{ NVENCSTATUS st_ = m_nvenc.nvEncInitializeEncoder(m_hEncoder, &m_initializeParams); if (st_ != NV_ENC_SUCCESS) { const char* le_ = m_nvenc.nvEncGetLastErrorString(m_hEncoder); std::ostringstream o_; o_ << \"nvEncInitializeEncoder returned \" << st_ << \": \" << (le_ ? le_ : \"(no driver message)\"); throw NVENCException::makeNVENCException(o_.str(), st_, __FUNCTION__, __FILE__, __LINE__); } }",
    );
    let nv_path = out.join("NvEncoder_122.cpp");
    fs::write(&nv_path, nv).unwrap();

    let mut b = cc::Build::new();
    b.cpp(true)
        .flag("/std:c++17")
        .flag("/EHsc")
        .define("NOMINMAX", None)
        .include(&out)
        .include(nvenc.join("shim122")) // NVENC SDK 12.2 header first (needs the compat patches above)
        .include(nvenc.join("shim"))
        .include(&up)
        .include(up.join("platform/win32/d3d-render-utils"))
        .include(up.join("platform/win32"))
        .include(&nvenc)
        .include(nvenc.join("hostlib"))
        .include(nvenc.join("../ovrshim"))
        .include(nvenc.join("../third_party/stb"))
        .file(nvenc.join("hostlib/nvh.cpp"))
        .file(nvenc.join("hostlib/bench.cpp"))
        .file(nvenc.join("shim/shim.cpp"))
        .file(&enc_cpp_path)
        .file(&nv_path)
        .file(&patched_path)
        .file(up.join("platform/win32/shared/d3drender.cpp"))
        .file(&ffr_path)
        .file(&cso_path)
        .file(up.join("platform/win32/d3d-render-utils/RenderPipeline.cpp"))
        .file(up.join("platform/win32/d3d-render-utils/RenderUtils.cpp"))
        .file(up.join("ALVR-common/exception.cpp"))
        .warnings(false);
    b.compile("nvh");
    for l in ["d3d11", "dxgi", "d3dcompiler", "user32", "advapi32"] {
        println!("cargo:rustc-link-lib={l}");
    }
    println!("cargo:rerun-if-changed={}", nvenc.display());
    println!("cargo:rerun-if-changed={}", nvenc.join("../third_party/stb").display());
}

/// Maps the SDK 12.0 bit-depth fields used by ALVR's encoder files to the SDK 12.2 ones (exact-text replacements; the build
/// fails loudly if upstream text changes).
fn bitdepth_compat(src: &str) -> String {
    let mut s = src.to_string();
    let pairs: [(&str, &str); 7] = [
        ("hevcConfig.pixelBitDepthMinus8 =\n            (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT ) ? 2 : 0;",
         "hevcConfig.outputBitDepth = pIntializeParams->encodeConfig->encodeCodecConfig.hevcConfig.inputBitDepth =\n            (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT || m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT ) ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;"),
        ("av1Config.pixelBitDepthMinus8 = (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) ? 2 : 0;",
         "av1Config.outputBitDepth = (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;"),
        ("av1Config.inputPixelBitDepthMinus8 = (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) ? 2 : 0;",
         "av1Config.inputBitDepth = (m_eBufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;"),
        ("hevcConfig.pixelBitDepthMinus8 != 2", "hevcConfig.outputBitDepth != NV_ENC_BIT_DEPTH_10"),
        ("av1Config.pixelBitDepthMinus8 != 2", "av1Config.outputBitDepth != NV_ENC_BIT_DEPTH_10"),
        ("encodeConfig.encodeCodecConfig.hevcConfig.pixelBitDepthMinus8 = 2;",
         "encodeConfig.encodeCodecConfig.hevcConfig.outputBitDepth = encodeConfig.encodeCodecConfig.hevcConfig.inputBitDepth = NV_ENC_BIT_DEPTH_10;"),
        ("config.pixelBitDepthMinus8 = 2;", "config.outputBitDepth = config.inputBitDepth = NV_ENC_BIT_DEPTH_10;"),
    ];
    // SDK 12.2 inserted a `reserved` field after `version` in NV_ENC_PRESET_CONFIG: the old initializer { VER, { CONFIG_VER } }
    // put CONFIG_VER into `reserved`, so the preset query failed (result unchecked upstream) and left the config zeroed.
    s = s.replace("{ NV_ENC_PRESET_CONFIG_VER, { NV_ENC_CONFIG_VER } }", "{ NV_ENC_PRESET_CONFIG_VER, 0, { NV_ENC_CONFIG_VER } }");
    for (old, new) in pairs {
        // each file contains only some of these; a file that mentions the field but matches none is an error
        s = s.replace(old, new);
    }
    assert!(!s.contains("pixelBitDepthMinus8"), "unpatched SDK 12.0 bit-depth field left in source");
    s
}

/// VideoEncoderNVENC::DescribeConfig: what NVENC was actually initialised with, the GPU's capabilities, and whether the documented
/// conditions for split-frame encoding hold (SDK 12.2 header: HEVC/AV1 only; not with weighted prediction, alpha layer, subframe
/// mode, video-memory output, or picture-timing SEI on DX12).
const DESCRIBE_CPP: &str = r#"
std::string VideoEncoderNVENC::DescribeConfig() {
    if (!m_NvNecoder) return "{}";
    NV_ENC_INITIALIZE_PARAMS ip = { NV_ENC_INITIALIZE_PARAMS_VER };
    NV_ENC_CONFIG cfg = { NV_ENC_CONFIG_VER };
    ip.encodeConfig = &cfg;
    m_NvNecoder->GetInitializeParams(&ip);
    const GUID hevc = NV_ENC_CODEC_HEVC_GUID;
    auto cap = [&](NV_ENC_CAPS c) { return m_NvNecoder->GetCapabilityValue(hevc, c); };
    const auto& hv = cfg.encodeCodecConfig.hevcConfig;
    const bool isHevc = memcmp(&ip.encodeGUID, &hevc, sizeof(GUID)) == 0;
    const int engines = cap(NV_ENC_CAPS_NUM_ENCODER_ENGINES);
    const bool conditions = isHevc && !ip.enableWeightedPrediction && !hv.enableAlphaLayerEncoding && !ip.enableSubFrameWrite && !ip.enableOutputInVidmem;
    const uint32_t mode = ip.splitEncodeMode;
    int strips = 1;
    if (!conditions || mode == 15) strips = 1;
    else if (mode == 2) strips = engines >= 2 ? 2 : 1;
    else if (mode == 3) strips = engines >= 3 ? 3 : (engines >= 1 ? engines : 1);
    else strips = -1;   // auto / auto-forced: chosen by the driver
    char b[3600];
    snprintf(b, sizeof b,
        "{\"hevc\":%d,\"size\":[%u,%u],\"fps\":%.1f,\"preset_tuning\":%d,\"rc_mode\":%d,\"avg_bps\":%u,\"max_bps\":%u,\"vbv\":%u,\"aq_spatial\":%d,\"aq_temporal\":%d,\"multi_pass\":%d,"
        "\"qp_map_mode\":%d,\"gop\":%u,\"frame_interval_p\":%d,\"idr_period\":%u,\"bit_depth_out\":%d,\"chroma_idc\":%u,\"cu_min\":%d,\"cu_max\":%d,"
        "\"split\":{\"requested_mode\":%u,\"engines\":%d,\"conditions_met\":%d,\"weighted_pred\":%d,\"alpha\":%d,\"subframe_write\":%d,\"vidmem_output\":%d,\"timing_sei\":%d,\"expected_strips\":%d},"
        "\"caps\":{\"width_max\":%d,\"height_max\":%d,\"width_min\":%d,\"height_min\":%d,\"10bit\":%d,\"dyn_bitrate_change\":%d,\"dyn_res_change\":%d,\"lookahead\":%d,\"temporal_aq\":%d,\"weighted_pred\":%d,\"mb_per_sec_max\":%d,\"multi_ref\":%d,\"recon_output\":%d,\"output_stats\":%d}}",
        isHevc ? 1 : 0, ip.encodeWidth, ip.encodeHeight, ip.frameRateDen ? (double)ip.frameRateNum / ip.frameRateDen : 0.0, (int)ip.tuningInfo,
        (int)cfg.rcParams.rateControlMode, cfg.rcParams.averageBitRate, cfg.rcParams.maxBitRate, cfg.rcParams.vbvBufferSize,
        (int)cfg.rcParams.enableAQ, (int)cfg.rcParams.enableTemporalAQ, (int)cfg.rcParams.multiPass,
        (int)cfg.rcParams.qpMapMode, cfg.gopLength, (int)cfg.frameIntervalP, hv.idrPeriod, (int)hv.outputBitDepth, hv.chromaFormatIDC, (int)hv.minCUSize, (int)hv.maxCUSize,
        mode, engines, conditions ? 1 : 0, (int)ip.enableWeightedPrediction, (int)hv.enableAlphaLayerEncoding, (int)ip.enableSubFrameWrite, (int)ip.enableOutputInVidmem,
        (int)hv.outputPictureTimingSEI, strips,
        cap(NV_ENC_CAPS_WIDTH_MAX), cap(NV_ENC_CAPS_HEIGHT_MAX), cap(NV_ENC_CAPS_WIDTH_MIN), cap(NV_ENC_CAPS_HEIGHT_MIN), cap(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE),
        cap(NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE), cap(NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE), cap(NV_ENC_CAPS_SUPPORT_LOOKAHEAD), cap(NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ),
        cap(NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION), cap(NV_ENC_CAPS_MB_PER_SEC_MAX), cap(NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES),
        cap(NV_ENC_CAPS_OUTPUT_RECON_SURFACE), cap(NV_ENC_CAPS_OUTPUT_BLOCK_STATS));
    return b;
}
"#;

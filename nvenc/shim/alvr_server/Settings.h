#pragma once
// Test shim: same field names/types as ALVR v20.14.1 alvr_server/Settings.h (subset used by VideoEncoderNVENC).
#include <cstdint>
#include <string>

enum ALVR_CODEC { ALVR_CODEC_H264 = 0, ALVR_CODEC_HEVC = 1, ALVR_CODEC_AV1 = 2 };
enum ALVR_RATE_CONTROL_MODE { ALVR_CBR = 0, ALVR_VBR = 1 };
enum ALVR_ENTROPY_CODING { ALVR_CABAC = 0, ALVR_CAVLC = 1 };

class Settings {
    Settings();
public:
    static Settings& Instance() { static Settings s; return s; }
    int m_refreshRate = 90;
    int m_codec = ALVR_CODEC_HEVC;
    bool m_use10bitEncoder = true;
    bool m_useFullRangeEncoding = true;
    bool m_enableHdr = false;
    uint32_t m_renderWidth = 0;      // eye_resolution_width * 2 (side-by-side)
    uint32_t m_renderHeight = 0;
    bool m_enableFoveatedEncoding = false;
    float m_foveationCenterSizeX = 0.45f, m_foveationCenterSizeY = 0.4f;
    float m_foveationCenterShiftX = 0.4f, m_foveationCenterShiftY = 0.1f;
    float m_foveationEdgeRatioX = 4.f, m_foveationEdgeRatioY = 5.f;
    uint32_t m_nvencQualityPreset = 3;       // P3
    uint32_t m_rateControlMode = ALVR_CBR;
    bool m_fillerData = false;
    uint32_t m_entropyCoding = ALVR_CABAC;
    uint32_t m_nvencTuningPreset = 2;        // NV_ENC_TUNING_INFO_LOW_LATENCY
    uint32_t m_nvencMultiPass = 0;           // disabled
    uint32_t m_nvencAdaptiveQuantizationMode = 1; // SpatialAQ
    int64_t m_nvencLowDelayKeyFrameScale = -1;
    int64_t m_nvencRefreshRate = -1;
    bool m_nvencEnableIntraRefresh = false;
    int64_t m_nvencIntraRefreshPeriod = -1;
    int64_t m_nvencIntraRefreshCount = -1;
    int64_t m_nvencMaxNumRefFrames = -1;
    int64_t m_nvencGopLength = -1;
    int64_t m_nvencPFrameStrategy = -1;
    int64_t m_nvencRateControlMode = -1;
    int64_t m_nvencRcBufferSize = -1;
    int64_t m_nvencRcInitialDelay = -1;
    int64_t m_nvencRcMaxBitrate = -1;
    int64_t m_nvencRcAverageBitrate = -1;
    bool m_nvencEnableWeightedPrediction = false;
    // QP delta map (our addition): per-CTB QP offsets, lower QP (better quality) in the foveal region, higher in the periphery
    bool m_qpMapEnabled = false;
    int m_qpMapCenterDelta = 0, m_qpMapEdgeDelta = 0;
    float m_qpMapTransition = 1.0f;           // width of the center->edge ramp, in units of the center region's half size
    uint32_t m_nvencSplitEncodeMode = 0;      // NV_ENC_SPLIT_ENCODE_MODE: 0 auto, 1 auto-forced, 2 two, 3 three, 15 disabled
};

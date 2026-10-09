#pragma once
// Test shim for the Rust FFI surface used by the encoder.
struct FfiDynamicEncoderParams { unsigned int updated; unsigned long long bitrate_bps; float framerate; };
extern FfiDynamicEncoderParams (*GetDynamicEncoderParams)();
void ParseFrameNals(int codec, unsigned char* buf, int len, unsigned long long targetTimestampNs, bool isIdr);

// Shader bytecode normally provided by the Rust side (include_bytes!); here generated from the upstream .cso files.
extern "C" const unsigned char* QUAD_SHADER_CSO_PTR;
extern "C" unsigned int QUAD_SHADER_CSO_LEN;
extern "C" const unsigned char* COMPRESS_AXIS_ALIGNED_CSO_PTR;
extern "C" unsigned int COMPRESS_AXIS_ALIGNED_CSO_LEN;

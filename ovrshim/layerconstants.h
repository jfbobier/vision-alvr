// Constants for the quad / cylinder / cube / final-encode passes (LayerVS.hlsl, LayerPS.hlsl, LayerCubePS.hlsl).
// Shared between HLSL and C++ like constantsbuffer.h. Matrices are stored transposed (see driver.cpp) so HLSL can mul(v, M).
#ifndef LAYERCONSTANTS_H_
#define LAYERCONSTANTS_H_

#ifndef HLSL
typedef DirectX::XMFLOAT2 float2;
typedef DirectX::XMFLOAT4X4 float4x4;
#define ALIGNED(N) __declspec(align(N))
#else
#define ALIGNED(N)
#endif

#define LAYER_KIND_QUAD 0
#define LAYER_KIND_CYLINDER 1
#define LAYER_KIND_CUBE 2
#define LAYER_KIND_FULLSCREEN 3

ALIGNED(16) struct LayerConstants {
    float4x4 worldViewProj; // quad/cylinder: model * view * projection
    float4x4 invProj;       // cube / fullscreen: clip -> view
    float4x4 viewToCube;    // cube: view direction -> cube sampling direction (eye rotation, inverse cube orientation, handedness flip)
    float2 uvOffset;        // normalized sub-rect of the swapchain
    float2 uvExtent;
    float2 size;            // quad size in meters
    float radius;           // cylinder
    float angle;
    float aspect;
    int kind;
    int flipY;              // texture origin at bottom left
    int encodeSrgb;         // write sRGB-encoded values (target is UNORM, source samples as linear)
    int decodeSrgb;         // source holds sRGB-encoded values in a UNORM format: convert to linear (linear compose target)
    int premultiply;        // straight-alpha source: multiply rgb by alpha (layer draws); 0 for the final encode pass
    int segments;           // cylinder tessellation
    float outputGamma;      // exponent applied to the final encoded value (1 / ALVR encoding gamma); 1 (or <= 0) = none
    float brightness, contrast, saturation, sharpening; // user colour correction (0 = neutral), only where finalPass is set
    int finalPass;          // 1: this draw writes the value that is encoded
};

#endif

// Pixel shader for cube layers (skybox at infinity, opaque).
#include "constantsbuffer.h"
#include "layerconstants.h"
#include "utils.hlsli"

cbuffer cb : register(b0)
{
    LayerConstants cb;
};

SamplerState linearSampler : register(s0);
TextureCube<float4> cubeColor : register(t0);

float4 main(in float4 position : SV_Position, in float3 uvw : TEXCOORD0) : SV_Target
{
    float4 color = cubeColor.SampleLevel(linearSampler, normalize(uvw), 0);
    if (cb.decodeSrgb)
    {
        float3 lo = color.rgb / 12.92;
        float3 hi = pow(max((color.rgb + 0.055) / 1.055, 0.0), 2.4);
        color.rgb = lerp(hi, lo, color.rgb <= 0.04045);
    }
    if (cb.encodeSrgb)
    {
        float3 lo = color.rgb * 12.92;
        float3 hi = 1.055 * pow(max(color.rgb, 0.0), 1.0 / 2.4) - 0.055;
        color.rgb = lerp(hi, lo, color.rgb <= 0.0031308);
    }
    if (cb.finalPass)
    {
        color.rgb = ColorCorrect(color.rgb, cb.brightness, cb.contrast, cb.saturation);
    }
    // ALVR encoding gamma (see LayerPS.hlsl)
    if (cb.outputGamma > 0.0 && cb.outputGamma != 1.0)
    {
        color.rgb = pow(max(color.rgb, 0.0), cb.outputGamma);
    }
    return float4(color.rgb, 1.0);
}

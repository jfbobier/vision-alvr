// Pixel shader for quad / cylinder layers and the final linear -> sRGB encode pass.
#include "constantsbuffer.h"
#include "layerconstants.h"
#include "utils.hlsli"

cbuffer cb : register(b0)
{
    LayerConstants cb;
};

SamplerState linearSampler : register(s0);
Texture2D<float4> sourceColor : register(t0);

float4 main(in float4 position : SV_Position, in float3 uvw : TEXCOORD0) : SV_Target
{
    Rect rect;
    rect.offset = cb.uvOffset;
    rect.extent = cb.uvExtent;
    float2 uv = ApplyImageRect(uvw.xy, rect, cb.flipY != 0);

    float4 color = sourceColor.SampleLevel(linearSampler, uv, 0);
    if (cb.finalPass && cb.sharpening > 0.0)
    {
        float w, h;
        sourceColor.GetDimensions(w, h);
        const float2 dx = float2(1.0 / w, 0), dy = float2(0, 1.0 / h);
        const float3 n = sourceColor.SampleLevel(linearSampler, uv + dx, 0).rgb + sourceColor.SampleLevel(linearSampler, uv - dx, 0).rgb +
                         sourceColor.SampleLevel(linearSampler, uv + dy, 0).rgb + sourceColor.SampleLevel(linearSampler, uv - dy, 0).rgb;
        color.rgb = max(color.rgb * (1.0 + cb.sharpening) - n * (cb.sharpening / 4.0), 0.0);
    }

    if (cb.decodeSrgb)
    {
        float3 lo = color.rgb / 12.92;
        float3 hi = pow(max((color.rgb + 0.055) / 1.055, 0.0), 2.4);
        color.rgb = lerp(hi, lo, color.rgb <= 0.04045);
    }
    if (cb.premultiply) // unused: layers arrive premultiplied from VDXR
    {
        color = PreMultiplyAlpha(color);
    }
    if (cb.encodeSrgb)
    {
        float3 lo = color.rgb * 12.92;
        float3 hi = 1.055 * pow(max(color.rgb, 0.0), 1.0 / 2.4) - 0.055;
        color.rgb = lerp(hi, lo, color.rgb <= 0.0031308);
        color.a = 1.0;
    }
    if (cb.finalPass)
    {
        color.rgb = ColorCorrect(color.rgb, cb.brightness, cb.contrast, cb.saturation);
    }
    // ALVR encoding gamma: the client decodes with pow(v, gamma), so the value it must reconstruct is sent as pow(v, 1 / gamma)
    if (cb.outputGamma > 0.0 && cb.outputGamma != 1.0)
    {
        color.rgb = pow(max(color.rgb, 0.0), cb.outputGamma);
    }
    return color;
}

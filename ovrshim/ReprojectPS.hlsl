// MIT License
//
// Copyright(c) 2025-2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "constantsbuffer.h"
#include "utils.hlsli"

cbuffer cb : register(b0)
{
    ConstantsBuffer cb;
};

SamplerState linearSampler : register(s0);

Texture2D<float4> sourceColor : register(t0);

float4 main(in float4 position : SV_Position, in float3 reprojectedCoord : TEXCOORD0) : SV_Target
{
    float2 reprojectedNdc = reprojectedCoord.xy / reprojectedCoord.z;
    if (any(abs(reprojectedNdc) > 1.0))
    {
        discard;
    }

    float2 texCoord = NdcToTexCoord(reprojectedNdc);
    float2 realTexCoord = ApplyImageRect(texCoord, cb.imageRectNormalized, cb.flipY);

    float4 color = sourceColor.SampleLevel(linearSampler, realTexCoord, 0);
    if (cb.finalPass && cb.sharpening > 0.0)
    {
        // unsharp mask on the 4 neighbours (ALVR uses 8 at weight sharpening / 8)
        float w, h;
        sourceColor.GetDimensions(w, h);
        const float2 dx = float2(1.0 / w, 0), dy = float2(0, 1.0 / h);
        const float3 n = sourceColor.SampleLevel(linearSampler, realTexCoord + dx, 0).rgb + sourceColor.SampleLevel(linearSampler, realTexCoord - dx, 0).rgb +
                         sourceColor.SampleLevel(linearSampler, realTexCoord + dy, 0).rgb + sourceColor.SampleLevel(linearSampler, realTexCoord - dy, 0).rgb;
        color.rgb = max(color.rgb * (1.0 + cb.sharpening) - n * (cb.sharpening / 4.0), 0.0);
    }

    if (cb.decodeSrgb)
    {
        float3 lo2 = color.rgb / 12.92;
        float3 hi2 = pow(max((color.rgb + 0.055) / 1.055, 0.0), 2.4);
        color.rgb = lerp(hi2, lo2, color.rgb <= 0.04045);
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
    // ALVR encoding gamma: the client decodes with pow(v, gamma), so the value it must reconstruct is sent as pow(v, 1 / gamma)
    if (cb.outputGamma > 0.0 && cb.outputGamma != 1.0)
    {
        color.rgb = pow(max(color.rgb, 0.0), cb.outputGamma);
    }

    // Premultiplied already: VDXR pre-processes the alpha of every layer above 0 for OVR.
    return color;
}

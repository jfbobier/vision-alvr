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

#ifndef CONSTANTSBUFFER_H_
#define CONSTANTSBUFFER_H_

#ifndef HLSL
typedef DirectX::XMFLOAT2 float2;
typedef DirectX::XMFLOAT4X4 float4x4;
#define ALIGNED(N) __declspec(align(N))
#else
#define ALIGNED(N)
#endif

ALIGNED(16) struct Rect {
    float2 offset;
    float2 extent;
};

ALIGNED(16) struct ConstantsBuffer {
    float4x4 reprojectionMatrix;
    Rect imageRectNormalized;
    bool flipY;
    int decodeSrgb; // 1: the UNORM source holds sRGB-encoded values and the target is linear: convert to linear first
    int encodeSrgb; // 1: the source SRV is sRGB-decoded to linear and the target is UNORM, so encode back to sRGB
    float outputGamma; // exponent applied to the final encoded value (1 / ALVR encoding gamma); 1 (or <= 0) = none
    float brightness, contrast, saturation, sharpening; // user colour correction (0 = neutral), only where finalPass is set
    int finalPass;     // 1: this draw writes the value that is encoded (colour correction + output gamma apply)
};

#ifdef HLSL
// The user's colour correction on encoded (sRGB) values, like ALVR's ColorCorrectionPixelShader: brightness offset, contrast around
// mid-grey, saturation around Rec.601 luma (a true desaturation here: ALVR's "lighten only" blend cannot reduce saturation).
float3 ColorCorrect(float3 c, float brightness, float contrast, float saturation)
{
    c += brightness;
    c = (c - 0.5) * (contrast + 1.0) + 0.5;
    const float luma = dot(c, float3(0.299, 0.587, 0.114));
    c = lerp(luma.xxx, c, saturation + 1.0);
    return saturate(c);
}
#endif

#endif

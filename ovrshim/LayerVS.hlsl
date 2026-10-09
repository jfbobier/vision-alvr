// Vertex shader for quad / cylinder / cube / fullscreen passes.
#include "layerconstants.h"

cbuffer cb : register(b0)
{
    LayerConstants cb;
};

float4 main(in uint id : SV_VertexID, out float3 uvw : TEXCOORD0) : SV_Position
{
    float3 local = float3(0, 0, 0);
    float2 uv = float2(0, 0);

    if (cb.kind == LAYER_KIND_QUAD)
    {
        // triangle strip: 0 = top-left, 1 = top-right, 2 = bottom-left, 3 = bottom-right
        float2 c = float2((id & 1) ? 0.5 : -0.5, (id & 2) ? -0.5 : 0.5);
        local = float3(c * cb.size, 0);
        uv = float2(c.x + 0.5, 0.5 - c.y);
    }
    else if (cb.kind == LAYER_KIND_CYLINDER)
    {
        // triangle strip over the interior surface: U runs along +X, V=0 is the top; arc centered on -Z
        uint i = id >> 1;
        bool top = (id & 1) == 0;
        float t = (float)i / (float)cb.segments;
        float theta = (t - 0.5) * cb.angle;
        float h = cb.radius * cb.angle / cb.aspect;
        local = float3(cb.radius * sin(theta), top ? 0.5 * h : -0.5 * h, -cb.radius * cos(theta));
        uv = float2(t, top ? 0.0 : 1.0);
    }
    else
    {
        // fullscreen triangle (cube / final encode)
        float2 p = float2((id == 1) ? 2.0 : 0.0, (id == 2) ? 2.0 : 0.0);
        float4 ndc = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
        if (cb.kind == LAYER_KIND_CUBE)
        {
            float4 v = mul(float4(ndc.xy, 0.5, 1.0), cb.invProj);
            float3 viewDir = v.xyz / v.w;
            uvw = mul(float4(viewDir, 0.0), cb.viewToCube).xyz;
        }
        else
        {
            uvw = float3(p, 0.0); // 0..2 over the oversized triangle = 0..1 over the screen
        }
        return ndc;
    }

    uvw = float3(uv, 0);
    return mul(float4(local, 1.0), cb.worldViewProj);
}

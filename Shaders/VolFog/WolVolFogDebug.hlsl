// Not in the original client (World of Legends): debug pixel shader of the native volumetric fog (wolVolFogDebug),
// brief B2. Written for this project, GPL-3.0-only like the rest of Shaders/VolFog (see LICENSE and
// NOTICE-modificaciones.txt). The register contract is the one of vf_common.hlsli (coa-vfog 205ade5).
// Compiled by fxc as ps_3_0, one permutation per mode: /D DEBUG_MODE=1|2|3|4 (WolVolFogDebug.bls permutation = mode - 1).
//   1: depth buffer linearised in grey, sky white (reads sDepth, the INTZ depth of brief B3; c3.z = far plane here)
//   2: semi-transparent magenta tint over the whole world view
//   3: test grid every 64 px (green) with a red border on the world viewport
//   4: (brief B9, wolVolFogDebug 6) the light shaft texture (mask after the two radial blurs) over the world viewport, scaled by c4.x
//      (0 when the frame has no shafts); reads sRays (s1)
#ifndef DEBUG_MODE
#error DEBUG_MODE must be 1, 2, 3 or 4
#endif

float4 cWorldViewportPx : register(c0); // xy origin, zw size of the world viewport, in pixels
float4 cPixelGrid : register(c1);       // zw = size of one texel of the depth texture
float4 cDepthToViewZ : register(c3);    // x depth at infinite z, y depth per 1/z, z max distance, w deepest world depth
float4 cDebugGain : register(c4);       // x = gain of the light shaft view (mode 4)
sampler2D sDepth : register(s0);
sampler2D sRays : register(s1);

static const float kSkyMinDepth = 0.95;

float4 main(float2 pixel : VPOS) : COLOR0
{
#if DEBUG_MODE == 1
    float depth = tex2Dlod(sDepth, float4((pixel + 0.5) * cPixelGrid.zw, 0, 0)).r;
    if (depth >= kSkyMinDepth)
    {
        return float4(1, 1, 1, 1);
    }
    // depth = x + y / viewZ with y < 0, so depth - x is negative for everything in front of infinity (LinearDepth of
    // vf_common.hlsli divides the same way); the beyond-far-clip test is the one of BeyondFarClip().
    float denominator = depth - cDepthToViewZ.x;
    float dist = (depth > cDepthToViewZ.w || denominator > -1e-6) ? cDepthToViewZ.z : min(cDepthToViewZ.y / denominator, cDepthToViewZ.z);
    float grey = sqrt(saturate(dist / cDepthToViewZ.z));
    return float4(grey, grey, grey, 1);
#elif DEBUG_MODE == 2
    return float4(1, 0, 1, 0.35);
#elif DEBUG_MODE == 3
    float2 local = pixel - cWorldViewportPx.xy;
    float2 cell = fmod(local, 64.0);
    float grid = (cell.x < 1.0 || cell.y < 1.0) ? 1.0 : 0.0;
    float border = (any(local < 2.0) || any(local > cWorldViewportPx.zw - 3.0)) ? 1.0 : 0.0;
    float3 colour = lerp(float3(0, 1, 0), float3(1, 0, 0), border);
    return float4(colour, saturate(grid + border) * 0.85);
#elif DEBUG_MODE == 4
    float2 uv = (pixel + 0.5 - cWorldViewportPx.xy) / cWorldViewportPx.zw;
    return float4(tex2Dlod(sRays, float4(uv, 0, 0)).rgb * cDebugGain.x, 1);
#else
#error DEBUG_MODE must be 1, 2, 3 or 4
#endif
}

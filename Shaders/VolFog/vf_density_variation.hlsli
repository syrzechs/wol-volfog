float4 cDensityVariation : register(c78);
// Modified for World of Legends (2026-10-20, brief B6): the Gx layer has no volume textures, so the 32^3 noise is a 2D atlas of 32
// slices (z) of 32x32 texels stacked along v (32 x 1024, LINEAR, WRAP in u). Was: sampler3D sDensityNoise : register(s9).
sampler2D sDensityNoise : register(s9);

static const float kNoiseSize = 32;

// Same result as tex3Dlod(volume, uv) with WRAP and trilinear filtering on the 32^3 volume: the two slices that bracket z are
// sampled with the bilinear filter of the atlas and blended. u wraps in hardware; v is kept inside its slice (half a texel of
// each edge), so a row of one slice never blends with the next slice (the y wrap becomes a hard step, once every 32 texels).
float SampleNoiseVolume(float3 uv)
{
    float3 texel = uv * kNoiseSize - 0.5;
    float lowerSlice = floor(texel.z);
    float blend = texel.z - lowerSlice;
    float2 slices = floor(frac((lowerSlice + float2(0, 1)) / kNoiseSize) * kNoiseSize + 0.5);
    float inSlice = clamp(frac(uv.y) * kNoiseSize, 0.5, kNoiseSize - 0.5);
    float2 v = (slices * kNoiseSize + inSlice) / (kNoiseSize * kNoiseSize);
    float lower = tex2Dlod(sDensityNoise, float4(uv.x, v.x, 0, 0)).r;
    float upper = tex2Dlod(sDensityNoise, float4(uv.x, v.y, 0, 0)).r;
    return lerp(lower, upper, blend);
}

float DensityVariation(float3 worldPosition)
{
    [branch] if (cDensityVariation.x <= 0)
        return 1;
    float3 uv = (worldPosition - float3(cDensityVariation.zw, 0)) * (cDensityVariation.y / 32) + 0.5 / 32;
    float noise = SampleNoiseVolume(uv) * (4.0 / 3.0);
    uv = uv * 2 + float3(36.5, 16.5, 52.5) / 32;
    float centered = noise + SampleNoiseVolume(uv) * (2.0 / 3.0) - 1;
    return 1 + saturate(cDensityVariation.x) * centered;
}

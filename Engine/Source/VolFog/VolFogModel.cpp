#include "Engine/Source/VolFog/VolFogModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "Engine/Source/VolFog/VolFog.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B5.
// Licensed under GPL-3.0-only. Derived from src/fog_model.cpp of coa-vfog 205ade5 (copyright of the authors of CoAVolFog,
// jealous-sound and the contributors of the Raz0r1337 fork; see Shaders/VolFog/NOTICE-modificaciones.txt).
// Modified 2026-10-20 (brief B5): in the namespace VolFog; only the derived mode is kept (DerivedLayers, DistanceLayer,
// ApplyInteriorFog and the matrix helpers; the authored/Classic layers, noise and direct-light matching are gone); Config is
// replaced by ModelTuning; FogParams gains `active` (false in liquid).
// Modified 2026-10-20 (brief B10): the moon fades in as the sun sets (moonFade).

namespace {
    constexpr float kHazeDensity = 0.00035f;
    constexpr float kHazeG = 0.75f;
    constexpr float kHazeIsotropic = 0.3f;
    constexpr float kHazeFalloff = 1.0f / 220.0f;
    constexpr float kHazeSun = 1.3f;
    constexpr float kHazeAmbient = 0.8f;

    constexpr float kGroundStart = 12.0f;
    constexpr float kGroundOpticalDepth = 1.1f;
    constexpr float kGroundG = 0.4f;
    constexpr float kGroundIsotropic = 0.5f;
    constexpr float kGroundFalloff = 1.0f / 18.0f;
    constexpr float kGroundSun = 0.8f;
    constexpr float kGroundAmbient = 0.75f;

    constexpr float kFarOpticalDepth = 3.0f;
    constexpr float kFarRamp = 60.0f;
    constexpr float kFarStartFraction = 0.25f;
    constexpr float kFarG = 0.2f;
    constexpr float kFarIsotropic = 0.7f;
    constexpr float kFarSun = 0.45f;
    constexpr float kFarAmbient = 0.6f;
    constexpr float kFarSkyFalloff = 10.0f;
    constexpr float kFarExponent = 2.0f;

    constexpr float kDerivedLayerLimit = 1500.0f;

    constexpr float kLightSettingHalfWidth = 0.02f;
    constexpr float kReferenceFarTarget = 200.0f;
    constexpr float kNoLimit = 1.0e9f;

    float SmoothStep(float e0, float e1, float x) {
        float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);

        return t * t * (3.0f - 2.0f * t);
    }

    void Scale(const float* rgb, float s, float* out) {
        out[0] = rgb[0] * s;
        out[1] = rgb[1] * s;
        out[2] = rgb[2] * s;
    }

    void Encode(float* rgb, bool linear) {
        if (linear) {
            for (int i = 0; i < 3; ++i) {
                rgb[i] = std::pow(std::max(rgb[i], 0.0f), 2.2f);
            }
        }
    }

    float ReferenceZ(const VolFog::FrameInputs& in) {
        float dx = in.camTarget[0] - in.camPos[0];
        float dy = in.camTarget[1] - in.camPos[1];
        float dz = in.camTarget[2] - in.camPos[2];

        if (dx * dx + dy * dy + dz * dz > kReferenceFarTarget * kReferenceFarTarget) {
            return in.camPos[2] - 2.0f;
        }

        return std::min(in.camPos[2], in.camTarget[2]) - 1.0f;
    }

    void Unshadowed(VolFog::FogLayer& l) {
        std::memcpy(l.shadowEmissive, l.emissive, sizeof(l.emissive));
        l.shadowDensity = 1.0f;
        l.shadowed = 0.0f;
    }

    void Unbounded(VolFog::FogLayer& l) {
        l.skyFalloff = 0.0f;
        l.endDistance = kNoLimit;
        l.densityVariation = 1.0f;
    }

    void DerivedLayers(const VolFog::ModelTuning& cfg, const VolFog::FogParams& p, float sunScatter, float fogDistance, const float* fogColor, VolFog::FogLayer* out) {
        float foggyZone = std::clamp(700.0f / fogDistance, 0.6f, 2.5f);

        VolFog::FogLayer& haze = out[0];
        haze = VolFog::FogLayer{};
        haze.start = 0.0f;
        haze.density = kHazeDensity * cfg.haze * cfg.density * foggyZone;
        haze.g = kHazeG;
        haze.isotropic = kHazeIsotropic;
        Scale(fogColor, kHazeAmbient * cfg.ambient, haze.emissive);
        Scale(p.lightColor, kHazeSun * sunScatter, haze.diffuse);
        haze.exponent = 1.0f;
        haze.upperHeight = p.referenceZ;
        haze.upperFalloff = kHazeFalloff;
        Unshadowed(haze);
        Unbounded(haze);
        haze.endDistance = kDerivedLayerLimit;

        VolFog::FogLayer& ground = out[1];
        ground = VolFog::FogLayer{};
        ground.start = kGroundStart;
        ground.density = kGroundOpticalDepth * cfg.groundFog * cfg.density / std::max(fogDistance, 250.0f);
        ground.g = kGroundG;
        ground.isotropic = kGroundIsotropic;
        Scale(fogColor, kGroundAmbient * cfg.ambient, ground.emissive);
        Scale(p.lightColor, kGroundSun * sunScatter, ground.diffuse);
        ground.exponent = 1.0f;
        ground.upperHeight = p.referenceZ;
        ground.upperFalloff = kGroundFalloff;
        Unshadowed(ground);
        Unbounded(ground);
        ground.endDistance = kDerivedLayerLimit;

        out[2] = VolFog::FogLayer{};
        Unbounded(out[2]);
    }

    float DistanceFogOpticalDepthPerDensity(float farStart, float farLimit, float range) {
        const float spanFraction = (farLimit - farStart) / range;

        return range * (spanFraction + kFarRamp * spanFraction * spanFraction * spanFraction / 3.0f);
    }

    void DistanceLayer(const VolFog::FrameInputs& in, const VolFog::ModelTuning& cfg, const VolFog::FogParams& p, float sunScatter, const float* fogColor, VolFog::FogLayer& out) {
        out = VolFog::FogLayer{};
        float farStart = std::min(std::max(in.fogStart, 0.0f), p.farLimit * kFarStartFraction);
        float opticalDepthPerDensity = DistanceFogOpticalDepthPerDensity(farStart, p.farLimit, p.maxDistance);
        out.start = farStart;
        out.density = cfg.stockFog && opticalDepthPerDensity > 0.0f ? kFarOpticalDepth * cfg.farFog * cfg.density / opticalDepthPerDensity : 0.0f;
        out.g = kFarG;
        out.isotropic = kFarIsotropic;
        out.strength = kFarRamp;
        out.exponent = kFarExponent;
        out.skyFalloff = kFarSkyFalloff;
        out.endDistance = p.farLimit;
        Scale(fogColor, kFarAmbient * cfg.ambient, out.emissive);
        Scale(p.lightColor, kFarSun * sunScatter, out.diffuse);
        Unshadowed(out);
    }

    float InteriorWeight(const VolFog::FrameInputs& in, const VolFog::ModelTuning& cfg) {
        return cfg.interiorAware && in.localLights.cameraInterior && std::isfinite(in.localLights.interiorBlend) ? std::clamp(in.localLights.interiorBlend, 0.0f, 1.0f) : 0.0f;
    }

    void ApplyInteriorFog(float weight, const VolFog::ModelTuning& cfg, VolFog::FogParams& fog) {
        const float retainedDensity = std::isfinite(cfg.interiorDensity) ? std::clamp(cfg.interiorDensity, 0.0f, 1.0f) : 0.15f;
        const float densityScale = 1.0f + (retainedDensity - 1.0f) * weight;

        for (int i = 0; i < VolFog::kSceneLayers; ++i) {
            fog.layers[i].density *= densityScale;
        }

        const float retainedFarFog = std::isfinite(cfg.interiorFarFog) ? std::clamp(cfg.interiorFarFog, 0.0f, 1.0f) : 1.0f;
        fog.layers[VolFog::kDistanceFogLayer].density *= 1.0f + (retainedFarFog - 1.0f) * weight;

        for (VolFog::FogLayer& layer : fog.layers) {
            Scale(layer.diffuse, 1.0f - weight, layer.diffuse);
        }

        fog.lightVisibility *= 1.0f - weight;
    }
}

VolFog::ModelTuning VolFog::CurrentTuning() {
    ModelTuning tuning;
    tuning.density = VolFog::Settings().density;

    return tuning;
}

VolFog::FogParams VolFog::BuildFogParams(const FrameInputs& in, const ModelTuning& cfg) {
    FogParams p = {};
    p.linear = cfg.linear;
    p.active = !in.inLiquid;
    const float interiorWeight = InteriorWeight(in, cfg);
    float fogColor[3];
    VolFog::UnpackColor(in.fogColor, fogColor);
    VolFog::UnpackColor(in.directColor, p.lightColor);
    VolFog::UnpackColor(in.lightIsMoon ? in.directColor : in.sunColor, p.rayColor);
    Encode(fogColor, p.linear);
    Encode(p.lightColor, p.linear);

    float elevation = SmoothStep(-0.03f, 0.10f, in.toLight[2]);
    const float moonFade = in.lightIsMoon ? 1.0f - SmoothStep(kMoonFadeEnd, kSunLightFloor, in.sunElevation) : 1.0f; // brief B10
    p.lightVisibility = elevation * (in.lightIsMoon ? kMoonLightScale * moonFade : 1.0f);
    p.lightAboveHorizon = SmoothStep(-kLightSettingHalfWidth, kLightSettingHalfWidth, in.toLight[2]);
    p.farClip = in.farClip;
    p.maxDistance = std::max(cfg.maxDistance, in.farClip);
    p.horizonStart = in.farClip * 0.85f;
    p.referenceZ = ReferenceZ(in);
    p.farLimit = std::clamp(in.fogEnd, 100.0f, in.farClip);

    float fogDistance = in.zoneFogDistance > 50.0f && in.zoneFogDistance < 20000.0f ? in.zoneFogDistance : in.fogEnd;
    fogDistance = std::clamp(fogDistance, 50.0f, 5000.0f);
    float elevationFadedScatter = p.lightVisibility * cfg.sunScatter;

    FogLayer& distanceFog = p.layers[kDistanceFogLayer];
    DerivedLayers(cfg, p, elevationFadedScatter, fogDistance, fogColor, p.layers);
    DistanceLayer(in, cfg, p, elevationFadedScatter, fogColor, distanceFog);
    ApplyInteriorFog(interiorWeight, cfg, p);

    return p;
}

void VolFog::Mul4x4(const float* a, const float* b, float* out) {
    float r[16];

    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
        }
    }

    std::memcpy(out, r, sizeof(r));
}

bool VolFog::Invert4x4(const float* m, float* out) {
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (std::fabs(det) < 1e-12) {
        return false;
    }

    for (int i = 0; i < 16; ++i) {
        out[i] = static_cast<float>(inv[i] / det);
    }

    return true;
}

void VolFog::TransformDirection(const float* v, const float* m, float* out) {
    float r[3];

    for (int j = 0; j < 3; ++j) {
        r[j] = v[0] * m[0 * 4 + j] + v[1] * m[1 * 4 + j] + v[2] * m[2 * 4 + j];
    }

    std::memcpy(out, r, sizeof(r));
}

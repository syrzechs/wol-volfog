#ifndef ENGINE_SOURCE_VOLFOG_VOLFOGMODEL_H
#define ENGINE_SOURCE_VOLFOG_VOLFOGMODEL_H

#include <cstdint>
#include "Engine/Source/VolFog/VolFogInputs.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B5.
// Licensed under GPL-3.0-only. Derived from src/fog_model.h of coa-vfog 205ade5 (copyright of the authors of CoAVolFog,
// jealous-sound and the contributors of the Raz0r1337 fork; see Shaders/VolFog/NOTICE-modificaciones.txt).
// Modified 2026-10-20 (brief B5): in the namespace VolFog; only the derived mode is kept (AuthoredLayers, AuthoredLayerNoise,
// ClassicFogThinness, HaloHue, ClientToClassicDirectLight, AuthoredNoiseScroll, WithMeanNoise and the LayerNoise structures are
// gone); Config is replaced by ModelTuning (the density comes from the wolVolFogDensity CVar through VolFog::Settings());
// FogParams gains `active`.

namespace VolFog {
    struct FogLayer {
        float start;
        float density;
        float g;
        float isotropic;
        float emissive[3];
        float strength;
        float diffuse[3];
        float exponent;
        float upperHeight;
        float upperFalloff;
        float lowerHeight;
        float lowerFalloff;
        float shadowEmissive[3];
        float shadowDensity;
        float shadowed;
        float skyFalloff;
        float endDistance;
        float densityVariation;
    };

    static_assert(sizeof(FogLayer) == 6 * sizeof(float[4]), "FogLayer uploads as a march layer's six float4 registers");

    constexpr float kMoonLightScale = 0.35f;

    // Brief B10 (REVISION-B7-B8, finding 4): the sun lights the fog down to this elevation (z of the unit vector to it), where its visibility
    // SmoothStep(-0.03, 0.10, z) reaches 0; below it the moon is the light, and its visibility is multiplied by 1 - SmoothStep(kMoonFadeEnd,
    // kSunLightFloor, sun z), so it comes in over the next 0.12 of elevation of the sun and both sides of the change are 0.
    constexpr float kSunLightFloor = -0.03f;
    constexpr float kMoonFadeEnd = -0.15f;

    constexpr int kFogLayers = 4;
    constexpr int kSceneLayers = 3;
    constexpr int kDistanceFogLayer = kSceneLayers;
    static_assert(kDistanceFogLayer + 1 == kFogLayers, "the distance fog follows the scene layers");

    // Layer 0 = haze, 1 = low fog, 2 = unused in the derived mode (all zero), 3 = distance fog.
    struct FogParams {
        FogLayer layers[kFogLayers];
        float lightColor[3];
        float rayColor[3];
        float lightVisibility;
        float lightAboveHorizon;
        float maxDistance;
        float horizonStart;
        float farClip;
        float referenceZ;
        float farLimit;
        bool linear;
        bool active;    // false while the camera is in liquid: the pass draws nothing and the client keeps its own (underwater) fog
    };

    // What coa-vfog reads from its Config for the model, minus the Classic and authored members. Only `density` follows a CVar
    // (wolVolFogDensity); the rest are the values the model was tuned with (Config defaults of 205ade5).
    struct ModelTuning {
        float density = 1.0f;
        float haze = 1.0f;
        float groundFog = 0.6f;
        float farFog = 1.0f;
        float sunScatter = 1.0f;
        float ambient = 1.0f;
        float maxDistance = 5000.0f;
        bool stockFog = true;       // Config::stockFog == 1: the distance layer stands in for the client's linear fog
        bool linear = true;         // Config::colorSpace == 1: scatter in linear light (colours raised to 2.2)
        bool interiorAware = true;
        float interiorDensity = 0.15f;
        float interiorFarFog = 1.0f;
    };

    // The tuning with the live wolVolFogDensity.
    ModelTuning CurrentTuning();

    // Brief B9: the light shafts of one frame (GodRayFrame and the sun block of Renderer::RenderPasses in renderer.cpp of coa-vfog 205ade5). The body is
    // FrameInputs::toRaySource (the visible sun, or the moon); its position on screen comes from the view and projection of the frame
    // (VolFogRays.cpp), not from GxuXformCalc3dScreenCoords. Shared by the pass and by wolVolFogDump.
    constexpr float kMinGodRayStrength = 0.005f;    // renderer.cpp kMinGodRayStrength: below this the shafts are not drawn

    struct RayFrame {
        float toLightInView[3];     // unit vector to the body in view space (x right, y up, z forward)
        bool inFront;               // z > 0.05
        float ndc[2];               // projected position, -1..1 across the viewport (only if inFront)
        float sunPx[2];             // pixels from the top-left corner of the back buffer (only if inFront)
        bool onScreen;              // inFront and inside the viewport (|ndc| <= 1)
        float screenFade;           // clamp(1.6 - max(|ndcX|, |ndcY|), 0, 1): the shafts still draw a little beyond the edge, as in coa-vfog
        float visibility;           // FrameInputs::rayVisibility
        float strength;             // godRays * fog.lightVisibility * visibility * screenFade; 0 when the body is behind the camera
    };

    RayFrame ComputeRayFrame(const FrameInputs& in, const FogParams& fog, float godRays);

    FogParams BuildFogParams(const FrameInputs& in, const ModelTuning& cfg);

    void Mul4x4(const float* a, const float* b, float* out);
    bool Invert4x4(const float* m, float* out);
    void TransformDirection(const float* v, const float* m, float* out);
}

#endif

#ifndef ENGINE_SOURCE_VOLFOG_VOLFOGLIGHTS_H
#define ENGINE_SOURCE_VOLFOG_VOLFOGLIGHTS_H

#include <cstdint>
#include "Engine/Source/VolFog/VolFogInputs.h"

class CGxTex;
struct IDirect3DDevice9;

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B8: point lights scattered in the fog.
// Licensed under GPL-3.0-only. The structures, the constants and the selection rules are derived from src/engine_lights.h/.cpp and
// src/local_light_stabiliser.h/.cpp of coa-vfog 205ade5 (copyright of the authors of CoAVolFog, jealous-sound and the contributors of the
// Raz0r1337 fork; see Shaders/VolFog/NOTICE-modificaciones.txt). Modified 2026-10-20 (brief B8): in the namespace VolFog; the lights are read
// from CM2Scene::m_lightGrid of the world scene instead of fixed addresses of the 3.3.5 binary (no signature check, no guarded memory
// reads); the 32 x 1 A32B32G32R32F texture is created and filled here and bound by VolFogRender.cpp.

namespace VolFog {
    constexpr uint32_t kMaxLocalPointLights = 8;                    // slots uploaded to the shader (3 texels each: 24 of the 32 texels)
    constexpr uint32_t kLocalLightCandidatePool = 24;               // lights in view the stabiliser chooses from
    // Brief B10 (REVISION-B7-B8, finding 1): the cut-off is 1/32 of the peak colour (it was 1/256). Every CM2Light has the same fixed attenuation
    // (0; 0.7; 0.03), so the radius of a light of peak colour 1 goes from 81 yd to 23 yd (from 173 yd to 55 yd for a peak of 4): the camera of a camp
    // is no longer inside the sphere of every light, and the march with local lights stops running the full step count for every pixel. The boundary
    // fade of the shader keeps the edge of the sphere soft.
    constexpr float kLocalPointLightContributionCutoff = 1.0f / 32.0f;
    // Brief B10: the lights are scattered only from this wolVolFog quality on (1 = no lights; 2 and 3 = up to 8), until they are measured on a modest GPU.
    constexpr int32_t kMinLocalLightQuality = 2;
    constexpr float kMaxLocalPointLightRadius = 200.0f;             // yards
    constexpr float kLocalLightGamma = 2.2f;
    constexpr float kLocalLightIntensity = 1.0f;                    // `LocalLightIntensity` of coa-vfog (default 1.0)
    constexpr float kLocalLightPhase = 0.3f;                        // `LocalLightPhase`: Henyey-Greenstein g of the lights (c53.z)

    // Camera of the frame for the view test and the priority (world position, camera-relative view and projection of FrameInputs).
    struct LightCamera {
        float position[3] = {};
        float cameraRelativeView[16] = {};
        float glProjection[16] = {};
    };

    struct PointLight {
        float position[3] = {};         // world space (CM2Light::m_pos)
        float color[3] = {};            // CM2Light::m_dirColor, as the client uploads it to its vertex shaders
        float attenuation[3] = {};      // constant, linear, quadratic
        float uploadedColor[3] = {};    // linearised, scaled by the intensity and, after the stabiliser, by the fade
        float cutoff = 0.0f;            // yards where the light drops under 1/256 of its colour (at most 200)
        uint32_t enabled = 1;
        uintptr_t nativeId = 0;         // address of the CM2Light, identity of the light between frames
    };

    enum class LightCapture : uint32_t { Captured, DamagedTable, TableTruncated };

    struct LightSet {
        PointLight pointLights[kMaxLocalPointLights] = {};
        uint32_t pointLightCount = 0;
        PointLight candidates[kLocalLightCandidatePool] = {};
        uint32_t candidateCount = 0;
        uint32_t skippedDisabled = 0;
        uint32_t skippedNonPoint = 0;
        uint32_t nodesWalked = 0;
        LightCapture capture = LightCapture::Captured;
    };

    // Called by CreateDepthTexture (the pure D3D9 device the texture is made on) and ReleaseDepthTexture (before a Reset or a rebuild: the
    // light texture is dropped with the depth and made again on the next use).
    void SetLightDevice(IDirect3DDevice9* device);
    void ReleaseLightTexture();

    // Once per frame from VolFog::Render, after the inputs: walks the light grid of FrameInputs::worldM2Scene, feeds the stabiliser and uploads
    // the chosen lights to the texture. Returns the number of lights uploaded (0 = use the unlit shader variants). `enabled` is
    // wolVolFogLocalLights; with 0 (or no scene) nothing is captured and the stabiliser is left alone.
    uint32_t PrepareLights(const FrameInputs& in, uint32_t frame, double seconds, bool enabled);

    // The result of the last PrepareLights: s8 (nullptr when there are no lights), and the second component of c53 (the farthest distance along
    // a ray where any light can still reach: |view position| + cutoff).
    CGxTex* LightTex();
    float LightReachLimit();
    uint32_t LightCount();
    // Brief B10: how many times the 32 x 1 texture has been created (a Reset or a rebuild of the device makes a new one); the temporal filter drops its
    // history when it changes, because the lit march of the old texture and the new one are not the same image.
    uint32_t LightTextureGeneration();

    // wolVolFogDump: one line for the capture and one per uploaded light (view position, screen pixel, colours, attenuation, cutoff).
    typedef void (*LightLineSink)(void* user, const char* line);
    void DescribeLights(const FrameInputs& in, LightLineSink sink, void* user);
}

#endif

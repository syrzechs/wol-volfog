#ifndef ENGINE_SOURCE_VOLFOG_VOLFOGINPUTS_H
#define ENGINE_SOURCE_VOLFOG_VOLFOGINPUTS_H

#include <cstdint>

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B5.
// Licensed under GPL-3.0-only. FrameInputs is derived from `struct FrameInputs` of src/engine.h in coa-vfog 205ade5 (copyright of
// the authors of CoAVolFog, jealous-sound and the contributors of the Raz0r1337 fork; see Shaders/VolFog/NOTICE-modificaciones.txt).
// Modified 2026-10-20 (brief B5): in the namespace VolFog; D3DVIEWPORT9 replaced by VolFog::Viewport; the Classic-only members
// (lightParams, point light table) are gone; toRaySource added; the values are read straight from the client instead of from fixed
// addresses (WoW/Source/WorldClient/VolFogWorld.cpp).

class CM2Scene;

namespace VolFog {
    // The world viewport that CGWorldFrame::OnWorldRender left set, in pixels from the top-left corner of the back buffer (same
    // meaning as D3DVIEWPORT9), plus its depth range (z 0..0.94 for the world).
    struct Viewport {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        float minZ = 0.0f;
        float maxZ = 1.0f;
    };

    // Brief B5: the two interior values. Brief B8: pointLightCount, set by VolFog::PrepareLights (VolFogLights.h keeps the lights themselves).
    struct LocalLightInputs {
        bool cameraInterior = false;    // the viewer is inside a WMO group that is not an exterior group (flags & 0x48 == 0)
        float interiorBlend = 0.0f;     // 0..1: DNInfo.m_interiorFogBlend (distance to the nearest exterior portal * 0.04); 1 when no portal sees outside
        uint32_t pointLightCount = 0;   // B8: lights uploaded to the fog shaders this frame (0 = unlit shader variants)
    };

    // Everything the fog model and the passes need from the client for one frame (design 1.3). The member names are the ones of
    // coa-vfog's FrameInputs, so that the packing of the constants (renderer.cpp) can be followed one to one.
    struct FrameInputs {
        float cameraRelativeView[16];   // GxXformView: eye at the origin, row-major, the same layout as the D3D matrix
        float glProjection[16];         // GxXformProjection (GxuXformCreateProjection_Exact): ndc z = c2 + d2 / z, so far plane = [14] / (1 - [10])
        float camPos[3];                // active camera position (world space)
        float camTarget[3];             // the camera target (DNInfo.m_lightRefPos: the followed unit, or the camera itself)
        Viewport viewport;
        float dayFraction;              // DNInfo.m_dayProgression 0..1 (0.5 = noon)
        float toLight[3];               // unit vector to the light that scatters: the visible sun or moon (= toRaySource, brief B6b)
        bool lightIsMoon;               // the sun is below the horizon
        uint32_t fogColor;              // A8R8G8B8, DayNight::s_volFogSourceFog.m_color (= m_fogInterior before UpdateFog pushes it out of range)
        uint32_t sunColor;              // DNInfo.m_bands.m_sunCloudColors[0]
        uint32_t directColor;           // DNInfo.m_bands.m_diffuse (SetLightColors: m_light1.m_diffuse)
        uint32_t ambientColor;          // DNInfo.m_bands.m_ambient (SetLightColors: m_light1.m_ambient)
        float fogStart;                 // DayNight::s_volFogSourceFog.m_start (brief B6b)
        float fogEnd;                   // DayNight::s_volFogSourceFog.m_end (brief B6b)
        float zoneFogDistance;          // DNInfo.m_bands.m_fogEnd (Light.dbc, before the far clip and the wolFarclipFactor)
        float farClip;                  // DNInfo.m_farClip = camera far plane
        float clientGlowAmount;         // DNInfo.m_bands.m_glow, only while ffxGlow is the active screen effect (0 otherwise, 0 in liquid)
        bool inLiquid;                  // World::SceneCamLiquidStatus != 0
        int32_t mapId;                  // World::QueryMapId
        LocalLightInputs localLights;
        // Added in brief B5 (not in coa-vfog's struct): the beams (brief B9) come from the visual position of the body, which does not
        // coincide with the light direction.
        float toRaySource[3];           // unit vector to the visual position of the sun, or of the moon when lightIsMoon
        float sunElevation;             // z of the unit vector to the visual sun position (negative = below the horizon); lightIsMoon = it is < 0
        float viewerLiquidDepth;        // World::SceneCamLiquidStatus(&depth): for the dump
        uint32_t liquidTypeId;          // the same call, return value
        bool fullyInterior;             // the viewer is in a WMO and no portal sees the outside (CPortalView::m_maxViewDepth < 0)
        bool glowEffectActive;          // ffxGlow is the active screen effect
        int32_t fogMode;                // Added in brief B10: DayNight's fogMode (0 = linear fog, 1 = exponential fog of Outland / Northrend maps)
        // Added in brief B8: the scene whose CM2Scene::m_lightGrid holds the point lights of the world (World::s_m2Scene); VolFog::PrepareLights walks it.
        CM2Scene* worldM2Scene;
        // Added in brief B9: visibility of the body the shafts start from (the sun, or the moon when lightIsMoon). rayOcclusion is the fraction 0..1 of
        // the body that the occlusion query of the glare (DNGlare::GetQueryOcclusion, or GetPointOcclusion without a query) sees, as the client itself
        // uses it for its glare; rayVisibility is that value eased in and out like the glare fade (up 4 / s, down 1.5 / s) so that branches and
        // clouds do not make the shafts pop. glareFade (DNGlare::m_fade) and glareScreenGlow (m_screenGlow) are only for the dump: they are NOT
        // used, because the fade of the moon glare includes DNMoonGlare::GetCloudDensityFade (1 - |2 (density - 0.5)|), which is 0 under a clear
        // sky, exactly when the moon is most visible.
        float rayOcclusion;
        float rayVisibility;
        float glareFade;
        float glareScreenGlow;
    };

    // The client side (WoW/Source/WorldClient/VolFogWorld.cpp) installs this when the CVars are registered: Engine/ does not depend
    // on WoW/, so it only sees a function pointer. It returns false when it cannot fill the structure (no world frame, no camera).
    typedef bool (*InputsProvider)(FrameInputs& out);
    extern InputsProvider s_inputsProvider;

    // Fills `out` from the client for the frame being drawn (call it from inside the world render: the matrices are the world camera's).
    // Returns false, with `out` zeroed, when there is no provider, the provider fails or a value is not finite.
    bool CollectInputs(FrameInputs& out);

    // Small float helpers shared with the model: ARGB to 0..1 RGB.
    void UnpackColor(uint32_t argb, float* rgb);
}

#endif

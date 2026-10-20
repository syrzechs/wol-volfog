#include "WoW/Source/WorldClient/VolFogWorld.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "Engine/Source/ConsoleBase/ConsoleCommand.h"
#include "Engine/Source/ConsoleBase/ConsoleVar.h"
#include "Engine/Source/FFX/FFX.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CRect.h"
#include "Engine/Source/VolFog/VolFog.h"
#include "Engine/Source/VolFog/VolFogInputs.h"
#include "Engine/Source/VolFog/VolFogModel.h"
#include "WoW/Source/UIUtil/Camera.h"
#include "WoW/Source/Ui/WorldFrame.h"
#include "WoW/Source/WorldClient/CPortalView.h"
#include "WoW/Source/WorldClient/DayNight.h"
#include "WoW/Source/WorldClient/DayNight__DNInfo.h"
#include "WoW/Source/WorldClient/MapObj.h"
#include "WoW/Source/WorldClient/MapObjDef.h"
#include "WoW/Source/WorldClient/MapObjGroup.h"
#include "WoW/Source/WorldClient/WorldParam.h"
#include "WoW/Source/WorldClient/World.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B5.
// Modified 2026-10-20 (brief B6b): toLight is the visible sun or moon (= toRaySource, as in coa-vfog), and the fog values come from the copy
// of DNInfo::m_fogInterior that DayNight::UpdateFog keeps before pushing the live groups out of range. Brief B8: FrameInputs::worldM2Scene.
// Brief B9: FrameInputs::rayOcclusion / rayVisibility (the occlusion of the sun or moon glare, eased), glareFade, glareScreenGlow (dump only).
// Brief B10: lightIsMoon switches at kSunLightFloor instead of at the horizon; FrameInputs::fogMode.
// Licensed under GPL-3.0-only. This is the client side of FrameInputs (src/engine.h of coa-vfog 205ade5 reads the same values from
// fixed addresses of the 3.3.5 binary; see Shaders/VolFog/NOTICE-modificaciones.txt). Written 2026-10-20 (brief B5).
//
// Names that differ from the table in the design (1.3):
//  - fogColor / fogStart / fogEnd come from the copy of DNInfo::m_fogInterior that UpdateFog keeps (DayNight::s_volFogSourceFog, brief B6b),
//    not from m_fog: it holds the final values (interior blend, eclipse and glow included; it equals m_fog outside WMOs, and inside only the
//    colour differs). UpdateFog pushes both live groups out of range to switch the linear fog off, so the live values are not the real ones.
//    coa-vfog reads the same group (kFrameInputsFogGroup = 1).
//  - sunColor / directColor / ambientColor are DNInfo::m_bands.m_sunCloudColors[0] / m_diffuse / m_ambient (SetLightColors copies
//    the last two to m_light1.m_diffuse / m_ambient).
//  - camTarget is DNInfo::m_lightRefPos (UpdateDayNight: the followed object, or the camera if there is none); CSimpleCamera has no
//    target.
//  - camPos is CGCamera::m_position (CSimpleCamera); the camera far plane is DNInfo::m_farClip = CGCamera::m_farZ.
//  - the mapId is World::QueryMapId(); the interior test is CWorldScene::s_viewerMapObjDef + s_viewerMapObjGroups (group flags & 0x48
//    as in CMapEntity::QueryCameraFog) and CWorldScene::frustumPortalView.m_maxViewDepth < 0 (CWorldScene::Render), not
//    DNInfo::m_interiorMapObjDef, which UpdateFog clears every frame.

namespace {
    bool Normalize(float* v) {
        float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

        if (!(length > 1.0e-6f) || !std::isfinite(length)) {
            return false;
        }

        v[0] /= length;
        v[1] /= length;
        v[2] /= length;

        return true;
    }

    uint32_t Pack(const CImVector& color) {
        return (static_cast<uint32_t>(color.a) << 24) | (static_cast<uint32_t>(color.r) << 16) | (static_cast<uint32_t>(color.g) << 8) | static_cast<uint32_t>(color.b);
    }

    bool CollectFrameInputs(VolFog::FrameInputs& out) {
        CGCamera* camera = CGWorldFrame::GetActiveCamera();

        if (!camera || !g_theGxDevicePtr) {
            return false;
        }

        DayNight::DNInfo& info = DayNight::g_dnInfo;

        // Matrices and viewport of the frame: GxXformView / GxXformProjection / GxXformViewport are what OnWorldRender left set.
        C44Matrix view;
        C44Matrix projection;
        GxXformView(view);
        GxXformProjection(projection);
        static_assert(sizeof(C44Matrix) == sizeof(out.cameraRelativeView), "C44Matrix is 16 floats");
        std::memcpy(out.cameraRelativeView, &view, sizeof(out.cameraRelativeView));
        std::memcpy(out.glProjection, &projection, sizeof(out.glProjection));

        CRect window = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_theGxDevicePtr->CapsWindowSize(window);
        float width = window.maxX - window.minX;
        float height = window.maxY - window.minY;
        float minX = 0.0f;
        float maxX = 0.0f;
        float minY = 0.0f;
        float maxY = 0.0f;
        float minZ = 0.0f;
        float maxZ = 1.0f;
        GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);

        // Same conversion as VolFog::Render: Gx viewport y grows upwards, pixels from the top-left corner of the back buffer.
        out.viewport.x = static_cast<uint32_t>(std::max(minX * width, 0.0f));
        out.viewport.y = static_cast<uint32_t>(std::max((1.0f - maxY) * height, 0.0f));
        out.viewport.width = static_cast<uint32_t>(std::max((maxX - minX) * width, 0.0f));
        out.viewport.height = static_cast<uint32_t>(std::max((maxY - minY) * height, 0.0f));
        out.viewport.minZ = minZ;
        out.viewport.maxZ = maxZ;

        out.camPos[0] = camera->m_position.x;
        out.camPos[1] = camera->m_position.y;
        out.camPos[2] = camera->m_position.z;
        out.camTarget[0] = info.m_lightRefPos.x;
        out.camTarget[1] = info.m_lightRefPos.y;
        out.camTarget[2] = info.m_lightRefPos.z;

        out.dayFraction = info.m_dayProgression;

        // The visual positions of the sun and the moon (SetPlanets: 12 yards from DNInfo::m_cameraPos). The light is the moon while the sun is
        // below the horizon. Brief B6b: the scattering light (toLight) is that same visible body, as in coa-vfog (src/engine.cpp:417-425), and
        // not the direction of the client's own lighting (-m_light1.m_dir: its z only goes from 0.34 to 0.60 over the day, so the sun never
        // faded at dusk and the forward lobe of the haze was centred above the visible disc). The beams (B9) start at the same point.
        float toSun[3] = { DayNight::g_planets.m_sun.m_position.x - info.m_cameraPos.x, DayNight::g_planets.m_sun.m_position.y - info.m_cameraPos.y, DayNight::g_planets.m_sun.m_position.z - info.m_cameraPos.z };
        float toMoon[3] = { DayNight::g_planets.m_moon.m_position.x - info.m_cameraPos.x, DayNight::g_planets.m_moon.m_position.y - info.m_cameraPos.y, DayNight::g_planets.m_moon.m_position.z - info.m_cameraPos.z };

        if (!Normalize(toSun) || !Normalize(toMoon)) {
            return false;
        }

        // Brief B10 (REVISION-B7-B8, finding 4): the body changes from the sun to the moon when the sun has no light left (z = kSunLightFloor, where its
        // visibility SmoothStep(-0.03, 0.10, z) is 0), not at the horizon, and the moon fades in from there (VolFogModel.cpp), so neither the scattering
        // nor the shafts jump at dusk or dawn.
        out.sunElevation = toSun[2];
        out.lightIsMoon = toSun[2] < VolFog::kSunLightFloor;
        std::memcpy(out.toRaySource, out.lightIsMoon ? toMoon : toSun, sizeof(out.toRaySource));
        std::memcpy(out.toLight, out.toRaySource, sizeof(out.toLight));

        // Brief B9: visibility of that body for the shafts (see FrameInputs::rayVisibility): the occlusion of the glare of the sun or the moon, the
        // same choice as DNGlare::GetOcclusionFade (the query when the glare has one and the occlusion CVar is on, the point test otherwise) but
        // without its early exits (the client does not even ask for the moon when its glare fade is 0) and without the clouds or the screen glow.
        // It begins and ends the query of the glare at most once per frame: a second call in the same frame finds the result not ready and returns the
        // stored pixel count (DNGlare::GetQueryOcclusion), so the dump and the pass do not disturb the client's own call for the sun.
        DayNight::DNGlare* glare = out.lightIsMoon ? static_cast<DayNight::DNGlare*>(&DayNight::g_moonGlare) : static_cast<DayNight::DNGlare*>(&DayNight::g_sunGlare);
        float occlusion = 0.0f;

        if (DayNight::GlareBase::m_masterEnable && glare->m_enable && glare->m_planet) {
            occlusion = glare->m_occlusionQuery && CWorldParam::cvar_occlusion && CWorldParam::cvar_occlusion->m_intValue ? glare->GetQueryOcclusion() : glare->GetPointOcclusion();
        }

        out.rayOcclusion = std::isfinite(occlusion) ? std::clamp(occlusion, 0.0f, 1.0f) : 0.0f;

        // Easing (the fade rates of InitSunGlare / InitMoonGlare): up 4 / s, down 1.5 / s, from the frame time of the client. Two reads in the same
        // frame (dump + pass) move it twice by at most a frame's worth.
        static float s_visibility = 0.0f;
        const float deltaSec = std::isfinite(info.m_deltaSec) ? std::clamp(info.m_deltaSec, 0.0f, 0.25f) : 0.0f;
        const float step = (out.rayOcclusion > s_visibility ? 4.0f : 1.5f) * deltaSec;
        s_visibility = out.rayOcclusion > s_visibility ? std::min(s_visibility + step, out.rayOcclusion) : std::max(s_visibility - step, out.rayOcclusion);
        out.rayVisibility = s_visibility;
        out.glareFade = std::isfinite(glare->m_fade) ? std::clamp(glare->m_fade, 0.0f, 1.0f) : 0.0f;
        out.glareScreenGlow = std::isfinite(glare->m_screenGlow) ? std::clamp(glare->m_screenGlow, 0.0f, 1.0f) : 0.0f;

        const DayNight::VolFogSourceFog& sourceFog = DayNight::s_volFogSourceFog; // brief B6b: the real values of the frame, before UpdateFog pushes the live ones out of range
        out.fogColor = Pack(sourceFog.m_color);
        out.sunColor = Pack(info.m_bands.m_sunCloudColors[0]);
        out.directColor = Pack(info.m_bands.m_diffuse);
        out.ambientColor = Pack(info.m_bands.m_ambient);
        out.fogStart = sourceFog.m_start;
        out.fogEnd = sourceFog.m_end;
        out.zoneFogDistance = info.m_bands.m_fogEnd;

        out.farClip = info.m_farClip;

        if (!(out.farClip > 10.0f && out.farClip < 100000.0f)) {
            out.farClip = out.glProjection[14] / (1.0f - out.glProjection[10]);
        }

        if (!(out.farClip > 10.0f && out.farClip < 100000.0f)) {
            out.farClip = 1000.0f;
        }

        float liquidDepth = 0.0f;
        int32_t liquidType = World::SceneCamLiquidStatus(&liquidDepth);
        out.inLiquid = liquidType != 0;
        out.liquidTypeId = static_cast<uint32_t>(liquidType);
        out.viewerLiquidDepth = liquidDepth;
        out.mapId = World::QueryMapId();
        out.fogMode = sourceFog.m_mode; // brief B10
        out.worldM2Scene = World::s_m2Scene; // brief B8: the point lights of the fog are read from its light grid

        // ffxGlow: only while the glow is the active screen effect (CGWorldFrame::UpdateScreenEffectParams returns early otherwise).
        out.glowEffectActive = FFX::s_cvarFFX && FFX::s_cvarFFX->m_intValue && FFX::s_effect && FFX::s_effect == CGWorldFrame::ffxEffectGlow && FFX::s_effect->Valid();
        out.clientGlowAmount = out.glowEffectActive && !out.inLiquid ? std::clamp(info.m_bands.m_glow, 0.0f, 1.0f) : 0.0f;

        // Interior: the viewer is in a WMO group that is not an exterior group (flags 0x8 / 0x40), the test CMapEntity::QueryCameraFog uses.
        CMapObjDef* viewer = CWorldScene::s_viewerMapObjDef;
        bool interior = false;

        if (viewer && viewer->m_owner) {
            for (uint32_t i = 0; i < CWorldScene::s_viewerMapObjGroups.m_count; ++i) {
                CMapObjGroup* group = viewer->m_owner->GetGroup(CWorldScene::s_viewerMapObjGroups.m_data[i], 0);

                if (group && !(group->m_flags & 0x48)) {
                    interior = true;
                }
            }
        }

        out.fullyInterior = viewer != nullptr && CWorldScene::frustumPortalView.m_maxViewDepth < 0.0f;
        out.localLights.cameraInterior = interior;
        float blend = std::isfinite(info.m_interiorFogBlend) ? std::clamp(info.m_interiorFogBlend, 0.0f, 1.0f) : 0.0f;
        out.localLights.interiorBlend = interior && out.fullyInterior ? 1.0f : blend;

        return true;
    }

    int32_t Command_Dump(const char* command, const char* arguments) {
        VolFog::RequestDump();

        return 1;
    }
}

void VolFogWorld::Install() {
    VolFog::s_inputsProvider = CollectFrameInputs;
}

void VolFogWorld::RegisterCommands() {
    ConsoleCommandRegister("wolVolFogDump", Command_Dump, CATEGORY_GRAPHICS, "Prints the inputs and layers of the native volumetric fog to the console and Logs\\volfog.log");
}

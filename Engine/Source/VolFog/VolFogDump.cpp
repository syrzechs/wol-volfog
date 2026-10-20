#include <cmath>
#include <cstdio>
#include <ctime>
#include <direct.h>
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/VolFog/VolFog.h"
#include "Engine/Source/VolFog/VolFogInputs.h"
#include "Engine/Source/VolFog/VolFogLights.h"
#include "Engine/Source/VolFog/VolFogModel.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B5.
// Licensed under GPL-3.0-only (the layer fields are the ones of coa-vfog's FogLayer; see Shaders/VolFog/NOTICE-modificaciones.txt).
//
// wolVolFogDump: prints to the game console and appends to Logs\volfog.log (with a time stamp) the inputs the fog reads from the
// client for the frame and the layers the model derives from them. It only prints; it is a console command, not a CVar. The
// command runs on the event thread between frames, when the Gx matrices are not the world camera's, so it only asks for a dump
// (RequestDump) and VolFog::Render() performs it inside the next world frame (RunPendingDump).

namespace {
    bool s_dumpPending;

    void Emit(std::FILE* file, const char* line) {
        ConsoleWrite(line, DEFAULT_COLOR);

        if (file) {
            std::fputs(line, file);
            std::fputc('\n', file);
        }
    }

    float Length3(const float* v) {
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }

    void DumpNow() {
        char line[512];
        std::time_t now = std::time(nullptr);
        std::tm local = {};
        localtime_s(&local, &now);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

        _mkdir("Logs");
        std::FILE* file = std::fopen("Logs\\volfog.log", "a");

        const VolFog::SettingValues& settings = VolFog::Settings();
        std::snprintf(line, sizeof(line), "[%s] wolVolFogDump  wolVolFog=%d density=%.2f godRays=%.2f localLights=%d intzDepth=%d debug=%d", stamp, settings.quality, settings.density, settings.godRays, settings.localLights ? 1 : 0, VolFog::DepthTexture() ? 1 : 0, VolFog::s_debugMode);
        Emit(file, line);

        VolFog::FrameInputs in;

        if (!VolFog::CollectInputs(in)) {
            Emit(file, "  inputs: not available (no world frame, no camera, or a value is not finite)");

            if (file) {
                std::fclose(file);
            }

            return;
        }

        float fogColor[3];
        float sunColor[3];
        float directColor[3];
        float ambientColor[3];
        VolFog::UnpackColor(in.fogColor, fogColor);
        VolFog::UnpackColor(in.sunColor, sunColor);
        VolFog::UnpackColor(in.directColor, directColor);
        VolFog::UnpackColor(in.ambientColor, ambientColor);

        float hours = in.dayFraction * 24.0f;
        int hour = static_cast<int>(hours);
        int minute = static_cast<int>((hours - static_cast<float>(hour)) * 60.0f);

        std::snprintf(line, sizeof(line), "  in: dayFraction=%.4f (game time %02d:%02d) mapId=%d fogMode=%d lightIsMoon=%d inLiquid=%d (type %u, depth %.2f) glow=%.3f (ffxGlow %d)", in.dayFraction, hour, minute, in.mapId, in.fogMode, in.lightIsMoon ? 1 : 0, in.inLiquid ? 1 : 0, in.liquidTypeId, in.viewerLiquidDepth, in.clientGlowAmount, in.glowEffectActive ? 1 : 0);
        Emit(file, line);
        std::snprintf(line, sizeof(line), "  in: toLight=(%.4f, %.4f, %.4f) |toLight|=%.4f  toRaySource=(%.4f, %.4f, %.4f) sunElevation=%.4f", in.toLight[0], in.toLight[1], in.toLight[2], Length3(in.toLight), in.toRaySource[0], in.toRaySource[1], in.toRaySource[2], in.sunElevation);
        Emit(file, line);
        std::snprintf(line, sizeof(line), "  in: fogColor=(%.3f, %.3f, %.3f) sunColor=(%.3f, %.3f, %.3f) directColor=(%.3f, %.3f, %.3f) ambientColor=(%.3f, %.3f, %.3f)", fogColor[0], fogColor[1], fogColor[2], sunColor[0], sunColor[1], sunColor[2], directColor[0], directColor[1], directColor[2], ambientColor[0], ambientColor[1], ambientColor[2]);
        Emit(file, line);
        std::snprintf(line, sizeof(line), "  in: fogStart=%.2f fogEnd=%.2f zoneFogDistance=%.2f farClip=%.2f", in.fogStart, in.fogEnd, in.zoneFogDistance, in.farClip);
        Emit(file, line);
        std::snprintf(line, sizeof(line), "  in: cameraInterior=%d interiorBlend=%.3f fullyInterior=%d  camPos=(%.2f, %.2f, %.2f) camTarget=(%.2f, %.2f, %.2f)", in.localLights.cameraInterior ? 1 : 0, in.localLights.interiorBlend, in.fullyInterior ? 1 : 0, in.camPos[0], in.camPos[1], in.camPos[2], in.camTarget[0], in.camTarget[1], in.camTarget[2]);
        Emit(file, line);
        std::snprintf(line, sizeof(line), "  in: viewport=%u,%u %ux%u z %.2f..%.2f  projection far plane=%.2f", in.viewport.x, in.viewport.y, in.viewport.width, in.viewport.height, in.viewport.minZ, in.viewport.maxZ, in.glProjection[14] / (1.0f - in.glProjection[10]));
        Emit(file, line);

        VolFog::ModelTuning tuning = VolFog::CurrentTuning();
        VolFog::FogParams fog = VolFog::BuildFogParams(in, tuning);

        std::snprintf(line, sizeof(line), "  fog: active=%d linear=%d lightVisibility=%.4f lightAboveHorizon=%.4f referenceZ=%.2f farLimit=%.2f maxDistance=%.1f horizonStart=%.1f farClip=%.1f", fog.active ? 1 : 0, fog.linear ? 1 : 0, fog.lightVisibility, fog.lightAboveHorizon, fog.referenceZ, fog.farLimit, fog.maxDistance, fog.horizonStart, fog.farClip);
        Emit(file, line);

        // Brief B9: the light shafts of this frame (position of the sun or moon on the screen, visibility, strength) and whether the last frame drew them.
        VolFog::RayFrame rays = VolFog::ComputeRayFrame(in, fog, settings.godRays);

        std::snprintf(line, sizeof(line), "  rays: wolVolFogGodRays=%.2f body=%s toLightInView=(%.4f, %.4f, %.4f) inFront=%d ndc=(%.3f, %.3f) sunPx=(%.0f, %.0f) onScreen=%d screenFade=%.3f (the shafts draw up to |ndc| 1.6)", settings.godRays, in.lightIsMoon ? "moon" : "sun", rays.toLightInView[0], rays.toLightInView[1], rays.toLightInView[2], rays.inFront ? 1 : 0, rays.ndc[0], rays.ndc[1], rays.sunPx[0], rays.sunPx[1], rays.onScreen ? 1 : 0, rays.screenFade);
        Emit(file, line);
        std::snprintf(line, sizeof(line), "  rays: occlusion=%.4f visibility(eased)=%.4f lightVisibility=%.4f -> strength=%.4f (min %.3f) rayColour=(%.3f, %.3f, %.3f) drawnLastFrame=%d  (not used: glareFade=%.4f screenGlow=%.4f)", in.rayOcclusion, rays.visibility, fog.lightVisibility, rays.strength, VolFog::kMinGodRayStrength, fog.rayColor[0], fog.rayColor[1], fog.rayColor[2], VolFog::RaysDrawn() ? 1 : 0, in.glareFade, in.glareScreenGlow);
        Emit(file, line);

        static const char* const s_layerNames[VolFog::kFogLayers] = { "haze", "low fog", "unused", "distance" };

        for (int i = 0; i < VolFog::kFogLayers; ++i) {
            const VolFog::FogLayer& l = fog.layers[i];
            std::snprintf(line, sizeof(line), "  layer %d (%s): start=%.2f density=%.7f g=%.2f isotropic=%.2f emissive=(%.3f, %.3f, %.3f) diffuse=(%.3f, %.3f, %.3f) upper=%.2f/%.5f lower=%.2f/%.5f strength=%.1f exponent=%.1f skyFalloff=%.1f endDistance=%.2f", i, s_layerNames[i], l.start, l.density, l.g, l.isotropic, l.emissive[0], l.emissive[1], l.emissive[2], l.diffuse[0], l.diffuse[1], l.diffuse[2], l.upperHeight, l.upperFalloff, l.lowerHeight, l.lowerFalloff, l.strength, l.exponent, l.skyFalloff, l.endDistance);
            Emit(file, line);
        }

        // Brief B8: the lights of the last frame that drew (capture counters, stabiliser counters, and the lights uploaded to the shader).
        VolFog::DescribeLights(in, [](void* user, const char* text) { Emit(static_cast<std::FILE*>(user), text); }, file);

        if (file) {
            std::fclose(file);
        }
    }
}

void VolFog::RequestDump() {
    if (!s_dumpPending) {
        s_dumpPending = true;
        ConsoleWrite("wolVolFogDump: the dump is written in the next world frame (Logs\\volfog.log).", DEFAULT_COLOR);
    }
}

void VolFog::RunPendingDump() {
    if (s_dumpPending) {
        s_dumpPending = false;
        DumpNow();
    }
}

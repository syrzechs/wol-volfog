#include "Engine/Source/VolFog/VolFogInputs.h"

#include <cmath>
#include <cstring>

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B5.
// Licensed under GPL-3.0-only (see VolFogInputs.h and Shaders/VolFog/NOTICE-modificaciones.txt).

VolFog::InputsProvider VolFog::s_inputsProvider;

namespace {
    bool Finite(const float* values, int count) {
        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(values[i])) {
                return false;
            }
        }

        return true;
    }
}

// Same conversion as UnpackColor in coa-vfog's fog_model.cpp.
void VolFog::UnpackColor(uint32_t argb, float* rgb) {
    rgb[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    rgb[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    rgb[2] = static_cast<float>(argb & 0xFF) / 255.0f;
}

bool VolFog::CollectInputs(FrameInputs& out) {
    std::memset(&out, 0, sizeof(out));
    out.viewport.maxZ = 1.0f;

    if (!VolFog::s_inputsProvider || !VolFog::s_inputsProvider(out)) {
        std::memset(&out, 0, sizeof(out));
        out.viewport.maxZ = 1.0f;

        return false;
    }

    bool finite = Finite(out.cameraRelativeView, 16) && Finite(out.glProjection, 16) && Finite(out.camPos, 3)
        && Finite(out.camTarget, 3) && Finite(out.toLight, 3) && Finite(out.toRaySource, 3)
        && std::isfinite(out.dayFraction) && std::isfinite(out.fogStart) && std::isfinite(out.fogEnd)
        && std::isfinite(out.zoneFogDistance) && std::isfinite(out.farClip) && std::isfinite(out.clientGlowAmount)
        && std::isfinite(out.localLights.interiorBlend) && std::isfinite(out.rayVisibility) && std::isfinite(out.rayOcclusion) && std::isfinite(out.glareFade) && std::isfinite(out.glareScreenGlow);

    if (!finite) {
        std::memset(&out, 0, sizeof(out));
        out.viewport.maxZ = 1.0f;

        return false;
    }

    return true;
}

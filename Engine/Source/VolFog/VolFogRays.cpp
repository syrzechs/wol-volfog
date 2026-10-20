#include <algorithm>
#include <cmath>
#include "Engine/Source/VolFog/VolFogInputs.h"
#include "Engine/Source/VolFog/VolFogModel.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B9.
// Licensed under GPL-3.0-only. Derived from the sun block of Renderer::RenderPasses in src/renderer.cpp of coa-vfog 205ade5 (copyright of the authors of
// CoAVolFog, jealous-sound and the contributors of the Raz0r1337 fork; see Shaders/VolFog/NOTICE-modificaciones.txt). Modified 2026-10-20 (brief B9):
// in the namespace VolFog over FrameInputs; the strength gets the occlusion visibility of the sun or moon of the client (FrameInputs::rayVisibility).

VolFog::RayFrame VolFog::ComputeRayFrame(const FrameInputs& in, const FogParams& fog, float godRays) {
    RayFrame r = {};

    VolFog::TransformDirection(in.toRaySource, in.cameraRelativeView, r.toLightInView);

    const float length = std::sqrt(r.toLightInView[0] * r.toLightInView[0] + r.toLightInView[1] * r.toLightInView[1] + r.toLightInView[2] * r.toLightInView[2]);

    if (!(length > 1.0e-6f) || !std::isfinite(length)) {
        return r;
    }

    for (int i = 0; i < 3; ++i) {
        r.toLightInView[i] /= length;
    }

    r.visibility = std::isfinite(in.rayVisibility) ? std::clamp(in.rayVisibility, 0.0f, 1.0f) : 0.0f;
    r.inFront = r.toLightInView[2] > 0.05f;

    if (!r.inFront) {
        return r;
    }

    // renderer.cpp: ndc = view xy / z * the projection scale + its offset (proj[0], proj[5], proj[8], proj[9]); pixels from the top-left corner of the viewport.
    const float* proj = in.glProjection;
    r.ndc[0] = r.toLightInView[0] / r.toLightInView[2] * proj[0] + proj[8];
    r.ndc[1] = r.toLightInView[1] / r.toLightInView[2] * proj[5] + proj[9];
    r.sunPx[0] = static_cast<float>(in.viewport.x) + (r.ndc[0] * 0.5f + 0.5f) * static_cast<float>(in.viewport.width);
    r.sunPx[1] = static_cast<float>(in.viewport.y) + (0.5f - r.ndc[1] * 0.5f) * static_cast<float>(in.viewport.height);

    const float edge = std::max(std::fabs(r.ndc[0]), std::fabs(r.ndc[1]));
    r.onScreen = edge <= 1.0f;
    r.screenFade = std::isfinite(edge) ? std::clamp(1.6f - edge, 0.0f, 1.0f) : 0.0f;

    const float strength = godRays * fog.lightVisibility * r.visibility * r.screenFade;
    r.strength = std::isfinite(strength) ? std::max(strength, 0.0f) : 0.0f;

    return r;
}

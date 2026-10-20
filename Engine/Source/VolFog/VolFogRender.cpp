#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Tempest/C2iVector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CRect.h"
#include "Engine/Source/Tempest/CiRect.h"
#include "Engine/Source/VolFog/VolFog.h"
#include "Engine/Source/VolFog/VolFogInputs.h"
#include "Engine/Source/VolFog/VolFogLights.h"
#include "Engine/Source/VolFog/VolFogModel.h"
#include "Engine/Source/VolFog/VolFogPass.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), briefs B2 (debug views) and B6 (march and composite).
// Licensed under GPL-3.0-only (see Shaders/VolFog/NOTICE-modificaciones.txt). The packing of the constants and the order of the passes follow
// Renderer::Draw of src/renderer.cpp in coa-vfog 205ade5 (copyright of the authors of CoAVolFog, jealous-sound and the contributors of the
// Raz0r1337 fork); modified 2026-10-20 (brief B6): rewritten over the Gx layer (FFX plumbing, Gx render targets and texture units instead of
// raw Direct3D 9 calls), without the temporal filter, the local lights and the light shafts, which come in later briefs.
// Modified 2026-10-20 (brief B8): the local lights (the lit permutations of the march and the composite, c53, s8) as in Renderer::Draw.
// Modified 2026-10-20 (brief B7): temporal filter (history ping-pong, previous depth, reprojection matrix, per-frame jitter) as in
// Renderer::FilterWithHistory / Renderer::Draw of renderer.cpp; wolVolFogDebug 3 = filtered history, 4 = no temporal filter, 5 = test grid.
// Modified 2026-10-20 (brief B9): the light shafts (sun or moon): reduced scene copy, mask, two radial blurs and the final blend, as DrawGodRayMask /
// DrawGodRaysOverScene of renderer.cpp, over Gx; wolVolFogDebug 6 = the shaft texture. The shafts are blended after the fog, over a second copy of the
// fogged image (the late path of coa-vfog), not inside the composition shader.

int32_t VolFog::s_debugMode;

namespace {
    // wolVolFogDebug 1..3 (permutation = mode - 1 of WolVolFogDebug.bls): 1 depth in grey (needs the INTZ depth of B3),
    // 2 magenta tint over the world view, 3 test grid. The registers follow the contract of the design (section 3).
    class DebugPass : public VolFog::Pass {
        public:
        DebugPass() : VolFog::Pass("WolVolFogDebug", 4, "WolVolFogQuad", GxBlend_Alpha) {}

        // Permutation 3 (wolVolFogDebug 6, brief B9): the light shaft texture on s1, scaled by m_raysGain (0 = nothing to show, black).
        CGxTex* m_raysTexture = nullptr;
        float m_raysGain = 0.0f;

        void Execute() override {
            // The pixel constants c0..c3 belong to whoever draws next: put them back as they were.
            float saved[20];
            memcpy(saved, CGxDevice::s_shadowConstants[0].constants, sizeof(saved));

            // wolVolFogDebug 1 (permutation 0) reads the INTZ depth as s0; Render() has checked that it exists.
            CGxTex* depth = this->m_pixelIndex == 0 ? VolFog::DepthTex() : nullptr;

            float texelX = 1.0f / this->m_targetPx[0];
            float texelY = 1.0f / this->m_targetPx[1];

            if (depth && VolFog::DepthWidth() && VolFog::DepthHeight()) {
                texelX = 1.0f / static_cast<float>(VolFog::DepthWidth());
                texelY = 1.0f / static_cast<float>(VolFog::DepthHeight());
            }

            float c0[4] = { this->m_viewportPx[0], this->m_viewportPx[1], this->m_viewportPx[2], this->m_viewportPx[3] };
            float c1[4] = { 1.0f, 0.0f, texelX, texelY };
            float c2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            float c3[4] = { 0.0f, 0.0f, 5000.0f, 0.94f };
            float c4[4] = { this->m_raysGain, 0.0f, 0.0f, 0.0f };

            // Contract of the design (section 3) from the projection the world was drawn with and its viewport depth range
            // (World.cpp: z 0..0.94). The projection is the GL-style matrix of GxuXformCreateProjection_Exact: ndc z = c2 + d2 / z, and the
            // depth buffer holds minZ + (maxZ - minZ) * (ndc z + 1) / 2, so  depth = x + y / viewZ  with x = c3.x and y = c3.y (y < 0).
            C44Matrix projection;
            GxXformProjection(projection);

            float minX = 0.0f;
            float maxX = 0.0f;
            float minY = 0.0f;
            float maxY = 0.0f;
            float minZ = 0.0f;
            float maxZ = 0.0f;
            GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);

            if (!(maxZ - minZ > 0.0001f) || minZ < 0.0f || maxZ > 1.0f) {
                minZ = 0.0f;
                maxZ = 1.0f;
            }

            float extent = maxZ - minZ;
            c2[0] = projection.a0;
            c2[1] = projection.b1;
            c2[2] = projection.c0;
            c2[3] = projection.c1;
            c3[0] = minZ + extent * (1.0f + projection.c2) * 0.5f;
            c3[1] = extent * projection.d2 * 0.5f;
            c3[3] = maxZ;

            if (projection.c2 > 1.0f && projection.d2 < 0.0f) {
                // Far plane of the projection. The final fog uses max(5000, farclip) here; the debug view normalises by the far plane so that
                // the gradient shows up (design brief B3).
                c3[2] = projection.d2 / (1.0f - projection.c2);
            }

            this->Begin();

            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, c0, 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 1, c1, 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 2, c2, 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 3, c3, 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 4, c4, 1);
            GxRsSet(GxRs_Texture0, depth); // sDepth: POINT, CLAMP (flags of the wrapper); nullptr in the modes that do not read it
            GxRsSet(GxRs_Texture1, this->m_pixelIndex == 3 ? this->m_raysTexture : nullptr); // sRays (wolVolFogDebug 6)

            this->DrawQuad();
            this->End();

            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &saved[0], 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 1, &saved[4], 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 2, &saved[8], 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 3, &saved[12], 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 4, &saved[16], 1);
        }
    };

    DebugPass* s_debugPass;
    bool s_failureLogged;

    void Say(const char* message) {
        ConsoleWrite(message, DEFAULT_COLOR);
        CGxDevice::Log("%s", message);
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Brief B6: march and composite.
    // ---------------------------------------------------------------------------------------------------------------------------------

    constexpr uint32_t kMarchConstants = 100; // c0..c99, the range the fog writes (renderer.cpp: kPixelConstants); saved and restored around the pass

    // Everything the two shaders read from constant registers for one frame (contract of the design, section 3; the layout is the one of
    // Renderer::Draw in renderer.cpp).
    struct FogFrame {
        float common[9][4];         // c0-c8
        float light[4];             // c9  cLight (the shader declares c9 only; c10 is not read)
        float march[4];             // c11 cMarch
        float layers[24][4];        // c12-c35 four FogLayer of six registers
        float layerNoise[12][4];    // c36-c47 cLayerNoise: no authored noise in the derived mode, uploaded as zeros
        float localControl[4];      // c53 cLocalLightControl: lights uploaded, reach limit, phase g (B8)
        float variation[4];         // c78 cDensityVariation
        float composite[3][4];      // c96-c98 cComposite, cGodRayColour, cSun
        float rayMask[2][4];        // c9-c10 cRayMask, cRaySource (WolVolFogRayMask, B9)
        float rayBlur[2][2][4];     // c9-c10 cRadialBlur, cRayTarget of each of the two blur passes (WolVolFogRayBlur, B9)
        float rayComposite[3][4];   // c96-c98 cComposite, cGodRayColour, cSun of WolVolFogRayComposite (B9)
        float viewToPreviousClip[16]; // c9-c12 cViewToPreviousClip (temporal pass only, B7): current view -> previous clip, row major
        float temporal[4];          // c13 cTemporal (temporal pass only): history weight, history valid, adaptive lighting, 0
        float worldToView[16];      // not uploaded: becomes the "previous" view of the next frame
    };

    constexpr float kTemporalWeight = 0.85f;            // `Temporal` of coa-vfog: weight of the history in the blend
    constexpr double kHistoryMaxSeconds = 0.25;         // renderer.cpp kHistoryMaxSeconds: older history is dropped
    constexpr float kHistoryMaxMove = 30.0f;            // renderer.cpp kHistoryMaxMove: yards of camera movement in one frame (teleport, mount jump)
    constexpr float kHistoryMinForwardDot = 0.70710678f; // renderer.cpp kHistoryMinForwardDot: turns of more than 45 degrees in one frame

    // Light shafts (B9), the constants of renderer.cpp: the ray target is a quarter of the world viewport, the mask is pow(dot(view ray, sun), 8) of the sky
    // pixels whose luminance is over 0.12, and the blur is 8 taps toward the sun that decay by 0.9, 0.075 of the way to the sun in the first pass and
    // that over 8 in the second.
    constexpr uint32_t kRayScale = 4;
    constexpr float kRayFalloff = 8.0f;
    constexpr float kRayThreshold = 0.12f;
    constexpr float kRayStep = 0.075f;
    constexpr float kRayDecay = 0.9f;
    constexpr int kRayTaps = 8;

    void NullTexFunc(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t face, uint32_t level, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
    }

    // One pass object per shader (WolVolFogMarch / WolVolFogComposite, six permutations each: (quality - 1) * 2 + localLights).
    class FogPass : public VolFog::Pass {
        public:
        explicit FogPass(const char* pixelName, uint32_t permutations = 6) : VolFog::Pass(pixelName, permutations, "WolVolFogQuad", GxBlend_Opaque) {}

        void Execute() override {}

        static void UploadShared(const FogFrame& f) {
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &f.common[0][0], 9);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 9, f.light, 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 11, f.march, 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 12, &f.layers[0][0], 24);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 36, &f.layerNoise[0][0], 12);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 53, f.localControl, 1);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 78, f.variation, 1);
        }

        static void SetTexture(uint32_t unit, CGxTex* texture) {
            GxRsSet(static_cast<EGxRenderState>(GxRs_Texture0 + unit), static_cast<void*>(texture));
        }

        // Ray march into the low-resolution target (sub-pass 1). False if the target could not be created on the device.
        bool DrawMarch(const FogFrame& f, uint32_t permutation, CGxTex* depth, CGxTex* noise, CGxTex* lights, CGxTex* target) {
            this->SetPixelPermutation(permutation);
            this->Begin();

            GxRenderTargetSet(GxBuffers_Color, target, 0);

            if (target->m_needsCreation) {
                // The device could not make the texture: nothing must be drawn (the colour target would still be the back buffer).
                GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
                this->End();

                return false;
            }

            // The target is the low-resolution texture: the whole of it, not the world viewport of the back buffer.
            GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            GxRsSet(GxRs_ColorWrite, 15);
            GxRsSet(GxRs_ScissorTest, 0);

            FogPass::UploadShared(f);
            FogPass::SetTexture(0, depth); // sDepth, POINT / CLAMP
            FogPass::SetTexture(9, noise); // sDensityNoise, LINEAR / WRAP in u
            FogPass::SetTexture(8, lights); // sLocalLights (B8): 32 x 1 A32B32G32R32F, POINT / CLAMP; nullptr with the unlit permutations

            this->DrawQuad();

            GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
            this->End();

            return true;
        }

        // Temporal filter (sub-pass 2, first half): the current march, the previous history and its packed depth, plus the depth of this frame,
        // blended into `target` (the history that is not the previous one). False if the target could not be created on the device.
        bool DrawTemporal(const FogFrame& f, CGxTex* current, CGxTex* history, CGxTex* depth, CGxTex* historyDepth, CGxTex* target) {
            this->SetPixelPermutation(0);
            this->Begin();

            GxRenderTargetSet(GxBuffers_Color, target, 0);

            if (target->m_needsCreation) {
                GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
                this->End();

                return false;
            }

            GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            GxRsSet(GxRs_ColorWrite, 15);
            GxRsSet(GxRs_ScissorTest, 0);

            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &f.common[0][0], 9);  // c0-c8 (the shader reads c0-c3 and c8)
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 9, f.viewToPreviousClip, 4); // c9-c12 cViewToPreviousClip
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 13, f.temporal, 1);      // c13 cTemporal
            FogPass::SetTexture(0, current);      // sCurrent
            FogPass::SetTexture(1, history);      // sHistory (history[prev])
            FogPass::SetTexture(2, depth);        // sDepth (INTZ)
            FogPass::SetTexture(3, historyDepth); // sHistoryDepth (the depth packed by the previous frame)

            this->DrawQuad();

            GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
            this->End();

            return true;
        }

        // Temporal filter (sub-pass 2, second half): the linear depth of this frame packed into RGB for the reprojection test of the next one.
        bool DrawHistoryDepth(const FogFrame& f, CGxTex* depth, CGxTex* target) {
            this->SetPixelPermutation(0);
            this->Begin();

            GxRenderTargetSet(GxBuffers_Color, target, 0);

            if (target->m_needsCreation) {
                GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
                this->End();

                return false;
            }

            GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            GxRsSet(GxRs_ColorWrite, 15);
            GxRsSet(GxRs_ScissorTest, 0);

            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &f.common[0][0], 9);  // c0-c8 (the shader reads c0, c1 and c3)
            FogPass::SetTexture(0, depth);   // sDepth
            FogPass::SetTexture(3, nullptr); // the temporal pass sampled this very texture as sHistoryDepth: never leave it bound while it is the target

            this->DrawQuad();

            GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
            this->End();

            return true;
        }

        // Light shafts, mask (B9, sub-pass 4): the bright sky around the sun or moon from the reduced scene copy and the depth, into `target`.
        bool DrawRayMask(const FogFrame& f, CGxTex* depth, CGxTex* scene, CGxTex* target) {
            this->SetPixelPermutation(0);
            this->Begin();

            GxRenderTargetSet(GxBuffers_Color, target, 0);

            if (target->m_needsCreation) {
                GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
                this->End();

                return false;
            }

            GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            GxRsSet(GxRs_ColorWrite, 15);
            GxRsSet(GxRs_ScissorTest, 0);

            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &f.common[0][0], 9); // c0-c8: viewport, depth mapping, projection
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 9, &f.rayMask[0][0], 2); // c9-c10 cRayMask, cRaySource
            FogPass::SetTexture(0, depth); // sDepth
            FogPass::SetTexture(1, scene); // sDownsampledScene (LINEAR)

            this->DrawQuad();

            GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
            this->End();

            return true;
        }

        // Light shafts, one radial blur (B9, sub-pass 4): 8 taps from each texel toward the sun, `source` into `target`.
        bool DrawRayBlur(const float (*blur)[4], CGxTex* source, CGxTex* target) {
            this->SetPixelPermutation(0);
            this->Begin();

            GxRenderTargetSet(GxBuffers_Color, target, 0);

            if (target->m_needsCreation) {
                GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
                this->End();

                return false;
            }

            GxXformSetViewport(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
            GxRsSet(GxRs_ColorWrite, 15);
            GxRsSet(GxRs_ScissorTest, 0);

            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 9, &blur[0][0], 2); // c9-c10 cRadialBlur, cRayTarget
            FogPass::SetTexture(0, source); // sSource (LINEAR)

            this->DrawQuad();

            GxRenderTargetSet(GxBuffers_Color, nullptr, 0);
            this->End();

            return true;
        }

        // Light shafts, blend (B9, sub-pass 6b): `scene` is the copy of the fogged image, `rays` the blurred shafts; writes the world viewport of the back
        // buffer (the shader returns the scene with the shafts added).
        void DrawRayComposite(const FogFrame& f, CGxTex* scene, CGxTex* rays) {
            this->SetPixelPermutation(0);
            this->Begin();

            GxRsSet(GxRs_ColorWrite, 15);
            GxRsSet(GxRs_ScissorTest, 0);

            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, &f.common[0][0], 9);                // c0-c8
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 96, &f.rayComposite[0][0], 3);         // c96-c98 cComposite, cGodRayColour, cSun
            FogPass::SetTexture(2, rays);  // sGodRays (LINEAR)
            FogPass::SetTexture(3, scene); // sSceneBeforeFog: here the fogged image

            this->DrawQuad();
            this->End();
        }

        // Composite over the back buffer, full resolution inside the world viewport (sub-pass 6).
        void DrawComposite(const FogFrame& f, uint32_t permutation, CGxTex* depth, CGxTex* fog, CGxTex* march, CGxTex* scene, CGxTex* lights) {
            this->SetPixelPermutation(permutation);
            this->Begin();

            GxRsSet(GxRs_ColorWrite, 15);
            GxRsSet(GxRs_ScissorTest, 0);

            FogPass::UploadShared(f);
            g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 96, &f.composite[0][0], 3);
            FogPass::SetTexture(0, depth);  // sDepth
            FogPass::SetTexture(1, fog);    // sFog: the filtered history[cur] (B7); the march itself without the temporal filter
            FogPass::SetTexture(2, march);  // sGodRays: unused (cComposite.y = 0), bound so that the sampler is never empty
            FogPass::SetTexture(3, scene);  // sSceneBeforeFog
            FogPass::SetTexture(4, march);  // sCurrentMarch: the unfiltered march of this frame
            FogPass::SetTexture(8, lights); // sLocalLights (B8): the lit composite checks which rays meet a light
            FogPass::SetTexture(9, VolFog::DensityNoiseTex()); // sDensityNoise (B10): the full resolution integration (silhouettes, rays that meet a light) reads the density variation; RsPop gives s9 back as the client had it

            this->DrawQuad();
            this->End();
        }
    };

    struct FogState {
        const CGxDevice* device;
        CGxTex* march;
        uint32_t marchWidth;
        uint32_t marchHeight;
        EGxTexFormat marchFormat;
        CGxTex* scene;
        uint32_t sceneWidth;
        uint32_t sceneHeight;
        uint32_t epoch;
        int32_t seenMode;
        uint32_t frame;
        uint32_t consecutiveFailures;
        bool shadersMessage;
        bool hardMessage;
        bool litMessage;
        bool noDepthDebugWarned;
        FogPass* marchPass;
        FogPass* compositePass;
        // Brief B7: temporal filter. history[historyIndex] is the last one written (the "previous" of the next frame); historyDepth is the packed
        // linear depth of that frame (ARGB8: the shader reads RGB = high byte, low byte, depth class).
        FogPass* temporalPass;
        FogPass* historyDepthPass;
        CGxTex* history[2];
        CGxTex* historyDepth;
        uint32_t historyWidth;
        uint32_t historyHeight;
        EGxTexFormat historyFormat;
        uint32_t historyIndex;
        bool historyValid;      // the last frame drew with the temporal filter and everything below describes it
        bool temporalOff;       // the history targets could not be made: the fog runs without the filter until the device or wolVolFog changes
        bool prevLit;           // B10: the last drawn frame used the local light variants; a change drops the history
        uint32_t prevLightGeneration; // B10: VolFog::LightTextureGeneration() of the last drawn frame
        float prevWorldToView[16];
        float prevProj[16];
        float prevCam[3];
        uint32_t prevViewport[4];
        float prevDepthRange[2];
        uint32_t prevScale;
        int32_t prevQuality;
        float prevDensity;
        int32_t prevMap;
        double prevSeconds;
        // Brief B9: light shafts. rays[0] / rays[1] are ARGB8, LINEAR, a quarter of the world viewport (kRayScale); raysOff = shaders or targets not available.
        FogPass* rayMaskPass;
        FogPass* rayBlurPass;
        FogPass* rayCompositePass;
        CGxTex* rays[2];
        uint32_t rayWidth;
        uint32_t rayHeight;
        bool raysOff;
        bool raysDrawn;
    };

    FogState s_fog;

    std::chrono::steady_clock::time_point s_clockStart = std::chrono::steady_clock::now();

    CGxTex* CreateTargetTexture(uint32_t width, uint32_t height, EGxTexFormat format, const char* name, EGxTexFilter filter = GxTex_Nearest) {
        // Exact size (GxTex_NonPow2): the shaders address the targets in texels of 1 / size. Nearest filter (Linear for the light shafts, B9), clamp, a
        // render target of the device (the device releases it on a reset and makes it again when it is next used).
        CGxTexFlags flags(filter, 0, 0, 0, 0, 1, 1, 0, 0, 0);
        CGxTex* texture = nullptr;

        if (!GxTexCreate(GxTex_NonPow2, width, height, 0, format, format, flags, nullptr, NullTexFunc, name, texture)) {
            return nullptr;
        }

        return texture;
    }

    void DestroyHistory() {
        for (int i = 0; i < 2; i++) {
            if (s_fog.history[i]) {
                GxTexDestroy(s_fog.history[i]);
                s_fog.history[i] = nullptr;
            }
        }

        if (s_fog.historyDepth) {
            GxTexDestroy(s_fog.historyDepth);
            s_fog.historyDepth = nullptr;
        }

        s_fog.historyWidth = 0;
        s_fog.historyHeight = 0;
        s_fog.historyIndex = 0;
        s_fog.historyValid = false;
    }

    // `wantHistory`: the temporal filter is going to run (B7), so its two history targets and the packed depth are needed. `historyReset` is set when
    // they were just (re)made: whatever the old history held is gone.
    bool EnsureTargets(uint32_t lowWidth, uint32_t lowHeight, uint32_t sceneWidth, uint32_t sceneHeight, bool wantHistory, bool& historyReset) {
        historyReset = false;

        if (s_fog.device != g_theGxDevicePtr) {
            // The device that owned the textures is gone (gxApi change): they went with it.
            s_fog.march = nullptr;
            s_fog.scene = nullptr;
            s_fog.marchWidth = 0;
            s_fog.marchHeight = 0;
            s_fog.sceneWidth = 0;
            s_fog.sceneHeight = 0;
            s_fog.history[0] = nullptr;
            s_fog.history[1] = nullptr;
            s_fog.historyDepth = nullptr;
            s_fog.historyWidth = 0;
            s_fog.historyHeight = 0;
            s_fog.historyIndex = 0;
            s_fog.historyValid = false;
            s_fog.rays[0] = nullptr;
            s_fog.rays[1] = nullptr;
            s_fog.rayWidth = 0;
            s_fog.rayHeight = 0;
            s_fog.device = g_theGxDevicePtr;
            historyReset = true;
        }

        if (s_fog.marchFormat == GxTex_Unknown) {
            s_fog.marchFormat = VolFog::s_caps.abgr16f ? GxTex_Abgr16F : GxTex_Argb8888;
        }

        if (!s_fog.march || s_fog.marchWidth != lowWidth || s_fog.marchHeight != lowHeight) {
            if (s_fog.march) {
                GxTexDestroy(s_fog.march);
            }

            s_fog.march = CreateTargetTexture(lowWidth, lowHeight, s_fog.marchFormat, "VolFog march");
            s_fog.marchWidth = lowWidth;
            s_fog.marchHeight = lowHeight;

            char line[160];
            std::snprintf(line, sizeof(line), "wolVolFog: march target %ux%u %s", lowWidth, lowHeight, s_fog.marchFormat == GxTex_Abgr16F ? "ABGR16F" : "ARGB8");
            Say(line); // Say takes a const char*: Log("%s", char[]) would pick the va_list overload of CGxDevice::Log
        }

        if (!s_fog.scene || s_fog.sceneWidth != sceneWidth || s_fog.sceneHeight != sceneHeight) {
            if (s_fog.scene) {
                GxTexDestroy(s_fog.scene);
            }

            s_fog.scene = CreateTargetTexture(sceneWidth, sceneHeight, GxTex_Argb8888, "VolFog scene");
            s_fog.sceneWidth = sceneWidth;
            s_fog.sceneHeight = sceneHeight;

            char line[160];
            std::snprintf(line, sizeof(line), "wolVolFog: scene copy %ux%u ARGB8", sceneWidth, sceneHeight);
            Say(line);
        }

        if (wantHistory && (!s_fog.history[0] || !s_fog.history[1] || !s_fog.historyDepth || s_fog.historyWidth != lowWidth || s_fog.historyHeight != lowHeight || s_fog.historyFormat != s_fog.marchFormat)) {
            // Two history targets in the format of the march (ping-pong) and the packed depth of the frame they describe: RGB = (high byte,
            // low byte, depth class) of vf_history_depth.hlsl, so an 8 bit target (R32F would not hold it).
            DestroyHistory();
            historyReset = true;

            s_fog.history[0] = CreateTargetTexture(lowWidth, lowHeight, s_fog.marchFormat, "VolFog history 0");
            s_fog.history[1] = CreateTargetTexture(lowWidth, lowHeight, s_fog.marchFormat, "VolFog history 1");
            s_fog.historyDepth = CreateTargetTexture(lowWidth, lowHeight, GxTex_Argb8888, "VolFog history depth");
            s_fog.historyWidth = lowWidth;
            s_fog.historyHeight = lowHeight;
            s_fog.historyFormat = s_fog.marchFormat;

            char line[160];
            std::snprintf(line, sizeof(line), "wolVolFog: temporal history %ux%u %s x2 + depth ARGB8", lowWidth, lowHeight, s_fog.marchFormat == GxTex_Abgr16F ? "ABGR16F" : "ARGB8");
            Say(line);
        }

        return s_fog.march && s_fog.scene;
    }

    bool HistoryTargetsExist() {
        return s_fog.history[0] && s_fog.history[1] && s_fog.historyDepth;
    }

    // Brief B9: the two ping-pong targets of the light shafts (ARGB8, LINEAR: the blur and the blend read them with bilinear filtering, as coa-vfog does).
    // Called only on frames that draw shafts. EnsureTargets has already dropped the old pointers if the device changed.
    bool EnsureRayTargets(uint32_t width, uint32_t height) {
        if (s_fog.rays[0] && s_fog.rays[1] && s_fog.rayWidth == width && s_fog.rayHeight == height) {
            return true;
        }

        for (int i = 0; i < 2; i++) {
            if (s_fog.rays[i]) {
                GxTexDestroy(s_fog.rays[i]);
                s_fog.rays[i] = nullptr;
            }
        }

        s_fog.rays[0] = CreateTargetTexture(width, height, GxTex_Argb8888, "VolFog rays 0", GxTex_Linear);
        s_fog.rays[1] = CreateTargetTexture(width, height, GxTex_Argb8888, "VolFog rays 1", GxTex_Linear);
        s_fog.rayWidth = width;
        s_fog.rayHeight = height;

        char line[160];
        std::snprintf(line, sizeof(line), "wolVolFog: light shaft targets %ux%u ARGB8 x2", width, height);
        Say(line);

        return s_fog.rays[0] && s_fog.rays[1];
    }

    bool Normalize3(float* v) {
        float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

        if (!(length > 1e-6f)) {
            return false;
        }

        v[0] /= length;
        v[1] /= length;
        v[2] /= length;

        return true;
    }

    // The constants of one frame (Renderer::Draw of renderer.cpp, without the temporal filter, the local lights and the beams).
    // B7: `temporal` = the temporal filter runs this frame (step jitter on, history blended); `previousUsable` = the last frame wrote a history that
    // is still around (no reset, no resize, no failure in between).
    bool PackFrame(const VolFog::FrameInputs& in, const VolFog::FogParams& fog, uint32_t scale, uint32_t lowWidth, uint32_t lowHeight, float debugView, bool temporal, bool previousUsable, FogFrame& f) {
        std::memset(&f, 0, sizeof(f));

        const float* proj = in.glProjection;

        // MapWorldDepth: the world is drawn in the viewport depth range 0..0.94 (minZ..maxZ of the frame).
        const bool usableRange = in.viewport.maxZ - in.viewport.minZ > 0.01f && in.viewport.minZ >= 0.0f && in.viewport.maxZ <= 1.0f;
        const float worldMinZ = usableRange ? in.viewport.minZ : 0.0f;
        const float worldExtent = usableRange ? in.viewport.maxZ - in.viewport.minZ : 1.0f;
        const float worldMaxZ = worldMinZ + worldExtent;
        const float depthAtInfinity = worldMinZ + worldExtent * (1.0f + proj[10]) * 0.5f;
        const float depthPerInverseViewZ = worldExtent * proj[14] * 0.5f;
        const float deepestWorldDepth = worldMaxZ >= 0.9999995f ? 0.9999995f : worldMaxZ + 2.0e-6f;

        // WorldViewFromCameraRelative: the view matrix has the eye at the origin; the translation of the inverse is the camera position.
        float viewToWorld[16];

        if (!VolFog::Invert4x4(in.cameraRelativeView, viewToWorld)) {
            return false;
        }

        viewToWorld[12] = in.camPos[0];
        viewToWorld[13] = in.camPos[1];
        viewToWorld[14] = in.camPos[2];

        // WorldViewFromCameraRelative: the view with the camera position in it (the inverse of the above); also the "previous view" of the next frame.
        if (!VolFog::Invert4x4(viewToWorld, f.worldToView)) {
            return false;
        }

        float depthTexelX = VolFog::DepthWidth() ? 1.0f / static_cast<float>(VolFog::DepthWidth()) : 0.0f;
        float depthTexelY = VolFog::DepthHeight() ? 1.0f / static_cast<float>(VolFog::DepthHeight()) : 0.0f;

        float c0[4] = { static_cast<float>(in.viewport.x), static_cast<float>(in.viewport.y), static_cast<float>(in.viewport.width), static_cast<float>(in.viewport.height) };
        float c1[4] = { static_cast<float>(scale), static_cast<float>(s_fog.frame % 1024u), depthTexelX, depthTexelY };
        float c2[4] = { proj[0], proj[5], proj[8], proj[9] };
        float c3[4] = { depthAtInfinity, depthPerInverseViewZ, fog.maxDistance, deepestWorldDepth };
        float c8[4] = { static_cast<float>(lowWidth), static_cast<float>(lowHeight), 1.0f / static_cast<float>(lowWidth), 1.0f / static_cast<float>(lowHeight) };
        std::memcpy(f.common[0], c0, sizeof(c0));
        std::memcpy(f.common[1], c1, sizeof(c1));
        std::memcpy(f.common[2], c2, sizeof(c2));
        std::memcpy(f.common[3], c3, sizeof(c3));
        std::memcpy(f.common[4], viewToWorld, sizeof(viewToWorld)); // c4-c7, row major, camera in row 3
        std::memcpy(f.common[8], c8, sizeof(c8));

        float toLightInView[3];
        VolFog::TransformDirection(in.toLight, in.cameraRelativeView, toLightInView);

        if (!Normalize3(toLightInView)) {
            return false;
        }

        f.light[0] = toLightInView[0];
        f.light[1] = toLightInView[1];
        f.light[2] = toLightInView[2];
        f.light[3] = fog.lightAboveHorizon;

        // cMarch.x: step jitter on (frac(interleaved gradient noise + frame * golden ratio), frame = cPixelGrid.y) only when the temporal filter
        // is going to average it away; 0 = fixed 0.5 in the shader.
        f.march[0] = temporal ? 1.0f : 0.0f;
        f.march[1] = fog.maxDistance;
        f.march[2] = fog.horizonStart;
        f.march[3] = fog.farClip;

        static_assert(sizeof(VolFog::FogLayer) * VolFog::kFogLayers == sizeof(f.layers), "four FogLayer of six registers");
        std::memcpy(f.layers, &fog.layers[0], sizeof(f.layers));

        // cLocalLightControl: x (lights) and y (reach limit) are set by RenderFog once the lights are chosen; phase 0.3 as the default of coa-vfog.
        f.localControl[2] = VolFog::kLocalLightPhase;

        // cDensityVariation: amount 0.15, scale 0.025, wind offset (0.5 yards per second around the 32 / scale period of the noise).
        const bool hasNoise = VolFog::DensityNoiseTex() != nullptr;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_clockStart).count();
        const double windPeriod = 32.0 / 0.025;
        f.variation[0] = hasNoise ? 0.15f : 0.0f;
        f.variation[1] = 0.025f;
        f.variation[2] = static_cast<float>(std::fmod(seconds * 0.5, windPeriod));

        // Temporal filter (B7). The history of the last frame is reused only if that frame drew with the same lens, viewport, scale, quality, density
        // and map, no more than 0.25 s ago, the camera did not jump (30 yards) or turn (45 degrees) in between: Renderer::Draw of renderer.cpp.
        // Otherwise cTemporal.y = 0 and the shader uses only the current march (weight 0 for this frame).
        const VolFog::SettingValues& settings = VolFog::Settings();
        bool historyValid = false;

        if (temporal && previousUsable) {
            const float dx = in.camPos[0] - s_fog.prevCam[0];
            const float dy = in.camPos[1] - s_fog.prevCam[1];
            const float dz = in.camPos[2] - s_fog.prevCam[2];
            const float forwardDot = f.worldToView[2] * s_fog.prevWorldToView[2] + f.worldToView[6] * s_fog.prevWorldToView[6] + f.worldToView[10] * s_fog.prevWorldToView[10];

            historyValid = s_fog.prevScale == scale
                && s_fog.prevQuality == settings.quality
                && s_fog.prevDensity == settings.density
                && s_fog.prevMap == in.mapId
                && forwardDot > kHistoryMinForwardDot
                && std::memcmp(s_fog.prevProj, proj, sizeof(s_fog.prevProj)) == 0
                && s_fog.prevViewport[0] == in.viewport.x && s_fog.prevViewport[1] == in.viewport.y
                && s_fog.prevViewport[2] == in.viewport.width && s_fog.prevViewport[3] == in.viewport.height
                && s_fog.prevDepthRange[0] == in.viewport.minZ && s_fog.prevDepthRange[1] == in.viewport.maxZ
                && seconds - s_fog.prevSeconds < kHistoryMaxSeconds
                && dx * dx + dy * dy + dz * dz < kHistoryMaxMove * kHistoryMaxMove;
        }

        // cViewToPreviousClip = viewToWorld * previousWorldToView * previousProjection (row vectors): a point in the current view space to the clip
        // space of the previous frame; w is its view depth there.
        float previousViewProjection[16];
        VolFog::Mul4x4(s_fog.prevWorldToView, s_fog.prevProj, previousViewProjection);
        VolFog::Mul4x4(viewToWorld, previousViewProjection, f.viewToPreviousClip);

        // cTemporal: history weight, history valid, adaptive lighting (the density noise moves with the wind: a large change relative to the
        // brightness lowers the weight, so the history does not smear it), unused.
        f.temporal[0] = kTemporalWeight;
        f.temporal[1] = historyValid ? 1.0f : 0.0f;
        f.temporal[2] = hasNoise ? 1.0f : 0.0f;
        f.temporal[3] = 0.0f;

        // cComposite: exposure 1, no beams, debug view (0 = none, 2 = transmittance), blend 1 = linear light over the scene copy.
        f.composite[0][0] = 1.0f;
        f.composite[0][1] = 0.0f;
        f.composite[0][2] = debugView;
        f.composite[0][3] = 1.0f;
        f.composite[1][0] = fog.rayColor[0];
        f.composite[1][1] = fog.rayColor[1];
        f.composite[1][2] = fog.rayColor[2];
        // cSun: no sun marker; cSun.w = the glow of the client to compensate (B10): FogSceneColour / BlendGodRays undo the screen glow on the fogged pixels.
        // clientGlowAmount is 0 unless ffxGlow is on and the glow is the active screen effect (VolFogWorld.cpp), so with ffxGlow 0 nothing changes.
        f.composite[2][3] = in.clientGlowAmount;

        return true;
    }

    // Brief B9: the constants of the shaft passes (DrawGodRayMask and DrawGodRaysOverScene of renderer.cpp). The direction to the body and its position
    // on the screen come from VolFog::ComputeRayFrame; the colour is the one of the fog model (the sun colour, or the direct light colour of the moon).
    void PackRays(const VolFog::FrameInputs& in, const VolFog::FogParams& fog, const VolFog::RayFrame& r, uint32_t rayWidth, uint32_t rayHeight, FogFrame& f) {
        const float invWidth = 1.0f / static_cast<float>(rayWidth);
        const float invHeight = 1.0f / static_cast<float>(rayHeight);

        const float mask[2][4] = {
            { r.toLightInView[0], r.toLightInView[1], r.toLightInView[2], kRayFalloff },
            { 0.0f, kRayThreshold, invWidth, invHeight },
        };
        std::memcpy(f.rayMask, mask, sizeof(mask));

        float norm = 0.0f;

        for (int k = 0; k < kRayTaps; ++k) {
            norm += std::pow(kRayDecay, static_cast<float>(k));
        }

        const float sunU = (r.sunPx[0] - static_cast<float>(in.viewport.x)) / static_cast<float>(in.viewport.width);
        const float sunV = (r.sunPx[1] - static_cast<float>(in.viewport.y)) / static_cast<float>(in.viewport.height);
        float step = kRayStep;

        for (int pass = 0; pass < 2; ++pass) {
            const float blur[2][4] = {
                { sunU, sunV, step, 1.0f / norm },
                { static_cast<float>(rayWidth), static_cast<float>(rayHeight), invWidth, invHeight },
            };
            std::memcpy(f.rayBlur[pass], blur, sizeof(blur));
            step /= static_cast<float>(kRayTaps);
        }

        // cComposite.y = strength (cComposite.x, z, w are unused by the blend), cGodRayColour, cSun.xy = the body in pixels, cSun.w = glow compensation (B10).
        f.rayComposite[0][1] = r.strength;
        f.rayComposite[1][0] = fog.rayColor[0];
        f.rayComposite[1][1] = fog.rayColor[1];
        f.rayComposite[1][2] = fog.rayColor[2];
        f.rayComposite[2][0] = r.sunPx[0];
        f.rayComposite[2][1] = r.sunPx[1];
        f.rayComposite[2][3] = in.clientGlowAmount; // B10: BlendGodRays compensates the glow of the client as the fog composition does
    }

    // Sub-pass 4 (B9): the world viewport of the back buffer as it is before the fog, reduced to rays[0] with a LINEAR stretch (StretchRect, as
    // CopySceneForGodRays), the mask into rays[1], and the two radial blurs (rays[1] -> rays[0] -> rays[1]). False if a target or a pass failed.
    bool DrawRays(const FogFrame& f, CGxTex* depth, const VolFog::Viewport& vp, uint32_t rayWidth, uint32_t rayHeight) {
        CiRect destination;
        destination.minX = 0;
        destination.minY = 0;
        destination.maxX = static_cast<int32_t>(rayWidth);
        destination.maxY = static_cast<int32_t>(rayHeight);

        CiRect source;
        source.minX = static_cast<int32_t>(vp.x);
        source.minY = static_cast<int32_t>(vp.y);
        source.maxX = static_cast<int32_t>(vp.x + vp.width);
        source.maxY = static_cast<int32_t>(vp.y + vp.height);

        return GxTexStretch(s_fog.rays[0], &destination, &source, 0, 0) != 0
            && s_fog.rayMaskPass->DrawRayMask(f, depth, s_fog.rays[0], s_fog.rays[1])
            && s_fog.rayBlurPass->DrawRayBlur(f.rayBlur[0], s_fog.rays[1], s_fog.rays[0])
            && s_fog.rayBlurPass->DrawRayBlur(f.rayBlur[1], s_fog.rays[0], s_fog.rays[1]);
    }

    enum FogResult { FogOk, FogTransient, FogFailed };

    // FogOk: the frame drew (or legitimately had nothing to draw). FogTransient: nothing could be drawn this frame (no depth yet, no camera): Armed() is
    // false for it, but it does not count towards giving the pass up. FogFailed: a real failure. `liquid` reports the camera.
    // `debugMode`: 0 normal; 2 the raw march buffer on screen; 3 the filtered history on screen; 4 normal image without the temporal filter (B7).
    FogResult RenderFog(int32_t debugMode, bool& liquid) {
        liquid = false;
        s_fog.raysDrawn = false;

        // Brief B7: the history is only good for the very next frame, and only if that one drew everything. It becomes valid again at the end of
        // a frame that drew with the filter; every early return below (no depth, no camera, underwater, failure) leaves it invalid.
        const bool previousUsable = s_fog.historyValid;
        s_fog.historyValid = false;

        if (!VolFog::DepthTex()) {
            // wolVolFog is on but there is no readable depth yet (waiting for gxRestart): the callback of the CVar already said so.
            return FogTransient;
        }

        if (VolFog::s_caps.ps30Slots < VolFog::kMinPs30Slots || !g_theGxDevicePtr->Caps().m_texTarget[GxTex_NonPow2]) {
            if (!s_fog.hardMessage) {
                s_fog.hardMessage = true;
                Say("wolVolFog: this video card cannot run the fog (it needs a ps_3_0 with at least 2048 instruction slots, the composition shader takes up to 1628, and non power of two textures); the fog stays off.");
            }

            VolFog::SetHardFailure(true);

            return FogFailed;
        }

        if (!s_fog.marchPass) {
            // Created on first use, kept for the whole run.
            s_fog.marchPass = new FogPass("WolVolFogMarch");
            s_fog.compositePass = new FogPass("WolVolFogComposite");
            s_fog.temporalPass = new FogPass("WolVolFogTemporal", 1);
            s_fog.historyDepthPass = new FogPass("WolVolFogHistoryDepth", 1);
        }

        const VolFog::SettingValues& settings = VolFog::Settings();
        const int32_t quality = std::clamp(settings.quality, 1, 3);
        const uint32_t basePermutation = static_cast<uint32_t>((quality - 1) * 2); // + 1 with local lights (B8)

        s_fog.marchPass->SetPixelPermutation(basePermutation);
        s_fog.compositePass->SetPixelPermutation(basePermutation);

        if (!s_fog.marchPass->Valid() || !s_fog.compositePass->Valid()) {
            if (!s_fog.shadersMessage) {
                s_fog.shadersMessage = true;
                Say("wolVolFog: the shaders WolVolFogQuad / WolVolFogMarch / WolVolFogComposite are not valid (is Data\\patch-Z.MPQ present, and does the video card support ps_3_0?); the fog stays off.");
            }

            VolFog::SetHardFailure(true);

            return FogFailed;
        }

        VolFog::FrameInputs in;

        if (!VolFog::CollectInputs(in)) {
            return FogTransient;
        }

        liquid = in.inLiquid;

        VolFog::FogParams fog = VolFog::BuildFogParams(in, VolFog::CurrentTuning());

        if (!fog.active || in.viewport.width < 8 || in.viewport.height < 8) {
            return FogOk; // underwater: the client keeps its own fog (Armed() is false there); nothing to draw
        }

        // Brief B8: capture the point lights of the world scene, stabilise them and upload the chosen ones (s8); the lit permutations are used while
        // there is at least one. If the lit shaders are not valid the frame goes on with the unlit ones.
        const double lightSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_clockStart).count();
        // Brief B10: the lights only run from quality kMinLocalLightQuality on (wolVolFog 1 is the cheap setting).
        uint32_t lightCount = VolFog::PrepareLights(in, s_fog.frame, lightSeconds, VolFog::Settings().localLights && quality >= VolFog::kMinLocalLightQuality);
        CGxTex* lightTex = lightCount > 0 ? VolFog::LightTex() : nullptr;
        uint32_t permutation = basePermutation;

        if (lightTex) {
            s_fog.marchPass->SetPixelPermutation(basePermutation + 1);
            s_fog.compositePass->SetPixelPermutation(basePermutation + 1);

            if (s_fog.marchPass->Valid() && s_fog.compositePass->Valid()) {
                permutation = basePermutation + 1;
            } else {
                if (!s_fog.litMessage) {
                    s_fog.litMessage = true;
                    Say("wolVolFog: the local light variants of WolVolFogMarch / WolVolFogComposite are not valid; the fog runs without local lights.");
                }

                lightTex = nullptr;
            }
        }

        if (!lightTex) {
            lightCount = 0;
        }

        // Brief B10 (REVISION-B7-B8, finding 3a): the history of the unlit march and the lit one are not the same image, and the old light texture is
        // gone after a Reset: the filter starts again when the variant changes or the texture is made anew. wolVolFogGodRays needs nothing here: the
        // shafts are blended in their own pass after the composition (cComposite.y = 0 in the fog composition), so the history never holds them.
        const bool lit = lightCount > 0;
        const uint32_t lightGeneration = VolFog::LightTextureGeneration();
        const bool litChanged = lit != s_fog.prevLit || (lit && lightGeneration != s_fog.prevLightGeneration);

        const uint32_t scale = quality == 1 ? 4u : 2u;
        const uint32_t lowWidth = std::max((in.viewport.width + scale - 1) / scale, 8u);
        const uint32_t lowHeight = std::max((in.viewport.height + scale - 1) / scale, 8u);

        // Temporal filter (B7): on unless wolVolFogDebug 4 asks for the plain B6 image or the shaders / targets of the filter are not available.
        // Without the filter the march uses a fixed step (no jitter) and the composite reads the march itself.
        bool temporal = debugMode != 4 && !s_fog.temporalOff;

        if (temporal && (!s_fog.temporalPass->Valid() || !s_fog.historyDepthPass->Valid())) {
            Say("wolVolFog: the shaders WolVolFogTemporal / WolVolFogHistoryDepth are not valid (is Data\\patch-Z.MPQ up to date?); the fog runs without the temporal filter.");
            s_fog.temporalOff = true;
            temporal = false;
        }

        bool historyReset = false;

        if (!EnsureTargets(lowWidth, lowHeight, in.viewport.width, in.viewport.height, temporal, historyReset)) {
            return FogFailed;
        }

        if (temporal && !HistoryTargetsExist()) {
            Say("wolVolFog: the temporal history targets could not be created; the fog runs without the temporal filter.");
            s_fog.temporalOff = true;
            temporal = false;
        }

        // Brief B9: light shafts. They are drawn when wolVolFogGodRays and the visibility of the sun or moon give a strength over kMinGodRayStrength, the
        // body is in front of the camera and not too far outside the viewport (VolFog::ComputeRayFrame); wolVolFogDebug 6 computes them whatever the
        // strength is, to show the texture. Not in the transmittance views (2, 3), which show other things.
        const VolFog::RayFrame rayFrame = VolFog::ComputeRayFrame(in, fog, settings.godRays);
        const bool rayDebug = debugMode == 6;
        const bool rayModes = debugMode == 0 || debugMode == 4 || rayDebug;
        const uint32_t rayWidth = std::max(in.viewport.width / kRayScale, 1u);
        const uint32_t rayHeight = std::max(in.viewport.height / kRayScale, 1u);
        bool wantRays = rayModes && !s_fog.raysOff && rayFrame.inFront && rayFrame.screenFade > 0.0f && (rayFrame.strength > VolFog::kMinGodRayStrength || rayDebug);

        if (wantRays) {
            if (!s_fog.rayMaskPass) {
                s_fog.rayMaskPass = new FogPass("WolVolFogRayMask", 1);
                s_fog.rayBlurPass = new FogPass("WolVolFogRayBlur", 1);
                s_fog.rayCompositePass = new FogPass("WolVolFogRayComposite", 1);
            }

            if (!s_fog.rayMaskPass->Valid() || !s_fog.rayBlurPass->Valid() || !s_fog.rayCompositePass->Valid()) {
                Say("wolVolFog: the shaders WolVolFogRayMask / WolVolFogRayBlur / WolVolFogRayComposite are not valid (is Data\\patch-Z.MPQ up to date?); the light shafts stay off.");
                s_fog.raysOff = true;
                wantRays = false;
            } else if (!EnsureRayTargets(rayWidth, rayHeight)) {
                Say("wolVolFog: the light shaft targets could not be created; the light shafts stay off.");
                s_fog.raysOff = true;
                wantRays = false;
            }
        }

        FogFrame frame;

        // debug view in the composite: 2 = transmittance of the fog it is given (debug 2: the march, debug 3: the filtered history)
        const float debugView = (debugMode == 2 || debugMode == 3) ? 2.0f : 0.0f;

        if (!PackFrame(in, fog, scale, lowWidth, lowHeight, debugView, temporal, previousUsable && !historyReset && !litChanged, frame)) {
            return FogTransient;
        }

        frame.localControl[0] = static_cast<float>(lightCount);
        frame.localControl[1] = lightCount > 0 ? VolFog::LightReachLimit() : 0.0f;

        if (wantRays) {
            PackRays(in, fog, rayFrame, rayWidth, rayHeight, frame);
        }

        // The constants the fog overwrites belong to whoever draws next: put them back as they were.
        float saved[kMarchConstants * 4];
        std::memcpy(saved, CGxDevice::s_shadowConstants[0].constants, sizeof(saved));

        CGxTex* depth = VolFog::DepthTex();
        bool drawn = s_fog.marchPass->DrawMarch(frame, permutation, depth, VolFog::DensityNoiseTex(), lightTex, s_fog.march);

        if (!drawn && s_fog.marchFormat == GxTex_Abgr16F) {
            // The device would not make a half float target: ARGB8 like coa-vfog does, from the next frame on.
            Say("wolVolFog: the ABGR16F march target could not be created; falling back to ARGB8.");
            s_fog.marchFormat = GxTex_Argb8888;
            GxTexDestroy(s_fog.march);
            s_fog.march = nullptr;
            s_fog.marchWidth = 0;
        }

        CGxTex* fogTex = s_fog.march; // what the composite reads as sFog

        if (drawn && temporal) {
            // Sub-pass 2: filter the march with the reprojected history into the other history target, then pack this frame's depth for the next one.
            const uint32_t write = s_fog.historyIndex ^ 1u;
            drawn = s_fog.temporalPass->DrawTemporal(frame, s_fog.march, s_fog.history[s_fog.historyIndex], depth, s_fog.historyDepth, s_fog.history[write]);

            if (drawn) {
                drawn = s_fog.historyDepthPass->DrawHistoryDepth(frame, depth, s_fog.historyDepth);
            }

            if (drawn) {
                s_fog.historyIndex = write;
                fogTex = s_fog.history[write];
            } else {
                // The device would not make the history targets (they were created, but not on the device): run without the filter from the next frame on.
                Say("wolVolFog: the temporal history targets could not be created on the device; the fog runs without the temporal filter.");
                s_fog.temporalOff = true;
            }
        }

        if (debugMode == 2) {
            fogTex = s_fog.march; // wolVolFogDebug 2 keeps showing the raw march
        }

        // Sub-pass 4 (B9): the shafts, from the scene as it is before the fog; the stretch reads the back buffer, which the composite has not touched yet.
        bool raysReady = false;

        if (drawn && wantRays) {
            raysReady = DrawRays(frame, depth, in.viewport, rayWidth, rayHeight);

            if (!raysReady) {
                Say("wolVolFog: the light shaft passes could not run (target not created on the device); the light shafts stay off.");
                s_fog.raysOff = true;
            }
        }

        if (drawn) {
            // Sub-pass 3: the scene as it is before the fog, the world viewport of the back buffer into a texture of exactly that size.
            C2iVector destination = { 0, 0 };
            C2iVector source = { static_cast<int32_t>(in.viewport.x), static_cast<int32_t>(in.viewport.y) };
            C2iVector size = { static_cast<int32_t>(in.viewport.width), static_cast<int32_t>(in.viewport.height) };
            drawn = GxTexCopy(s_fog.scene, destination, source, size, 0, 0) != 0;

            if (drawn) {
                s_fog.compositePass->DrawComposite(frame, permutation, depth, fogTex, s_fog.march, s_fog.scene, lightTex);

                // Sub-pass 6b (B9): copy the fogged image once more and blend the shafts over it (the late path of coa-vfog). wolVolFogDebug 6 shows
                // the shaft texture instead, drawn by VolFog::Render() over this.
                if (raysReady && !rayDebug && rayFrame.strength > VolFog::kMinGodRayStrength) {
                    if (GxTexCopy(s_fog.scene, destination, source, size, 0, 0) != 0) {
                        s_fog.rayCompositePass->DrawRayComposite(frame, s_fog.scene, s_fog.rays[1]);
                    }
                }
            }
        }

        s_fog.raysDrawn = raysReady;

        g_theGxDevicePtr->ShaderConstantsSet(GxSh_Pixel, 0, saved, kMarchConstants);

        if (drawn) {
            s_fog.frame++;

            // What the next frame's reprojection needs: this frame's view, projection, position, viewport and settings.
            const VolFog::SettingValues& settings = VolFog::Settings();
            std::memcpy(s_fog.prevWorldToView, frame.worldToView, sizeof(s_fog.prevWorldToView));
            std::memcpy(s_fog.prevProj, in.glProjection, sizeof(s_fog.prevProj));
            std::memcpy(s_fog.prevCam, in.camPos, sizeof(s_fog.prevCam));
            s_fog.prevViewport[0] = in.viewport.x;
            s_fog.prevViewport[1] = in.viewport.y;
            s_fog.prevViewport[2] = in.viewport.width;
            s_fog.prevViewport[3] = in.viewport.height;
            s_fog.prevDepthRange[0] = in.viewport.minZ;
            s_fog.prevDepthRange[1] = in.viewport.maxZ;
            s_fog.prevScale = scale;
            s_fog.prevQuality = settings.quality;
            s_fog.prevDensity = settings.density;
            s_fog.prevMap = in.mapId;
            s_fog.prevSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_clockStart).count();
            s_fog.historyValid = temporal;
            s_fog.prevLit = lit;
            s_fog.prevLightGeneration = lightGeneration;
        }

        return drawn ? FogOk : FogFailed;
    }

    // wolVolFogDebug 1 (depth) and 3 (test grid): the debug shader of brief B2.
    void RenderDebugView(int32_t mode) {
        if (mode == 1 && !VolFog::DepthTex()) {
            // No readable depth (wolVolFog 0, no INTZ, or the device has not been rebuilt yet): say it once and draw nothing.
            if (!VolFog::s_noDepthWarned) {
                VolFog::s_noDepthWarned = true;
                Say("wolVolFogDebug 1: there is no readable INTZ depth (set wolVolFog 1-3 and run gxRestart; the video card must report INTZ); nothing is drawn.");
            }

            return;
        }

        if (!s_debugPass) {
            // Created on first use and kept for the whole run; the shaders belong to the device and go with it.
            s_debugPass = new DebugPass();
        }

        // Brief B7: the grid moved from 3 to 5 (3 is now the filtered history); the permutations of WolVolFogDebug are still 0 depth, 1 tint, 2 grid.
        s_debugPass->SetPixelPermutation(mode == 5 ? 2u : mode == 6 ? 3u : static_cast<uint32_t>(mode - 1));
        s_debugPass->m_raysTexture = mode == 6 ? s_fog.rays[1] : nullptr; // brief B9: the shaft texture, black when the frame drew no shafts
        s_debugPass->m_raysGain = mode == 6 && s_fog.raysDrawn ? 1.0f : 0.0f;

        if (!s_debugPass->Valid()) {
            // Failure policy (design 1.3): say it once, draw nothing, touch nothing else.
            if (!s_failureLogged) {
                s_failureLogged = true;
                Say("wolVolFog: the shaders WolVolFogQuad / WolVolFogDebug are not valid (is Data\\patch-Z.MPQ present, and does the video card support ps_3_0?); wolVolFogDebug does nothing.");
            }

            return;
        }

        CRect window = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_theGxDevicePtr->CapsWindowSize(window);

        float width = window.maxX - window.minX;
        float height = window.maxY - window.minY;

        if (width <= 0.0f || height <= 0.0f) {
            return;
        }

        // The world viewport that CGWorldFrame::OnWorldRender left set, in pixels from the top-left corner of the back
        // buffer (Gx viewport y grows upwards; D3D9 viewport y is (1 - maxY) * height, CGxDeviceD3d::IXformSetViewport).
        float minX = 0.0f;
        float maxX = 0.0f;
        float minY = 0.0f;
        float maxY = 0.0f;
        float minZ = 0.0f;
        float maxZ = 0.0f;
        GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);

        s_debugPass->SetViewport(minX * width, (1.0f - maxY) * height, (maxX - minX) * width, (maxY - minY) * height, width, height);
        s_debugPass->Execute();
    }
}

void VolFog::Render() {
    // Brief B5: a pending wolVolFogDump runs here, inside the world frame (it only reads and prints).
    VolFog::RunPendingDump();

    // A device rebuild (gxRestart, resize, reset) or a change of wolVolFog gives the pass a clean slate: a failure for good (shaders, caps,
    // targets) is only for as long as the same device and the same setting.
    if (s_fog.epoch != VolFog::DeviceEpoch() || s_fog.seenMode != VolFog::s_mode) {
        s_fog.epoch = VolFog::DeviceEpoch();
        s_fog.seenMode = VolFog::s_mode;
        s_fog.consecutiveFailures = 0;
        s_fog.shadersMessage = false;
        s_fog.hardMessage = false;
        s_fog.litMessage = false;
        s_fog.temporalOff = false; // B7: the history targets are made again, and the contents of the old ones are gone
        s_fog.raysOff = false; // B9: the shaft shaders and targets are tried again
        s_fog.historyValid = false;
        VolFog::SetHardFailure(false);
        VolFog::SetFrameResult(true, false);
    }

    // Brief B13: in the MSAA + copy mode the INTZ is refreshed from the multisampled depth here, once per frame and before any sub-pass (or
    // wolVolFogDebug 1) reads it. Nothing happens in the other modes.
    if (VolFog::s_mode > 0 && !VolFog::HardFailure()) {
        VolFog::ResolveDepth();
    }

    int32_t debugMode = VolFog::s_debugMode;

    if (debugMode == 1 || debugMode == 5) {
        RenderDebugView(debugMode);

        return;
    }

    if (VolFog::s_mode <= 0) {
        if (((debugMode >= 2 && debugMode <= 4) || debugMode == 6) && !s_fog.noDepthDebugWarned) {
            s_fog.noDepthDebugWarned = true;
            Say("wolVolFogDebug 2-4 and 6 show the volumetric fog (2 march buffer, 3 filtered history, 4 no temporal filter, 6 light shafts): set wolVolFog 1-3 first (and gxRestart).");
        }

        return;
    }

    if (VolFog::HardFailure()) {
        return;
    }

    bool liquid = false;
    FogResult result = RenderFog(debugMode, liquid);

    if (debugMode == 6 && result == FogOk) {
        RenderDebugView(6); // brief B9: the shaft texture over the world viewport (black if this frame drew none)
    }

    if (result == FogOk) {
        s_fog.consecutiveFailures = 0;
    } else if (result == FogFailed && !VolFog::HardFailure() && ++s_fog.consecutiveFailures >= 3) {
        // Three frames in a row failing for real: give up until the setting or the device changes.
        Say("wolVolFog: the fog pass failed three frames in a row; the client's own fog is back.");
        VolFog::SetHardFailure(true);
    }

    VolFog::SetFrameResult(result == FogOk, liquid);
}

bool VolFog::RaysDrawn() {
    return s_fog.raysDrawn;
}

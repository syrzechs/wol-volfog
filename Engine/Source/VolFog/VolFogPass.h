#ifndef ENGINE_SOURCE_VOLFOG_VOLFOGPASS_H
#define ENGINE_SOURCE_VOLFOG_VOLFOGPASS_H

#include <cstddef>
#include <cstdint>
#include "Engine/Source/FFX/FFX.h"

class CGxShader;

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B2.
// Licensed under GPL-3.0-only (the module combines the FFX::Pass plumbing of the client with the shader contract of
// coa-vfog 205ade5; see Shaders/VolFog/NOTICE-modificaciones.txt).

namespace VolFog {
    // Base of every sub-pass of the fog (design 1.1): an FFX::Pass that owns a vs_3_0 / ps_3_0 pair loaded by name from
    // Data\patch-Z.MPQ (Shaders\Vertex\vs_3_0\<vs>.bls, Shaders\Pixel\ps_3_0\<ps>.bls) and draws one full-screen quad
    // over the world viewport with the FFX quad index buffer. The vertex shader is vf_fullscreen_vs.hlsl, which returns
    // its position untouched, so the quad is built in clip space (-1..1) instead of the pixel space of FFX::SetupGeo_Quad.
    class Pass : public FFX::Pass {
        public:
        static const uint32_t kMaxPermutations = 6;

        ~Pass() override;
        bool Valid() override;

        // Permutation of the pixel shader used by the next Execute().
        void SetPixelPermutation(uint32_t index);
        // World viewport in pixels (origin at the top-left of the back buffer) and back buffer size, for the constants.
        void SetViewport(float x, float y, float width, float height, float targetWidth, float targetHeight);

        protected:
        Pass(const char* pixelName, uint32_t pixelPermutations, const char* vertexName, EGxBlend blend);

        // SetupGx equivalent: RsPush, culling / lighting / depth / fog off, blend of the pass, viewport of the world with
        // the full 0..1 depth range. End() undoes it (RsPop and the viewport as Begin() found it).
        void Begin();
        // Binds the shaders of the pass, builds the quad (FFX::SetupVB_PCT + FFX::SetupIB) and draws it.
        void DrawQuad();
        void End();

        CGxShader* m_vertexShader;
        CGxShader* m_pixelShaders[kMaxPermutations];
        uint32_t m_pixelCount;
        uint32_t m_pixelIndex;
        float m_viewportPx[4];
        float m_targetPx[2];

        private:
        float m_savedViewport[6];
    };
}

#endif

#include "Engine/Source/VolFog/VolFogPass.h"
#include <cstring>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B2.
// Licensed under GPL-3.0-only (see Shaders/VolFog/NOTICE-modificaciones.txt).

VolFog::Pass::Pass(const char* pixelName, uint32_t pixelPermutations, const char* vertexName, EGxBlend blend) : FFX::Pass(nullptr, 0, nullptr, blend, false) {
    this->m_vertexShader = nullptr;
    memset(this->m_pixelShaders, 0, sizeof(this->m_pixelShaders));
    this->m_pixelCount = 0;
    this->m_pixelIndex = 0;
    memset(this->m_viewportPx, 0, sizeof(this->m_viewportPx));
    memset(this->m_targetPx, 0, sizeof(this->m_targetPx));
    memset(this->m_savedViewport, 0, sizeof(this->m_savedViewport));

    if (pixelPermutations < 1) {
        pixelPermutations = 1;
    } else if (pixelPermutations > kMaxPermutations) {
        pixelPermutations = kMaxPermutations;
    }

    this->m_pixelCount = pixelPermutations;

    // Same loading as PassBox4: shaders are looked up by name in the MPQ and cached by the device. A missing or invalid
    // .bls leaves the shader without code and Valid() reports it.
    g_theGxDevicePtr->ShaderCreate(&this->m_vertexShader, GxSh_Vertex, "Shaders\\Vertex", vertexName, 1);
    g_theGxDevicePtr->ShaderCreate(this->m_pixelShaders, GxSh_Pixel, "Shaders\\Pixel", pixelName, pixelPermutations);
}

VolFog::Pass::~Pass() {
    if (this->m_vertexShader) {
        g_theGxDevicePtr->ShaderDestroy(this->m_vertexShader);
    }

    for (uint32_t i = 0; i < this->m_pixelCount; i++) {
        if (this->m_pixelShaders[i]) {
            g_theGxDevicePtr->ShaderDestroy(this->m_pixelShaders[i]);
        }
    }
}

bool VolFog::Pass::Valid() {
    CGxShader* vertex = this->m_vertexShader;
    CGxShader* pixel = this->m_pixelIndex < this->m_pixelCount ? this->m_pixelShaders[this->m_pixelIndex] : nullptr;

    return vertex && pixel && vertex->Valid() && pixel->Valid();
}

void VolFog::Pass::SetPixelPermutation(uint32_t index) {
    this->m_pixelIndex = index;
}

void VolFog::Pass::SetViewport(float x, float y, float width, float height, float targetWidth, float targetHeight) {
    this->m_viewportPx[0] = x;
    this->m_viewportPx[1] = y;
    this->m_viewportPx[2] = width;
    this->m_viewportPx[3] = height;
    this->m_targetPx[0] = targetWidth;
    this->m_targetPx[1] = targetHeight;
}

void VolFog::Pass::Begin() {
    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_BlendingMode, static_cast<int32_t>(this->m_blend));

    // The caller (CGWorldFrame::OnWorldRender) leaves the world viewport set: keep its x / y and open the depth range,
    // the world uses 0..0.94 and the quad has z = 0.
    float* saved = this->m_savedViewport;
    GxXformViewport(saved[0], saved[1], saved[2], saved[3], saved[4], saved[5]);
    GxXformSetViewport(saved[0], saved[1], saved[2], saved[3], 0.0f, 1.0f);
}

void VolFog::Pass::DrawQuad() {
    // Clip space quad in the vertex order of FFX::SetupGeo_Quad (bottom-left, top-left, top-right, bottom-right) so
    // that FFX::s_quadIndices draws it as a strip. The colour (shade 1.0 = white) and the texcoords travel in the
    // buffer for the pixel shaders that want them; the vertex shader of the pass only reads the position.
    static const C3Vector s_positions[4] = {
        C3Vector(-1.0f, -1.0f, 0.0f),
        C3Vector(-1.0f, 1.0f, 0.0f),
        C3Vector(1.0f, 1.0f, 0.0f),
        C3Vector(1.0f, -1.0f, 0.0f)
    };
    static const C2Vector s_texCoords[4] = {
        C2Vector(0.0f, 1.0f),
        C2Vector(0.0f, 0.0f),
        C2Vector(1.0f, 0.0f),
        C2Vector(1.0f, 1.0f)
    };
    static const float s_shade[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    FFX::SetupVB_PCT(4, s_positions, s_texCoords, s_shade);
    g_theGxDevicePtr->PrimVertexMask(0x51); // position, colour 0, texcoord 0
    FFX::SetupIB();

    g_theGxDevicePtr->RsSet(GxRs_VertexShader, this->m_vertexShader);
    g_theGxDevicePtr->RsSet(GxRs_PixelShader, this->m_pixelShaders[this->m_pixelIndex]);

    g_theGxDevicePtr->Draw(&FFX::s_quadBatch, 1);
}

void VolFog::Pass::End() {
    g_theGxDevicePtr->RsPop();

    const float* saved = this->m_savedViewport;
    GxXformSetViewport(saved[0], saved[1], saved[2], saved[3], saved[4], saved[5]);
}

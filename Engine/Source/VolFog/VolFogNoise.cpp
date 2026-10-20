#include <cstdint>
#include <cstring>
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/VolFog/VolFog.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B6.
// Licensed under GPL-3.0-only. DensityNoiseValue is derived from CreateDensityNoise in src/noise_volume.cpp of coa-vfog 205ade5 (copyright of
// the authors of CoAVolFog, jealous-sound and the contributors of the Raz0r1337 fork; see Shaders/VolFog/NOTICE-modificaciones.txt).
// Modified 2026-10-20 (brief B6): the hash is the same, but the 32x32x32 volume texture of Direct3D 9 is uploaded as a 2D atlas of 32 slices
// (32 x 1024, A8R8G8B8 grey) through the Gx texture layer, which has no volume textures. Texel (x, y, z) of the volume is texel (x, z * 32 + y)
// of the atlas; vf_density_variation.hlsli does the matching lookup.

namespace {
    constexpr uint32_t kNoiseSize = 32;
    constexpr uint32_t kAtlasHeight = kNoiseSize * kNoiseSize;

    uint32_t s_texels[kNoiseSize * kAtlasHeight]; // A8R8G8B8, one row of 32 texels per atlas row
    bool s_filled;
    CGxTex* s_texture;
    const CGxDevice* s_device;
    bool s_failed;

    // Same hash as DensityNoiseValue of noise_volume.cpp.
    uint8_t DensityNoiseValue(uint32_t x, uint32_t y, uint32_t z) {
        uint32_t value = x + kNoiseSize * (y + kNoiseSize * z);
        value = ((value ^ (value >> 7)) * 4051u + 12743u) & 0x7FFFu;
        value ^= value >> 9;
        value = (value * 109u + 583u) & 0x7FFFu;
        value ^= value >> 6;
        value = (value * 29317u + 9419u) & 0x7FFFu;
        value ^= value >> 7;

        return static_cast<uint8_t>(value >> 7);
    }

    void FillTexels() {
        for (uint32_t z = 0; z < kNoiseSize; z++) {
            for (uint32_t y = 0; y < kNoiseSize; y++) {
                uint32_t* row = &s_texels[(z * kNoiseSize + y) * kNoiseSize];

                for (uint32_t x = 0; x < kNoiseSize; x++) {
                    row[x] = 0xFF000000u | (static_cast<uint32_t>(DensityNoiseValue(x, y, z)) * 0x00010101u); // WriteGreyArgb
                }
            }
        }

        s_filled = true;
    }

    // Supplies the atlas when the device uploads the texture (creation and every device reset); the texture is MANAGED.
    void NoiseTexFunc(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t face, uint32_t level, void* userArg, uint32_t& texelStrideInBytes, const void*& texels) {
        if (command == GxTex_Latch) {
            texelStrideInBytes = kNoiseSize * sizeof(uint32_t);
            texels = s_texels;
        }
    }
}

CGxTex* VolFog::DensityNoiseTex() {
    if (s_texture && s_device != g_theGxDevicePtr) {
        s_texture = nullptr; // the device that owned it is gone
    }

    if (s_texture || s_failed || !g_theGxDevicePtr) {
        return s_texture;
    }

    if (!s_filled) {
        FillTexels();
    }

    // Linear filter, wrap in u, clamp in v (v is kept inside its slice by the shader), no mip maps, plain managed texture.
    CGxTexFlags flags(GxTex_Linear, 1, 0, 0, 0, 0, 1, 0, 0, 0);

    if (!GxTexCreate(kNoiseSize, kAtlasHeight, GxTex_Argb8888, flags, nullptr, NoiseTexFunc, s_texture) || !s_texture) {
        s_texture = nullptr;
        s_failed = true;

        return nullptr;
    }

    s_device = g_theGxDevicePtr;

    return s_texture;
}

#include "Engine/Source/VolFog/VolFog.h"
#include "Engine/Source/VolFog/VolFogLights.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"

#if defined(_WIN32)
#include <d3d9.h>
#endif

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B3: the main depth buffer as a
// readable INTZ texture. Only the D3D9 backends call CreateDepthTexture / ReleaseDepthTexture; the OpenGL stub never does,
// so there DepthTexture() stays nullptr and the pass reports "no depth".

int32_t VolFog::s_mode;
VolFog::MessageHook VolFog::s_messageHook;
bool VolFog::s_noDepthWarned;

namespace {
#if defined(_WIN32)
    IDirect3DTexture9* s_texture;
    IDirect3DSurface9* s_surface;
#endif
    uint32_t s_width;
    uint32_t s_height;
    CGxTex* s_wrapper; // kept for the whole run: the pass binds it, the device never owns it
}

bool VolFog::WantsReadableDepth() {
    return VolFog::s_mode > 0 && VolFog::s_caps.intz;
}

void VolFog::Message(const char* line) {
    if (VolFog::s_messageHook) {
        VolFog::s_messageHook(line);
    }
}

#if defined(_WIN32)

static uint32_t RoundUpToPow2(uint32_t value) {
    uint32_t result = 1;

    while (result < value) {
        result <<= 1;
    }

    return result;
}

bool VolFog::WantsReadableDepthFor(IDirect3D9* d3d, uint32_t backBufferFormat) {
    if (VolFog::s_mode <= 0 || !d3d) {
        return false;
    }

    const D3DFORMAT intz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

    return d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, static_cast<D3DFORMAT>(backBufferFormat), D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, intz) == D3D_OK;
}

bool VolFog::CreateDepthTexture(IDirect3DDevice9* device) {
    VolFog::ReleaseDepthTexture(device);

    if (!device || !VolFog::WantsReadableDepth()) {
        return false;
    }

    IDirect3DSurface9* colour = nullptr;

    if (FAILED(device->GetRenderTarget(0, &colour)) || !colour) {
        return false;
    }

    D3DSURFACE_DESC desc;
    HRESULT result = colour->GetDesc(&desc);
    colour->Release();

    if (FAILED(result)) {
        return false;
    }

    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        // A multisampled back buffer cannot share the depth of a plain texture. This happens on the very first device of a
        // run (the caps are read after the present parameters) and is solved by gxRestart.
        CGxDevice::Log("wolVolFog: the back buffer is multisampled (%d), the readable depth waits for gxRestart", static_cast<int>(desc.MultiSampleType));
        VolFog::Message("wolVolFog: pendiente de gxRestart (el back buffer tiene MSAA)");
        return false;
    }

    const D3DFORMAT intz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    uint32_t width = desc.Width;
    uint32_t height = desc.Height;
    IDirect3DTexture9* texture = nullptr;

    result = device->CreateTexture(width, height, 1, D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT, &texture, nullptr);

    if (FAILED(result)) {
        // Devices with POW2-only textures: a depth bigger than the render target is legal in D3D9.
        uint32_t roundedWidth = RoundUpToPow2(width);
        uint32_t roundedHeight = RoundUpToPow2(height);

        if (roundedWidth != width || roundedHeight != height) {
            result = device->CreateTexture(roundedWidth, roundedHeight, 1, D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT, &texture, nullptr);

            if (SUCCEEDED(result)) {
                width = roundedWidth;
                height = roundedHeight;
            }
        }
    }

    if (FAILED(result) || !texture) {
        CGxDevice::Log("wolVolFog: CreateTexture(%ux%u, INTZ) failed (0x%08X), the original depth buffer stays", width, height, static_cast<unsigned>(result));
        return false;
    }

    IDirect3DSurface9* surface = nullptr;

    if (FAILED(texture->GetSurfaceLevel(0, &surface)) || !surface) {
        texture->Release();
        return false;
    }

    if (FAILED(device->SetDepthStencilSurface(surface))) {
        surface->Release();
        texture->Release();
        CGxDevice::Log("wolVolFog: SetDepthStencilSurface(INTZ) failed, the original depth buffer stays");
        return false;
    }

    s_texture = texture;
    s_surface = surface;
    s_width = width;
    s_height = height;
    VolFog::SetLightDevice(device); // brief B8: the local light texture is made on this device when the first lights show up
    CGxDevice::Log("wolVolFog: INTZ depth texture %ux%u is now the main depth (back buffer %ux%u)", width, height, desc.Width, desc.Height);

    return true;
}

void VolFog::ReleaseDepthTexture(IDirect3DDevice9* device) {
    // Brief B6: the device is being reset or rebuilt; the pass drops its failure state and re-checks its targets (VolFogRender.cpp). The
    // targets and the scene copy are Gx textures (render targets), which the device itself releases and recreates on demand.
    VolFog::NoteDeviceRelease();
    VolFog::ReleaseLightTexture(); // brief B8: the 32 x 1 light texture goes with the depth (a DEFAULT pool texture would make Reset fail)

    if (device && (s_texture || s_surface)) {
        // Brief B3b: the INTZ may still be bound as a texture (the pass leaves s0 on it) and is the depth-stencil of the device. The
        // native D3D9 runtime fails Reset with D3DERR_INVALIDCALL while a D3DPOOL_DEFAULT resource of the application is still alive,
        // and what the device has bound counts as alive. Unbind first, then release; Reset brings the AutoDepthStencil back and
        // IStateSetD3DDefaults makes the INTZ again. No Get* calls: the client creates a pure device, which does not support them.
        for (DWORD stage = 0; stage < 16; stage++) {
            device->SetTexture(stage, nullptr);
        }

        device->SetDepthStencilSurface(nullptr);
    }

    if (s_surface) {
        s_surface->Release();
        s_surface = nullptr;
    }

    if (s_texture) {
        s_texture->Release();
        s_texture = nullptr;
    }

    if (s_wrapper) {
        s_wrapper->m_apiSpecificData = nullptr;
    }

    s_width = 0;
    s_height = 0;
}

IDirect3DTexture9* VolFog::DepthTexture() {
    return s_texture;
}

#else

bool VolFog::WantsReadableDepthFor(IDirect3D9*, uint32_t) {
    return false;
}

bool VolFog::CreateDepthTexture(IDirect3DDevice9*) {
    return false;
}

void VolFog::ReleaseDepthTexture(IDirect3DDevice9*) {
}

IDirect3DTexture9* VolFog::DepthTexture() {
    return nullptr;
}

#endif

CGxTex* VolFog::DepthTex() {
    IDirect3DTexture9* texture = VolFog::DepthTexture();

    if (!texture) {
        return nullptr;
    }

    if (!s_wrapper) {
        // Point filter, clamp: s0 of the shaders (sDepth). Not created through GxTexCreate, so the device does not list it, does
        // not recreate it on reset and never calls a user function for it: ISetTexture only reads m_apiSpecificData and m_flags.
        // Brief B3b: m_renderTarget = 1 on purpose. The D3D9Ex backend keeps every texture that is not a render target in its LRU cache
        // (ISetTexture -> ITexTouch -> m_texCacheList) and, when the cache is full, evicts the oldest with ITexReleaseD3dTexture, which
        // would Release() the INTZ reference this file owns (and, with m_apiSpecificData cleared, leave the wrapper linked: endless loop).
        // With the flag set ISetTexture / ITexTouch / ITexLinkBound / ITexUnlinkBound / ITexAcquire skip it; D3D9 never looks at it.
        s_wrapper = new CGxTex(GxTex_2d, s_width, s_height, 1, GxTex_IntZ, GxTex_IntZ, CGxTexFlags(GxTex_Nearest, 0, 0, 0, 0, 1), nullptr, nullptr, "wolVolFogDepth");
    }

    s_wrapper->m_width = s_width;
    s_wrapper->m_height = s_height;
    s_wrapper->m_apiSpecificData = texture;
    s_wrapper->m_needsCreation = 0;
    s_wrapper->m_needsUpdate = 0;
    s_wrapper->m_needsFlagUpdate = 0;

    return s_wrapper;
}

uint32_t VolFog::DepthWidth() {
    return s_width;
}

uint32_t VolFog::DepthHeight() {
    return s_height;
}

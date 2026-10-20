#include "Engine/Source/VolFog/VolFog.h"
#include "Engine/Source/VolFog/VolFogLights.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"

#if defined(_WIN32)
#include <d3d9.h>
#include <wchar.h>
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
    // Brief B13, MSAA + copy mode: s_texture is then the RESZ copy of the multisampled depth, s_surface stays nullptr (the INTZ is not the
    // depth-stencil of the device) and ResolveDepth() uses the device and the state block below.
    IDirect3DDevice9* s_device;
    IDirect3DStateBlock9* s_block;
#endif
    uint32_t s_width;
    uint32_t s_height;
    CGxTex* s_wrapper; // kept for the whole run: the pass binds it, the device never owns it
    bool s_copyMode;           // brief B13
    bool s_reszBroken;         // brief B13: the RESZ self-test failed once in this run; the MSAA is not kept again
    bool s_restartRequested;   // brief B13: see TakeDeviceRestart
    uint32_t s_resolveFailures;
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

// Brief B13: the system d3d9.dll (AMD's native driver) must not be asked for RESZ. Measured on the RX 9070 XT (Adrenalin, 2026-10-20) with
// prueba-b13\resz_test.cpp: CheckDeviceFormat reports RESZ, but the AMD sample sequence (dummy point with the INTZ in stage 0, then
// SetRenderState(D3DRS_POINTSIZE, 0x7FA05000)) leaves the GPU unable to finish: the next event query or Release never returns, with 2x, 4x and
// 8x, D24S8 / D24X8 / D16, pure device or not. DXVK implements the same trigger itself (D3D9DeviceEx::ResolveZ) and works. A hang cannot be
// caught by a self-test, so the copy mode is only offered when d3d9.dll is not the one in the system folder; the environment variable
// WOL_RESZ_NATIVE=1 lifts the block (for testing other drivers: it can hang the machine's GPU).
static bool InFolder(const wchar_t* file, const wchar_t* folder) {
    const wchar_t* slash = wcsrchr(file, L'\\');

    if (!slash) {
        return false;
    }

    size_t length = static_cast<size_t>(slash - file);

    return wcslen(folder) == length && _wcsnicmp(file, folder, length) == 0;
}

static bool NativeD3d9() {
    static int s_native = -1;

    if (s_native < 0) {
        wchar_t path[MAX_PATH] = {};
        wchar_t system[MAX_PATH] = {};
        wchar_t wow64[MAX_PATH] = {};
        HMODULE module = GetModuleHandleW(L"d3d9.dll");

        s_native = module && GetModuleFileNameW(module, path, MAX_PATH) && ((GetSystemDirectoryW(system, MAX_PATH) && InFolder(path, system)) || (GetSystemWow64DirectoryW(wow64, MAX_PATH) && InFolder(path, wow64))) ? 1 : 0;
        // const char*: MSVC lets a string literal (and a ?: of two) convert to char*, which picks the va_list overload of Log (the first run of B13 crashed here)
        const char* const which = s_native ? "the system (native) driver copy" : "not the system copy (DXVK or another wrapper)";
        CGxDevice::Log("wolVolFog: d3d9.dll is %s", which);
    }

    return s_native == 1;
}

static bool ReszAllowed() {
    if (!NativeD3d9()) {
        return true;
    }

    wchar_t value[8] = {};

    return GetEnvironmentVariableW(L"WOL_RESZ_NATIVE", value, 8) > 0 && value[0] == L'1';
}

// Hook point WOL_NVAPI (brief B13). On NVIDIA's native D3D9 driver there is no RESZ; coa-vfog copies the multisampled D24S8 depth there with
// NvAPI_D3D9_StretchRectEx after NvAPI_D3D9_RegisterResource of the two surfaces (coa-vfog src/msaa_depth.cpp: NvapiD3D9, DepthCopy::Attach /
// Copy). The NvAPI SDK of NVIDIA is not downloaded (licence; decision of Cristian), so nothing of it is compiled today and without RESZ the
// MSAA is dropped as in B3b. To add it: define WOL_NVAPI, write VolFogNvapi.cpp with VolFog::NvapiProbe(IDirect3D9*) (nvapi.dll loads and
// initialises on an NVIDIA adapter that is not DXVK), VolFog::NvapiAttach(IDirect3DSurface9* multisampledDepth, IDirect3DTexture9* intz),
// VolFog::NvapiCopy(IDirect3DDevice9*) and VolFog::NvapiDetach(), declare them in VolFog.h and enable the three WOL_NVAPI blocks of this file
// (PrepareMsaaCopy, CreateDepthCopy, ResolveDepth); the self-test then runs through the same call.

// Brief B13: the back buffer is multisampled and the driver has RESZ: INTZ becomes a copy of the multisampled depth that the device already has
// as AutoDepthStencil (the backend made it in ISetPresentParms through PrepareMsaaCopy). Returns false like CreateDepthTexture does when the INTZ is
// not the depth of the device; DepthTexture() tells whether the copy exists.
static bool CreateDepthCopy(IDirect3DDevice9* device) {
    const D3DFORMAT intz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    IDirect3DSurface9* depth = nullptr;
    D3DSURFACE_DESC depthDesc;

    if (FAILED(device->GetDepthStencilSurface(&depth)) || !depth || FAILED(depth->GetDesc(&depthDesc)) || depthDesc.MultiSampleType == D3DMULTISAMPLE_NONE) {
        if (depth) {
            depth->Release();
        }

        CGxDevice::Log("wolVolFog: the multisampled depth of the device is not available, the readable depth waits for gxRestart");
        VolFog::Message("wolVolFog: pendiente de gxRestart (el back buffer tiene MSAA)");

        return false;
    }

    IDirect3DTexture9* texture = nullptr;
    HRESULT result = device->CreateTexture(depthDesc.Width, depthDesc.Height, 1, D3DUSAGE_DEPTHSTENCIL, intz, D3DPOOL_DEFAULT, &texture, nullptr);

    if (FAILED(result) || !texture) {
        depth->Release();
        CGxDevice::Log("wolVolFog: CreateTexture(%ux%u, INTZ) for the MSAA copy failed (0x%08X), the readable depth waits for gxRestart", depthDesc.Width, depthDesc.Height, static_cast<unsigned>(result));
        VolFog::Message("wolVolFog: pendiente de gxRestart (el back buffer tiene MSAA)");

        return false;
    }

    // Self-test (design 5.3): two known depths written to the multisampled depth, resolved into the INTZ and read back, twice with the halves
    // swapped. If it fails the MSAA cannot be kept with the fog: s_reszBroken makes the next device single-sampled, and the backend's
    // DeviceSetFormat rebuilds it right away (TakeDeviceRestart).
    IDirect3DStateBlock9* block = VolFog::ReszCreateStateBlock(device);
    float first = -1.0f;
    float second = -1.0f;
    bool passed = block != nullptr;

#if defined(WOL_NVAPI)
    // WOL_NVAPI hook point (1/3): attach `depth` and `texture` to the NvAPI copy here when the driver has no RESZ.
#endif

    passed = passed && VolFog::ReszSelfTest(device, texture, depth, block, &first, &second);

    if (!passed) {
        depth->Release();

        if (block) {
            block->Release();
        }

        texture->Release();
        s_reszBroken = true;
        s_restartRequested = true;
        CGxDevice::Log("wolVolFog: RESZ self-test failed (%s): the last round read %.4f / %.4f where the multisampled depth held 0.7500 / 0.3125; the MSAA is dropped on the next device", block ? "the resolve did not give the depth back" : "no state block", first, second);
        VolFog::Message("wolVolFog: RESZ no funciona, MSAA desactivado");

        return false;
    }

    s_texture = texture;
    s_surface = nullptr;
    s_width = depthDesc.Width;
    s_height = depthDesc.Height;
    s_device = device;
    s_block = block;
    s_copyMode = true;
    s_resolveFailures = 0;
    VolFog::SetLightDevice(device); // brief B8: the local light texture is made on this device when the first lights show up
    CGxDevice::Log("wolVolFog: depth mode MSAA+copy: INTZ %ux%u is a RESZ copy of the %dx multisampled depth (D3DFORMAT %d), self-test passed (last round read %.4f / %.4f)", depthDesc.Width, depthDesc.Height, static_cast<int>(depthDesc.MultiSampleType), static_cast<int>(depthDesc.Format), first, second);
    depth->Release();

    return false;
}

bool VolFog::CreateDepthTexture(IDirect3DDevice9* device) {
    VolFog::ReleaseDepthTexture(device);
    s_restartRequested = false; // brief B13: only a failure of this very call asks for a rebuild

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
        // Brief B13: with RESZ the multisampled depth stays the depth of the device and the INTZ is a copy of it.
        if (VolFog::s_caps.resz && !s_reszBroken && ReszAllowed()) {
            return CreateDepthCopy(device);
        }

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

        // Brief B13: in the copy mode the depth-stencil of the device is the multisampled AutoDepthStencil, which Reset handles itself.
        if (!s_copyMode) {
            device->SetDepthStencilSurface(nullptr);
        }
    }

    // Brief B13: the state block holds references to whatever the device had bound when it was captured, so it goes before the Reset too.
    if (s_block) {
        s_block->Release();
        s_block = nullptr;
    }

    s_device = nullptr;
    s_copyMode = false;

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

bool VolFog::PrepareMsaaCopy(IDirect3D9* d3d, uint32_t backBufferFormat, bool windowed, uint32_t multisampleType, uint32_t* depthFormat, const char** message) {
    static const char* const s_noResz = "wolVolFog: gxMultisample ignorado (el driver no ofrece RESZ: el depth legible no admite MSAA)";
    static const char* const s_broken = "wolVolFog: RESZ no funciona, MSAA desactivado";
    static const char* const s_native = "wolVolFog: gxMultisample ignorado (D3D9 nativo: el RESZ del driver bloquea la GPU; con DXVK si hay MSAA)";

    *message = s_noResz;

    if (VolFog::s_mode <= 0 || !d3d || !depthFormat) {
        return false;
    }

    if (s_reszBroken) {
        *message = s_broken;

        return false;
    }

    if (!ReszAllowed()) {
        *message = s_native;

        return false;
    }

    const D3DFORMAT back = static_cast<D3DFORMAT>(backBufferFormat);
    const D3DFORMAT resz = static_cast<D3DFORMAT>(MAKEFOURCC('R', 'E', 'S', 'Z'));
    bool copyable = d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, back, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, resz) == D3D_OK;

#if defined(WOL_NVAPI)
    // WOL_NVAPI hook point (2/3): copyable = copyable || VolFog::NvapiProbe(d3d);
#endif

    if (!copyable) {
        return false;
    }

    // The client asks for D24X8 with gxDepthBits 24; the fog's own depth (INTZ) has stencil bits, and so does coa-vfog's multisampled one.
    if (static_cast<D3DFORMAT>(*depthFormat) == D3DFMT_D24X8 &&
        d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, back, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8) == D3D_OK &&
        d3d->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_D24S8, windowed ? TRUE : FALSE, static_cast<D3DMULTISAMPLE_TYPE>(multisampleType), nullptr) == D3D_OK) {
        *depthFormat = static_cast<uint32_t>(D3DFMT_D24S8);
    }

    return true;
}

bool VolFog::ReszBroken() {
    return s_reszBroken;
}

bool VolFog::TakeDeviceRestart() {
    bool requested = s_restartRequested;
    s_restartRequested = false;

    return requested;
}

bool VolFog::DepthIsCopy() {
    return s_copyMode && s_texture != nullptr;
}

void VolFog::ResolveDepth() {
    if (!s_copyMode || !s_texture || !s_device || !s_block || VolFog::s_mode <= 0) {
        return;
    }

    bool ok = VolFog::ReszResolve(s_device, s_texture, s_block);

#if defined(WOL_NVAPI)
    // WOL_NVAPI hook point (3/3): when the copy method is NvAPI, ok = VolFog::NvapiCopy(s_device);
#endif

    if (ok) {
        s_resolveFailures = 0;

        return;
    }

    if (++s_resolveFailures == 3) {
        static const char* const s_failed = "wolVolFog: the RESZ depth copy failed three frames in a row; the client's own fog is back.";
        CGxDevice::Log("%s", s_failed);
        VolFog::Message(s_failed);
        VolFog::SetHardFailure(true);
    }
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

bool VolFog::PrepareMsaaCopy(IDirect3D9*, uint32_t, bool, uint32_t, uint32_t*, const char** message) {
    *message = "";

    return false;
}

bool VolFog::ReszBroken() {
    return false;
}

bool VolFog::TakeDeviceRestart() {
    return false;
}

bool VolFog::DepthIsCopy() {
    return false;
}

void VolFog::ResolveDepth() {
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

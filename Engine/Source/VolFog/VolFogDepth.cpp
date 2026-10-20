#include "Engine/Source/VolFog/VolFog.h"
#include "Engine/Source/VolFog/VolFogLights.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxTex.h"
#include "Engine/Source/Gx/CGxTexFlags.h"

#if defined(_WIN32)
#include <d3d9.h>
#include <cstring>
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
    bool s_newDevice;          // brief B13b: NoteDeviceCreate, taken (and cleared) by the next CreateDepthTexture
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

// Brief B13b (REVISION-B13, finding 1): the RESZ trigger is only sent to a d3d9.dll recognised as DXVK by its content. Measured on the RX 9070 XT
// (Adrenalin, 2026-10-20) with prueba-b13\resz_test.cpp: the native AMD driver reports RESZ, but the AMD sample sequence (dummy point with the
// INTZ in stage 0, then SetRenderState(D3DRS_POINTSIZE, 0x7FA05000)) leaves the GPU unable to finish: the next event query or Release never
// returns, with 2x, 4x and 8x, D24S8 / D24X8 / D16, pure device or not. DXVK implements the same trigger itself (D3D9DeviceEx::ResolveZ) and
// works. A hang cannot be caught by a self-test. B13 only refused the d3d9.dll of System32 / SysWOW64 (a block list), but ReShade, GShade, SpecialK
// and other proxies are installed as d3d9.dll next to the exe, load the System32 one by full path and pass the trigger on to the driver; with two
// modules called d3d9.dll, GetModuleHandle cannot say which one it returns. So this is an allow list and it looks at what the module is, not at
// where it lives: the module that implements the IDirect3D9 object the client got from Direct3DCreate9 (the address of its vtable and the address
// of its first method must both be in the image of one and the same module) has to export Direct3DCreate9 and the unnamed ordinals 28257 / 28258
// (DXVK_RegisterAnnotation / DXVK_UnRegisterAnnotation: not in the d3d9.dll of Windows, nor in a proxy) and its version resource must say
// ProductName DXVK. Anything else, or any doubt, counts as "not DXVK": the MSAA is dropped as in B3b. A future DXVK that changes any of this fails
// to the safe side. The environment variable WOL_RESZ_NATIVE=1 lifts the block, for tests only: it can hang the GPU of the machine.
namespace {
    // -1: no IDirect3D9 has been looked at in this run (CreateDepthTexture then refuses the copy mode), 0: not DXVK or in doubt, 1: DXVK.
    int s_dxvk = -1;
}

static HMODULE ModuleOfAddress(const void* address) {
    HMODULE module = nullptr;

    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module)) {
        return nullptr;
    }

    return module;
}

// True when the version resource of `module` (read from memory) has ProductName = "DXVK". It is a tree of structures with UTF-16 text: for each
// string, a 6 byte header (all of them start on a multiple of 4 from the start of the resource), the key and a NUL, padding to a multiple of 4 and the
// value and a NUL.
static bool ProductNameIsDxvk(HMODULE module) {
    static const wchar_t s_key[] = L"ProductName";
    static const wchar_t s_value[] = L"DXVK";

    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16)) /* 16 = RT_VERSION */;
    HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
    const BYTE* data = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
    const size_t size = found ? SizeofResource(module, found) : 0;

    if (!data) {
        return false;
    }

    for (size_t at = 2; at + sizeof(s_key) <= size; at += 4) { // a key starts 6 bytes into a structure: 2 more than a multiple of 4
        if (std::memcmp(data + at, s_key, sizeof(s_key)) == 0) {
            const size_t value = (at + sizeof(s_key) + 3) & ~static_cast<size_t>(3);

            return value + sizeof(s_value) <= size && std::memcmp(data + value, s_value, sizeof(s_value)) == 0;
        }
    }

    return false;
}

static bool IsDxvk(IDirect3D9* d3d) {
    static HMODULE s_loggedModule;
    static int s_loggedVerdict = -1;

    const void* const* table = d3d ? *reinterpret_cast<const void* const* const*>(d3d) : nullptr;
    HMODULE viaTable = table ? ModuleOfAddress(table) : nullptr;
    HMODULE viaMethod = table ? ModuleOfAddress(table[0]) : nullptr;
    HMODULE module = viaTable == viaMethod ? viaTable : nullptr; // two different modules (or none) is a doubt: not DXVK
    const bool create = module && GetProcAddress(module, "Direct3DCreate9") != nullptr;
    const bool ordinals = module && GetProcAddress(module, MAKEINTRESOURCEA(28257)) != nullptr && GetProcAddress(module, MAKEINTRESOURCEA(28258)) != nullptr;
    const bool product = module && ProductNameIsDxvk(module);
    const bool dxvk = create && ordinals && product;

    s_dxvk = dxvk ? 1 : 0;

    if (module != s_loggedModule || s_loggedVerdict != s_dxvk) {
        s_loggedModule = module;
        s_loggedVerdict = s_dxvk;

        char path[MAX_PATH * 3] = "(the IDirect3D9 is not in a single module)";
        wchar_t wide[MAX_PATH] = {};

        if (module && GetModuleFileNameW(module, wide, MAX_PATH)) {
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, sizeof(path), nullptr, nullptr);
        }

        // const char* variables with several arguments: a lone literal (or a ?: of two) would pick the va_list overload of Log
        const char* const verdict = dxvk ? "DXVK" : "not DXVK";
        const char* const yes = "yes";
        const char* const no = "no";
        CGxDevice::Log("wolVolFog: d3d9.dll is %s (the IDirect3D9 comes from %s; Direct3DCreate9: %s, ordinals 28257/28258: %s, ProductName DXVK: %s)", verdict, path, create ? yes : no, ordinals ? yes : no, product ? yes : no);
    }

    return dxvk;
}

static bool ReszNativeOverride() {
    wchar_t value[8] = {};

    return GetEnvironmentVariableW(L"WOL_RESZ_NATIVE", value, 8) > 0 && value[0] == L'1';
}

static bool ReszAllowed(IDirect3D9* d3d) {
    if (IsDxvk(d3d)) {
        return true;
    }

    if (ReszNativeOverride()) {
        CGxDevice::Log("wolVolFog: WOL_RESZ_NATIVE=1: RESZ is used although d3d9.dll is not DXVK (for tests only: it can hang the GPU)");

        return true;
    }

    return false;
}

// CreateDepthTexture has no IDirect3D9 (the client creates a pure device: no GetDirect3D) and uses the verdict that PrepareMsaaCopy reached for the
// device it is making. No verdict yet (the back buffer is multisampled without PrepareMsaaCopy having run) is a refusal.
static bool ReszAllowedCached() {
    return s_dxvk == 1 || ReszNativeOverride();
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
static bool CreateDepthCopy(IDirect3DDevice9* device, bool freshDevice) {
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
    // swapped. Brief B13b: only a depth that comes back wrong, on a device that has just been created, shows that RESZ does not work: s_reszBroken
    // makes the next device single-sampled and the backend's DeviceSetFormat rebuilds it right away (TakeDeviceRestart). A failed D3D call says
    // nothing about RESZ (device lost right after a Reset, no video memory), and after a Reset (Alt+Tab, resize) nobody rebuilds the device: in those
    // cases nothing is marked for the run, the copy is not made and the next Reset or gxRestart tries again.
    IDirect3DStateBlock9* block = VolFog::ReszCreateStateBlock(device);
    float first = -1.0f;
    float second = -1.0f;
    VolFog::ReszTest test = VolFog::ReszTest::CallFailed;

#if defined(WOL_NVAPI)
    // WOL_NVAPI hook point (1/3): attach `depth` and `texture` to the NvAPI copy here when the driver has no RESZ.
#endif

    if (block) {
        test = VolFog::ReszSelfTest(device, texture, depth, block, &first, &second);
    }

    if (test != VolFog::ReszTest::Passed) {
        depth->Release();

        if (block) {
            block->Release();
        }

        texture->Release();

        if (test == VolFog::ReszTest::WrongValue && freshDevice) {
            s_reszBroken = true;
            s_restartRequested = true;
            CGxDevice::Log("wolVolFog: RESZ self-test failed: the last round read %.4f / %.4f where the multisampled depth held 0.7500 / 0.3125; the MSAA is dropped and the device is rebuilt without it", first, second);
            VolFog::Message("wolVolFog: RESZ no funciona, MSAA desactivado (se reconstruye el dispositivo sin MSAA)");
        } else if (test == VolFog::ReszTest::WrongValue) {
            CGxDevice::Log("wolVolFog: RESZ self-test failed after a device Reset: the last round read %.4f / %.4f where the multisampled depth held 0.7500 / 0.3125; RESZ is not marked as broken, no readable depth until the next Reset or gxRestart, which tries again", first, second);
            VolFog::Message("wolVolFog: RESZ ha fallado tras reiniciar el dispositivo; la niebla no tiene depth hasta el proximo gxRestart (se reintenta)");
        } else {
            const char* const why = block ? "a D3D call of the test failed" : "no state block";
            CGxDevice::Log("wolVolFog: the RESZ self-test could not run (%s); nothing is marked, the readable depth waits for the next Reset or gxRestart, which tries again", why);
            VolFog::Message("wolVolFog: pendiente de gxRestart (no se ha podido comprobar RESZ; se reintenta)");
        }

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
    const bool freshDevice = s_newDevice; // brief B13b: NoteDeviceCreate, as opposed to a device that went through a Reset
    s_newDevice = false;

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
        if (VolFog::s_caps.resz && !s_reszBroken && ReszAllowedCached()) {
            return CreateDepthCopy(device, freshDevice);
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
    static const char* const s_notDxvk = "wolVolFog: gxMultisample ignorado (d3d9.dll no es DXVK: el RESZ del driver nativo, o de un proxy como ReShade delante, puede bloquear la GPU; el MSAA con niebla solo se ofrece con DXVK)";

    *message = s_noResz;

    if (VolFog::s_mode <= 0 || !d3d || !depthFormat) {
        return false;
    }

    if (s_reszBroken) {
        *message = s_broken;

        return false;
    }

    if (!ReszAllowed(d3d)) {
        *message = s_notDxvk;

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

void VolFog::NoteDeviceCreate() {
    s_newDevice = true;
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

    // Brief B13b: >= 3 and counting anew after the report. The pass stops calling this once SetHardFailure(true) is set and starts again after
    // SetHardFailure(false) (a change of wolVolFog, a rebuilt device) without a new CreateDepthCopy, so the counter used to stay at 3 or more and
    // never equalled 3 again: a resolve that kept failing was never reported again and the fog read an old INTZ.
    if (++s_resolveFailures >= 3) {
        s_resolveFailures = 0;
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

void VolFog::NoteDeviceCreate() {
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

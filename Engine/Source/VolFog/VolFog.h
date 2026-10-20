#ifndef ENGINE_SOURCE_VOLFOG_VOLFOG_H
#define ENGINE_SOURCE_VOLFOG_VOLFOG_H

#include <cstdint>

struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DStateBlock9;
struct IDirect3DSurface9;
struct IDirect3DTexture9;
class CGxTex;

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B1.

namespace VolFog {
    // Support for the texture formats the fog needs, filled in by the Gx backends when the device caps are read
    // (CGxDeviceD3d::ISetCaps / CGxDeviceD3d9Ex::ISetCaps). They are not part of CGxCaps: its layout is checked
    // against the original client. The OpenGL backend never sets them, so on OpenGL everything stays false.
    struct Caps {
        bool intz = false;          // INTZ readable depth texture (D3DUSAGE_DEPTHSTENCIL + D3DRTYPE_TEXTURE)
        bool abgr16f = false;       // A16B16G16R16F render target texture
        bool abgr32f = false;       // A32B32G32R32F plain texture
        bool resz = false;          // Brief B13: RESZ depth resolve (CheckDeviceFormat with the FOURCC 'RESZ', D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE)
        uint32_t ps30Slots = 0;     // D3DCAPS9::MaxPixelShader30InstructionSlots
    };

    extern Caps s_caps;

    // Brief B6b: the smallest MaxPixelShader30InstructionSlots the pass accepts. The longest shader is WolVolFogComposite with local lights
    // (1628 slots, fxc), and a card that declares between 1024 and 1627 would fail CreatePixelShader with a message that blames patch-Z.MPQ.
    constexpr uint32_t kMinPs30Slots = 2048;

    // Brief B2: wolVolFogDebug, set by the CVar in WorldParam_CVar.cpp. 0 = off, 1 depth in grey, 2 raw march buffer (was a magenta tint in B2),
    // 3 filtered history (brief B7; was the test grid), 4 normal image without the temporal filter (B7),
    // 5 test grid (moved here from 3 in B7), 6 the light shaft texture (mask after the radial blurs) over the world viewport (brief B9).
    extern int32_t s_debugMode;

    // Brief B3: wolVolFog (0-3), set by the CVar in WorldParam_CVar.cpp. 0 = original client.
    extern int32_t s_mode;

    // Brief B4: the other wolVolFog* CVars (WorldParam_CVar.cpp validates them and writes here; every consumer reads them each
    // frame, so a change applies at once). `quality` mirrors wolVolFog (s_mode). Defaults = the CVar defaults.
    struct SettingValues {
        float density = 1.0f;       // wolVolFogDensity 0.0-4.0: multiplies the haze, the low fog and the distance fog
        float godRays = 0.0f;       // wolVolFogGodRays 0.0-1.0: strength of the radial light shafts (0 = off)
        bool localLights = true;    // wolVolFogLocalLights 0/1: point lights scattered in the fog
        int32_t quality = 0;        // wolVolFog 0-3 (0 = original client)
    };

    // Live values of the CVars above. Not a copy: the reference stays valid for the whole run.
    const SettingValues& Settings();
    // Written by the CVar callbacks only.
    void SetDensity(float value);
    void SetGodRays(float value);
    void SetLocalLights(bool value);

    // True when the main depth should be the readable INTZ texture: wolVolFog > 0 and the device reports INTZ. The Gx
    // backends ask it in ISetPresentParms (to drop MSAA) and in IStateSetD3DDefaults (to create the texture).
    bool WantsReadableDepth();

    // Brief B3b: the same question for ISetPresentParms of the D3D9 backends, which runs before CreateDevice and so before
    // ISetCaps has filled s_caps (the first device of a run would never drop MSAA): wolVolFog > 0 and the INTZ format passes
    // CheckDeviceFormat on `d3d` for the back buffer format (a D3DFORMAT value, kept as uint32_t to leave d3d9.h out of here).
    bool WantsReadableDepthFor(IDirect3D9* d3d, uint32_t backBufferFormat);

    // Brief B13, MSAA + copy mode. With gxMultisample > 1 the main depth stays the multisampled AutoDepthStencil of the device and the
    // INTZ texture becomes a plain (not multisampled) copy of it, refreshed once per frame by ResolveDepth() with the RESZ trick of AMD
    // (DXVK implements it too). Asked by ISetPresentParms of the D3D9 backends after WantsReadableDepthFor said yes and gxMultisample > 1:
    // true when the MSAA can be kept, and then *depthFormat (a D3DFORMAT value) is the format of the multisampled AutoDepthStencil (a
    // D24X8 request becomes D24S8, like coa-vfog does; any other format is left alone). False when the driver reports no RESZ (and no
    // NvAPI copy: see the WOL_NVAPI hook point in VolFogDepth.cpp) or the RESZ self-test of an earlier device failed: the backend then
    // forces MSAA 1 as in B3b. `message` (never nullptr) is the line for the player in that case. ReszBroken(): the self-test gave a wrong depth
    // on a brand-new device of this run. Brief B13b: also false unless the d3d9.dll behind `d3d` is recognised as DXVK by its content (VolFogDepth.cpp,
    // IsDxvk): the native driver, and any proxy (ReShade...) in front of it, pass the RESZ trigger to a driver that hangs AMD GPUs.
    bool PrepareMsaaCopy(IDirect3D9* d3d, uint32_t backBufferFormat, bool windowed, uint32_t multisampleType, uint32_t* depthFormat, const char** message);
    bool ReszBroken();

    // Brief B13: the device was built with the copy mode and the RESZ self-test failed, so the MSAA cannot be kept. The backends' DeviceSetFormat
    // asks after a successful build; true once means "rebuild the device now" (ISetPresentParms then drops the MSAA). Cleared by the next
    // CreateDepthTexture, so a failure in a Reset path (no DeviceSetFormat) waits for the next gxRestart without a double rebuild.
    bool TakeDeviceRestart();

    // Brief B13b: the backends call this in ICreateD3dDevice, right before IStateSetD3DDefaults: the next CreateDepthTexture is on a brand-new
    // device (DeviceSetFormat asks TakeDeviceRestart afterwards). Otherwise it is on a device that went through a Reset (Alt+Tab, resize), where
    // nobody rebuilds the device, so a failed self-test must not mark RESZ as broken for the rest of the run: the next Reset tries again.
    void NoteDeviceCreate();

    // Brief B13: INTZ is a RESZ copy of the multisampled main depth (the copy mode is active on this device).
    bool DepthIsCopy();

    // Brief B13: refreshes the INTZ copy from the multisampled depth. Called first thing in Render(), at the same point of the frame the
    // fog always read the depth at. It changes raw device state (texture 0, shaders, three render states and the RESZ point size) and puts
    // all of it back with a state block, so the client's render-state cache stays true. No-op unless DepthIsCopy().
    void ResolveDepth();

    // Brief B13, VolFogResz.cpp (the only file that knows the RESZ sequence). ReszSelfTest clears the multisampled depth to two known
    // values in two halves, resolves it into `intz`, reads a texel of each half back through a small R32F target and does it again with the
    // halves swapped; Passed when both rounds read what was written (firstRead / secondRead: the last two texels read). It saves and restores
    // the render target, the depth-stencil and all the device state (`resolveBlock`: the block the resolves use). ReszResolve is the per-frame resolve; `block` is a D3DSBT_ALL state
    // block of the device (made by ReszCreateStateBlock), captured before and applied after the resolve.
    // Brief B13b: WrongValue = every call worked and the resolve gave another depth than the one written (RESZ does not work on this device);
    // CallFailed = a D3D call of the test failed (device lost right after a Reset, no video memory...): nothing is known about RESZ.
    enum class ReszTest { Passed, WrongValue, CallFailed };
    ReszTest ReszSelfTest(IDirect3DDevice9* device, IDirect3DTexture9* intz, IDirect3DSurface9* multisampledDepth, IDirect3DStateBlock9* resolveBlock, float* firstRead, float* secondRead);
    bool ReszResolve(IDirect3DDevice9* device, IDirect3DTexture9* intz, IDirect3DStateBlock9* block);
    IDirect3DStateBlock9* ReszCreateStateBlock(IDirect3DDevice9* device);

    // Brief B3, D3D9 backends only (the OpenGL stub never calls them). CreateDepthTexture makes an INTZ texture the size
    // of the back buffer (rounded up when the device has POW2-only textures) and makes it the depth-stencil surface of
    // the device; the backend then re-reads it with GetDepthStencilSurface into m_defDepthSurface. It returns false, and
    // leaves the device untouched, when the depth is not wanted, the back buffer is multisampled or the texture fails. Brief B13: with a
    // multisampled back buffer and RESZ it makes the INTZ a copy (DepthIsCopy()) and returns false as well, since the device keeps its own depth.
    // ReleaseDepthTexture must run before the device is reset or destroyed (IReleaseD3dResources). Brief B3b: it takes the device
    // so it can unbind the INTZ first (SetTexture(stage, nullptr) on stages 0-15 and SetDepthStencilSurface(nullptr)); a native D3D9
    // Reset fails with D3DERR_INVALIDCALL while a D3DPOOL_DEFAULT resource is still bound. nullptr device = just release.
    bool CreateDepthTexture(IDirect3DDevice9* device);
    void ReleaseDepthTexture(IDirect3DDevice9* device);

    // The INTZ texture, nullptr when there is none (wolVolFog 0, no INTZ, MSAA, lost device). DepthTex() wraps it in a
    // CGxTex that is not registered in the device texture list (the backend ISetTexture only needs m_apiSpecificData
    // and the flags), so the pass binds it with GxRsSet(GxRs_Texture0, ...), point filter and clamp; nullptr when
    // DepthTexture() is. DepthWidth / DepthHeight are the texture size in texels (c1.zw = 1 / size).
    IDirect3DTexture9* DepthTexture();
    CGxTex* DepthTex();
    uint32_t DepthWidth();
    uint32_t DepthHeight();

    // Lines for the player: the Gx backends write them to gx.log themselves; this one also sends them to the console
    // through the hook that WorldParam_CVar.cpp installs (the Gx library does not link the console). No hook, no output.
    typedef void (*MessageHook)(const char* line);
    extern MessageHook s_messageHook;
    void Message(const char* line);

    // wolVolFogDebug 1 without a readable depth: the warning is written once per change of the CVar.
    extern bool s_noDepthWarned;

    // Brief B5: console command wolVolFogDump (registered by WoW/Source/WorldClient/VolFogWorld.cpp). RequestDump only marks it;
    // VolFog::Render() calls RunPendingDump() first thing in the next world frame (the Gx matrices are the world camera's there),
    // which prints the frame inputs and the model layers to the console and appends them to Logs\volfog.log.
    void RequestDump();
    void RunPendingDump();

    // Brief B6: the fog is "armed" when the pass is going to draw the fog of the world this frame, so the client's own linear fog has to be
    // pushed out of range (DayNight::UpdateFog, design 1.3): wolVolFog > 0, the caps allow it (INTZ, at least kMinPs30Slots pixel shader slots), the
    // INTZ depth exists, the pass has not failed for good (shaders, targets), the view is not underwater, wolVolFogDebug is not showing
    // another view, and the last frame that ran drew without error. If a frame fails, Armed() is false from the next frame on and the
    // client goes back to its linear fog; the world is never left without fog for more than that one frame.
    bool Armed();

    // Written by VolFog::Render() (VolFogRender.cpp) and read by Armed(). SetFrameResult: the frame drew (or legitimately drew nothing,
    // e.g. underwater) = true; the camera was in liquid. SetHardFailure: the pass cannot work with this device/shaders; it is cleared when
    // wolVolFog changes or the device is rebuilt (NoteDeviceRelease, called from ReleaseDepthTexture).
    void SetFrameResult(bool ok, bool inLiquid);
    void SetHardFailure(bool failed);
    bool HardFailure();
    void NoteDeviceRelease();
    uint32_t DeviceEpoch();

    // The 32^3 density noise as a 2D atlas texture (32 x 1024, A8R8G8B8, MANAGED), created on first use; nullptr if it could not be made
    // (the fog then has no density variation). VolFogNoise.cpp.
    CGxTex* DensityNoiseTex();

    // Brief B9: the last frame drew light shafts (mask and blurs ran). For wolVolFogDump.
    bool RaysDrawn();

    // Draws the fog sub-passes over the world viewport of CGWorldFrame::OnWorldRender, between
    // DayNight::RenderGlares and PlayerNameUpdateLate. Brief B6: with wolVolFog > 0 (and the INTZ depth) it ray-marches the fog into a
    // low-resolution target, filters it with the reprojected history of the previous frame (brief B7), copies the scene and composites the
    // fog over the back buffer; wolVolFogDebug 2 shows the raw march buffer instead, 3 the filtered history, 4 the image without the temporal
    // filter, 1 the depth, 5 the test grid. If the shaders are not valid it writes one line to the console and does nothing else.
    void Render();
}

#endif

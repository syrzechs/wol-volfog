#ifndef ENGINE_SOURCE_VOLFOG_VOLFOG_H
#define ENGINE_SOURCE_VOLFOG_VOLFOG_H

#include <cstdint>

struct IDirect3D9;
struct IDirect3DDevice9;
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

    // Brief B3, D3D9 backends only (the OpenGL stub never calls them). CreateDepthTexture makes an INTZ texture the size
    // of the back buffer (rounded up when the device has POW2-only textures) and makes it the depth-stencil surface of
    // the device; the backend then re-reads it with GetDepthStencilSurface into m_defDepthSurface. It returns false, and
    // leaves the device untouched, when the depth is not wanted, the back buffer is multisampled or the texture fails.
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

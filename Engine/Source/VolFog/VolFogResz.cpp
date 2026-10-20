#include "Engine/Source/VolFog/VolFog.h"

#if defined(_WIN32)
#include <d3d9.h>
#include <cmath>
#endif

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B13: MSAA with the readable depth.
// Licensed under GPL-3.0-only. Derived from coa-vfog 205ade5 (src/msaa_depth.cpp: ResolveThroughResz, TestDepthCopy, ReadDepthTexels;
// src/fullscreen_triangle.h; shaders/vf_fullscreen_vs.hlsl and vf_depth_check.hlsl) and modified for this client: the state of the device is
// saved and restored with a D3DSBT_ALL state block instead of the per-state lists of the original (the client creates a pure device, which has
// no Get* calls for states and textures), the two shaders are embedded as ps_2_0 / vs_2_0 tokens (no build step), and the self-test and the
// per-frame resolve share one code path. See Shaders/VolFog/NOTICE-modificaciones.txt.
//
// The sequence AMD documents for RESZ (the driver resolves the multisampled depth-stencil of the device into the texture bound at stage 0
// when the point size render state is set to the magic value) is the one coa-vfog uses; DXVK implements the same trigger in
// D3D9DeviceEx::ResolveZ (it needs the multisampled depth bound as the depth-stencil and the destination texture in stage 0).

#if defined(_WIN32)

namespace {
    constexpr DWORD kReszCode = 0x7FA05000;
    constexpr float kFirstDepth = 0.3125f;   // exact in 24 bits
    constexpr float kSecondDepth = 0.75f;    // exact in 24 bits
    constexpr float kTolerance = 0.002f;     // a D24 depth read back through a driver can be off by a few units of the last bit
    constexpr DWORD kAllColour = 0xF;
    constexpr UINT kProbeTexels = 2;

    // vs_2_0: mov oPos, v0  (vf_fullscreen_vs.hlsl). fxc 10.1 output without the constant table.
    const DWORD kVertexTokens[] = {
        0xFFFE0200,
        0x0200001F, 0x80000000, 0x900F0000,
        0x02000001, 0xC00F0000, 0x90E40000,
        0x0000FFFF
    };

    // ps_2_0: texld r0, c0.xy, s0 ; oC0 = r0.xxxx  (vf_depth_check.hlsl with tex2D). fxc 10.1 output without the constant table.
    const DWORD kPixelTokens[] = {
        0xFFFF0200,
        0x0200001F, 0x90000000, 0xA00F0800,
        0x02000001, 0x80030001, 0xA0E40000,
        0x03000042, 0x800F0000, 0x80E40001, 0xA0E40800,
        0x02000001, 0x800F0000, 0x80000000,
        0x02000001, 0x800F0800, 0x80E40000,
        0x0000FFFF
    };

    // Covers the clip square of a 1 x 1 viewport (coa-vfog fullscreen_triangle.h).
    const float kTriangle[3][4] = {
        { -1.5f, -1.5f, 0.0f, 1.0f },
        { -1.5f, 4.5f, 0.0f, 1.0f },
        { 4.5f, -1.5f, 0.0f, 1.0f }
    };

    template <typename T>
    void SafeRelease(T*& pointer) {
        if (pointer) {
            pointer->Release();
            pointer = nullptr;
        }
    }

    // The sequence itself. Needs a scene to be open and the multisampled depth bound as the depth-stencil of the device.
    bool TriggerResz(IDirect3DDevice9* device, IDirect3DTexture9* intz) {
        device->SetTexture(0, intz);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZ);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);

        const float point[3] = {};
        const bool drawn = SUCCEEDED(device->DrawPrimitiveUP(D3DPT_POINTLIST, 1, point, sizeof(point)));

        // AMD's sample puts the depth and colour writes back before the trigger.
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZENABLE, TRUE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, kAllColour);
        device->SetRenderState(D3DRS_POINTSIZE, kReszCode);

        return drawn;
    }

    struct Probe {
        IDirect3DVertexShader9* vertexShader = nullptr;
        IDirect3DPixelShader9* pixelShader = nullptr;
        IDirect3DVertexDeclaration9* declaration = nullptr;
        IDirect3DTexture9* target = nullptr;
        IDirect3DSurface9* targetSurface = nullptr;
        IDirect3DSurface9* system = nullptr;

        Probe() = default;
        Probe(const Probe&) = delete;
        Probe& operator=(const Probe&) = delete;

        ~Probe() {
            SafeRelease(system);
            SafeRelease(targetSurface);
            SafeRelease(target);
            SafeRelease(declaration);
            SafeRelease(pixelShader);
            SafeRelease(vertexShader);
        }
    };

    bool CreateProbe(IDirect3DDevice9* device, Probe& probe) {
        static const D3DVERTEXELEMENT9 s_elements[] = {
            { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
            D3DDECL_END()
        };

        return SUCCEEDED(device->CreateVertexShader(kVertexTokens, &probe.vertexShader)) &&
               SUCCEEDED(device->CreatePixelShader(kPixelTokens, &probe.pixelShader)) &&
               SUCCEEDED(device->CreateVertexDeclaration(s_elements, &probe.declaration)) &&
               SUCCEEDED(device->CreateTexture(kProbeTexels, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &probe.target, nullptr)) &&
               SUCCEEDED(probe.target->GetSurfaceLevel(0, &probe.targetSurface)) &&
               SUCCEEDED(device->CreateOffscreenPlainSurface(kProbeTexels, 1, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &probe.system, nullptr));
    }

    // Draws one texel of the INTZ per pixel of the small R32F target (a 1 x 1 viewport per texel, a triangle over it, the pixel shader reads the
    // texel in c0). Scene open.
    void DrawTexels(IDirect3DDevice9* device, const Probe& probe, IDirect3DTexture9* intz, const D3DSURFACE_DESC& desc, const UINT (&texels)[kProbeTexels][2]) {
        device->SetRenderTarget(0, probe.targetSurface);
        device->SetDepthStencilSurface(nullptr);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, kAllColour);
        device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetVertexShader(probe.vertexShader);
        device->SetPixelShader(probe.pixelShader);
        device->SetVertexDeclaration(probe.declaration);
        device->SetStreamSourceFreq(0, 1);
        device->SetTexture(0, intz);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
        device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

        for (UINT i = 0; i < kProbeTexels; i++) {
            const D3DVIEWPORT9 texel = { i, 0, 1, 1, 0.0f, 1.0f };
            const float uv[4] = { (texels[i][0] + 0.5f) / desc.Width, (texels[i][1] + 0.5f) / desc.Height, 0.0f, 0.0f };
            device->SetViewport(&texel);
            device->SetPixelShaderConstantF(0, uv, 1);
            device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, kTriangle, sizeof(kTriangle[0]));
        }
    }

    bool CopyOut(IDirect3DDevice9* device, const Probe& probe, float* values) {
        D3DLOCKED_RECT locked = {};

        if (FAILED(device->GetRenderTargetData(probe.targetSurface, probe.system)) || FAILED(probe.system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            return false;
        }

        const float* texels = static_cast<const float*>(locked.pBits);

        for (UINT i = 0; i < kProbeTexels; i++) {
            values[i] = texels[i];
        }

        probe.system->UnlockRect();

        return true;
    }

    // One round: both halves of the multisampled depth cleared to known values, resolved, read back.
    bool Round(IDirect3DDevice9* device, IDirect3DTexture9* intz, IDirect3DSurface9* multisampledDepth, IDirect3DSurface9* colour, IDirect3DStateBlock9* block, const Probe& probe, const D3DSURFACE_DESC& desc, float left, float right, float* read) {
        const UINT texels[kProbeTexels][2] = { { desc.Width / 4, desc.Height / 2 }, { desc.Width * 3 / 4, desc.Height / 2 } };
        const bool scene = SUCCEEDED(device->BeginScene());

        device->SetRenderTarget(0, colour);
        device->SetDepthStencilSurface(multisampledDepth);

        const D3DVIEWPORT9 whole = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
        device->SetViewport(&whole);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

        const LONG middle = static_cast<LONG>(desc.Width / 2);
        const D3DRECT leftHalf = { 0, 0, middle, static_cast<LONG>(desc.Height) };
        const D3DRECT rightHalf = { middle, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
        device->Clear(1, &leftHalf, D3DCLEAR_ZBUFFER, 0, left, 0);
        device->Clear(1, &rightHalf, D3DCLEAR_ZBUFFER, 0, right, 0);

        const bool resolved = VolFog::ReszResolve(device, intz, block);

        if (resolved) {
            DrawTexels(device, probe, intz, desc, texels);
        }

        if (scene) {
            device->EndScene();
        }

        // Like coa-vfog: the small target is unbound before GetRenderTargetData reads it.
        device->SetRenderTarget(0, colour);
        device->SetDepthStencilSurface(multisampledDepth);

        return resolved && CopyOut(device, probe, read);
    }
}

IDirect3DStateBlock9* VolFog::ReszCreateStateBlock(IDirect3DDevice9* device) {
    IDirect3DStateBlock9* block = nullptr;

    if (!device || FAILED(device->CreateStateBlock(D3DSBT_ALL, &block))) {
        return nullptr;
    }

    return block;
}

bool VolFog::ReszResolve(IDirect3DDevice9* device, IDirect3DTexture9* intz, IDirect3DStateBlock9* block) {
    if (!device || !intz || !block || FAILED(block->Capture())) {
        return false;
    }

    // Capture / Apply put back texture 0, the shaders, the FVF, stream 0 (the UP draw unbinds it), Z / colour writes and the point size.
    const bool ok = TriggerResz(device, intz);
    block->Apply();

    return ok;
}

VolFog::ReszTest VolFog::ReszSelfTest(IDirect3DDevice9* device, IDirect3DTexture9* intz, IDirect3DSurface9* multisampledDepth, IDirect3DStateBlock9* resolveBlock, float* firstRead, float* secondRead) {
    D3DSURFACE_DESC desc = {};

    if (!device || !intz || !multisampledDepth || !resolveBlock || FAILED(multisampledDepth->GetDesc(&desc)) || desc.Width < 4) {
        return ReszTest::CallFailed;
    }

    IDirect3DSurface9* colour = nullptr;

    if (FAILED(device->GetRenderTarget(0, &colour)) || !colour) {
        return ReszTest::CallFailed;
    }

    // The whole device state, so that the client finds it as it left it (this runs while the Gx backend builds the device).
    IDirect3DStateBlock9* saved = VolFog::ReszCreateStateBlock(device);
    IDirect3DSurface9* depth = nullptr;
    device->GetDepthStencilSurface(&depth);

    Probe probe;
    ReszTest result = ReszTest::CallFailed; // brief B13b: a call that fails says nothing about RESZ; only a depth read back wrong does

    if (saved && CreateProbe(device, probe)) {
        float read[kProbeTexels] = {};
        const float rounds[2][2] = { { kFirstDepth, kSecondDepth }, { kSecondDepth, kFirstDepth } };

        result = ReszTest::Passed;

        for (int round = 0; round < 2 && result == ReszTest::Passed; round++) {
            if (!Round(device, intz, multisampledDepth, colour, resolveBlock, probe, desc, rounds[round][0], rounds[round][1], read)) {
                result = ReszTest::CallFailed;
                break;
            }

            if (firstRead) {
                *firstRead = read[0];
            }

            if (secondRead) {
                *secondRead = read[1];
            }

            for (UINT i = 0; i < kProbeTexels; i++) {
                if (!(std::fabs(read[i] - rounds[round][i]) <= kTolerance)) { // also true for NaN
                    result = ReszTest::WrongValue;
                }
            }
        }

        device->SetRenderTarget(0, colour);
        device->SetDepthStencilSurface(depth);
        saved->Apply();
    }

    SafeRelease(saved);
    SafeRelease(depth);
    SafeRelease(colour);

    return result;
}

#else

IDirect3DStateBlock9* VolFog::ReszCreateStateBlock(IDirect3DDevice9*) {
    return nullptr;
}

bool VolFog::ReszResolve(IDirect3DDevice9*, IDirect3DTexture9*, IDirect3DStateBlock9*) {
    return false;
}

VolFog::ReszTest VolFog::ReszSelfTest(IDirect3DDevice9*, IDirect3DTexture9*, IDirect3DSurface9*, IDirect3DStateBlock9*, float*, float*) {
    return ReszTest::CallFailed;
}

#endif

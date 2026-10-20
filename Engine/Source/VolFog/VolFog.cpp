#include "Engine/Source/VolFog/VolFog.h"

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B1.

VolFog::Caps VolFog::s_caps;

// Brief B4: the live values of wolVolFogDensity / wolVolFogGodRays / wolVolFogLocalLights. The CVar callbacks (WorldParam_CVar.cpp)
// validate and call the setters; wolVolFog itself stays in s_mode (VolFogDepth.cpp) and is mirrored in `quality` on every read.
namespace {
    VolFog::SettingValues s_settings;
}

const VolFog::SettingValues& VolFog::Settings() {
    s_settings.quality = VolFog::s_mode;

    return s_settings;
}

void VolFog::SetDensity(float value) {
    s_settings.density = value;
}

void VolFog::SetGodRays(float value) {
    s_settings.godRays = value;
}

void VolFog::SetLocalLights(bool value) {
    s_settings.localLights = value;
}

// Brief B6: state of the pass for VolFog::Armed(). No dependency on FFX or the shaders: the Gx backends call NoteDeviceRelease.
namespace {
    bool s_hardFailure;
    bool s_lastFrameOk = true;
    bool s_inLiquid;
    uint32_t s_deviceEpoch;
}

bool VolFog::Armed() {
    return VolFog::s_mode > 0 && VolFog::s_caps.intz && VolFog::s_caps.ps30Slots >= VolFog::kMinPs30Slots && VolFog::DepthTexture() != nullptr && !s_hardFailure && s_lastFrameOk && !s_inLiquid && (VolFog::s_debugMode == 0 || (VolFog::s_debugMode >= 2 && VolFog::s_debugMode <= 4) || VolFog::s_debugMode == 6);
}

void VolFog::SetFrameResult(bool ok, bool inLiquid) {
    s_lastFrameOk = ok;
    s_inLiquid = inLiquid;
}

void VolFog::SetHardFailure(bool failed) {
    s_hardFailure = failed;
}

bool VolFog::HardFailure() {
    return s_hardFailure;
}

void VolFog::NoteDeviceRelease() {
    s_deviceEpoch++;
}

uint32_t VolFog::DeviceEpoch() {
    return s_deviceEpoch;
}

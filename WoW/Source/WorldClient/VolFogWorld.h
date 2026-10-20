#ifndef WOW_SOURCE_WORLDCLIENT_VOLFOGWORLD_H
#define WOW_SOURCE_WORLDCLIENT_VOLFOGWORLD_H

// Not in the original client (World of Legends): native volumetric fog (wolVolFog), brief B5.
// Licensed under GPL-3.0-only (see Shaders/VolFog/NOTICE-modificaciones.txt).

namespace VolFogWorld {
    // Installs VolFog::s_inputsProvider (the reader of the frame inputs, design 1.3), so that Engine/Source/VolFog does not depend on
    // WoW/. Called by CWorldParam::RegisterVolFogCVars.
    void Install();

    // Registers the console command wolVolFogDump. Called by CWorldParam::Initialize (the console command table exists by then).
    void RegisterCommands();
}

#endif

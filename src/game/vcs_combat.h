#pragma once

#include "ps2_runtime.h"

namespace vcs::combat
{
    // Installs combat enhancements:
    // 1. Removes the hardcoded 4-ped scan limit in ScanForEntities (0x001EEEB8)
    // 2. Forces continuous every-frame near-ped scanning for the player ped
    // 3. Implements forward cone fallback for AI melee decisions (0x001D47B8)
    // 4. Implements forward cone fallback & target alignment for player melee attacks in CWeapon::FireMelee (0x002C1D80)
    void install(PS2Runtime &runtime);
}

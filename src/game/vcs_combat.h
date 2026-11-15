#pragma once

#include "ps2_runtime.h"

namespace vcs::combat
{
    // Installs combat enhancements:
    // 1. Removes the hardcoded 4-ped scan limit in ScanForEntities (0x001EEEB8)
    // 2. Forces continuous every-frame near-ped scanning for the player ped
    // 3. Implements forward cone fallback for melee attacks (0x001D47B8)
    void install(PS2Runtime &runtime);
}

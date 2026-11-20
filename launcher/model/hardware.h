// What the player's PC has (graphics card, processor, memory) and the graphics preset it gets on a first install.
#pragma once

#include "game_config.h"

#include <cstdint>
#include <string>

namespace launcher
{
    struct HardwareInfo
    {
        std::string gpuName;       // empty when no graphics card was found (Linux: not read)
        uint64_t vramMb = 0;       // the card's own memory (0 for one that shares the system's)
        bool integrated = false;   // built into the processor (shares the system's memory)
        int threads = 0;           // logical processors
        bool avx2 = false;
        uint64_t ramMb = 0;        // physical memory
    };

    HardwareInfo probeHardware();

    // The preset this PC runs with room to spare: the lowest of what its graphics card, processor and memory each
    // allow, and never Vice Ultra (that one is the player's to choose). `why` gets a one-line reason.
    GraphicsPreset recommendPreset(const HardwareInfo &hw, std::string &why);
}

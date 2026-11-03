// The game's memory past the PS2's 32 MB (docs/FORK.md T10, docs/game-internals.md 9e). The host holds 64 MB of EE RAM
// and the game sees the PS2's 32 (runtime/ps2_memory.h); the upper 32 MB are the port's. VCS streams every model and
// texture dictionary into one heap of 5.4 MB, which FUN_002E5688 takes from its main heap at boot: built in the upper
// memory instead, it can be larger, so more stays loaded (more car variety, room for large mods) - and the main heap
// keeps the 5.4 MB. The world's buildings are not in it (the level's areas: game-internals.md 9f).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::memory
{
    // Game > Memory > Streaming memory (applies at the next start: the heap is built at boot).
    void registerSettings();
    // The hook on FUN_002E5688, and the `memstate` command (tools: the streaming heap's size / in use / free).
    void install(PS2Runtime &runtime);

    // Where the upper memory starts (the PS2's 32 MB), and the first byte after the streaming heap there: what is free for
    // the port beyond it (resident model replacements, T10 step 4). 0 when the game's heap is used.
    constexpr uint32_t kExtendedBase = 0x02000000u;
    uint32_t extendedUsedEnd();
}

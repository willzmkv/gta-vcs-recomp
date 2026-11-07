// HD World Further (FORK.md T2): extend the detailed city radius beyond ~260m (3x3 cells).
#pragma once

class PS2Runtime;

namespace vcs::world_hd
{
    // Graphics > Distance and population > HD world distance.
    void registerSettings();
    // Hooks the area chooser, swap, and world draw to stream more cells in detail.
    void install(PS2Runtime &runtime);
}
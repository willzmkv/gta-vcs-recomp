// The game's distance fog as settings: where it starts and where it is total (docs/game-internals.md 6t).
#pragma once

class PS2Runtime;

namespace vcs::fog
{
    // Graphics > Fog: distance and start, percent of the game's (100 = the original).
    void registerSettings();
    // Wraps the RSL fog-range setter (FUN_002F6008), so every renderer path draws the scaled fog; the `fogstate` command.
    void install(PS2Runtime &runtime);
}

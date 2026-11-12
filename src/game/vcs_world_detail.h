// Enhancements > World: traffic / pedestrian density and draw distance (docs/game-internals.md 6o, 12d).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::world
{
    // Enhancements > World: Traffic density, Pedestrian density, Draw distance (100% = the original game).
    void registerSettings();
    // The `worlddetail` command; VCS_FAR_CLIP=<metres> (a test: the far plane and fog end moved out).
    void install(PS2Runtime &runtime);
    // At the frame kick (game thread): the spawners' factors and caps, the LOD distance scale, the pool and packet
    // buffer guards.
    void onGameFrame(uint8_t *rdram);
}

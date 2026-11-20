// Particles on the game's time step instead of the frame (docs/game-internals.md 4, "Effects that run per frame").
#pragma once

class PS2Runtime;

namespace vcs
{
    // Puts the particle system's per-frame parts on the time step when frames are shorter than the PS2's 1/30 s: the
    // per-particle fades, growth, rotation and animation in CParticle::Update (FUN_001E9C90, scaled before it runs), and
    // the continuous emitters' AddParticle calls (FUN_001E9078 / FUN_001E9040, passed at the PS2's rate by return
    // address). At 30 fps and in original timing at 30 nothing changes. VCS_PARTICLE_TIMING=0 off; VCS_PARTICLE_LOG=1
    // prints the AddParticle calls per second by return address.
    void installParticleTimingHooks(PS2Runtime &runtime);
}

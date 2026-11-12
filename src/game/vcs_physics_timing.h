// Vehicle physics that acts a fixed amount every frame, put on the game's time step (docs/game-internals.md 4).
#pragma once

class PS2Runtime;

namespace vcs
{
    // Scales three per-frame vehicle effects by the frame's time step over the PS2's 1/30 s when the frame is shorter, so
    // they act as much per second at any frame rate: the upward push a wheel gets from a dead or knocked-down ped under
    // it (FUN_003F34F0's ApplyTurnForce), the rolling resistance of a wheel with neither gas nor brake (ProcessWheel,
    // constants patched to guest RAM 0x7FF00..) and a bike's brake wobble (CBike::ProcessControl's ApplyTurnForce at
    // 0x33BE54, GitHub issue 64). Only while the frame isn't the PS2's (independent timing, or original
    // timing at 60). VCS_PHYSICS_TIMESTEP=0 keeps the game's per-frame amounts. Needed by the ProcessWheel patches in
    // game/vcs/overrides.toml: install it whenever the game runs.
    void installPhysicsTimingHooks(PS2Runtime &runtime);
}

// Visual effects that count frames, put on the PS2's 30 frames a second (docs/game-internals.md 4).
#pragma once

class PS2Runtime;

namespace vcs
{
    // Lightning, sun glare, clouds, camera shake, the video-cam overlay, the photo flash, the HUD bar flash's count, peds
    // fading, melee streaks, rubbish, steam after rain, explosions and the banknote spray: what they step or re-roll once a
    // frame happens once per 1/30 s of real time (or scaled by the frame's share of it) when frames are shorter than the
    // PS2's. Replaces the game's rand (FUN_00408058) with the same generator. Nothing changes at 30 fps.
    // VCS_EFFECTS_TIMING=0 off; VCS_EFFECTS_LOG=1 logs.
    void installEffectsTimingHooks(PS2Runtime &runtime);
}

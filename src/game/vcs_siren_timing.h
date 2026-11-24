// Emergency vehicle siren / lights horn-tap history timing above 30 fps (gothamor#209).
#pragma once

class PS2Runtime;

namespace vcs
{
    // Keeps CPad's 8-entry horn history (CPad+0x9A..0xA2, sampled once per frame on the PS2) on real time above
    // 30 fps so short horn taps (< 7 PS2 frames, ~233 ms) still toggle emergency lights and siren cleanly instead of
    // expiring into a continuous horn hold at high frame rates. VCS_SIREN_TIMING=0 leaves the PS2 per-frame history.
    void installSirenTimingHooks(PS2Runtime &runtime);
}

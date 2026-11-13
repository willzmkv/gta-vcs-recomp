// The radio's progress while it isn't heard, put on the game's time step (docs/game-internals.md 9, GitHub issue 117).
#pragma once

class PS2Runtime;

namespace vcs
{
    // The music manager counts, for every station, the frames since it was last heard and resumes it that many
    // 1/30 s further on. At 60 fps a minute on foot moved the station on two minutes. Scaled by the frame's time step
    // over the PS2's while the frame isn't the PS2's (independent timing, or original timing at 60);
    // VCS_MUSIC_TIMESTEP=0 keeps the game's count per frame.
    void installMusicTimingHooks(PS2Runtime &runtime);
}

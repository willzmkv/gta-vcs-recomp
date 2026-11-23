// On-foot mouse camera stabilisation (game-internals.md 12b).
#pragma once

class PS2Runtime;

namespace vcs::footcam
{
    // Hooks CCam::FollowPed (0x0029DAB0). VCS_FOOTCAM=0 disables the hook cleanly (rule 1 reference).
    void install(PS2Runtime &runtime);

    // True while keyboard+mouse is the active device and Mouse camera is on.
    void enable(bool on);

    // Right stick or camera keys moved this frame -> yield to the game's own follow camera.
    void stickMoved();
}

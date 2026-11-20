// Enhancements > Combat: the fire rate of people shooting from vehicles (docs/game-internals.md 12c).
#pragma once

class PS2Runtime;

namespace vcs::driveby
{
    // Enhancements > Combat: "Drive-by fire rate" - As on the PS2 (default) / The gun's own rate.
    void registerSettings();
    // The hook on CWeapon::Fire for the NPC drive-by's two calls; the `driveby` command. VCS_DRIVEBY_HOOKS=0: none (the
    // original game: a shot every frame, faster with the frame rate).
    void install(PS2Runtime &runtime);
}

// Mission scripts that got stuck above 30 fps (docs/game-internals.md 11h).
#pragma once

class PS2Runtime;

namespace vcs
{
    // Two hooks on the script VM's handlers (vcs_script_fixes.cpp has the story of each):
    // - the main script's "wait until this mission audio line is loaded" loop also ends when the line's slot was freed
    //   meanwhile (its speaker died): the loop never looked again, so the mission waited for ever - behind a faded-out
    //   screen that is the game's "Loading..." (GitHub issue 41). At every frame rate.
    // - GON_C3 "Purple Haze": the truck that rams the van is parked (by the mission's own commands, half a second
    //   early, behind the fade) before the two thugs are created where it would run them over. Only while the frame
    //   isn't the PS2's: at 30 fps the truck has stopped behind the van by then.
    // VCS_SCRIPT_FIXES=0 turns both off.
    void installScriptFixHooks(PS2Runtime &runtime);
}

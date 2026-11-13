// The clump animation's per-node work off the game thread (PERFORMANCE.md 48).
//
// The game's animation update (FUN_00154d10, for every ped, vehicle and object each frame in CWorld::Process) ends in
// a loop over the clump's nodes (FUN_00401ad8) that samples each node's keyframes and blends its pose - pure arithmetic
// on the clump's own node frames and node states, ~6% of the game thread with the city's population raised. The loop is
// replaced: its nodes are split among a few worker threads and the game thread waits for them - the same order of
// everything else, the same results: the per-node code is the game's own recompiled code, copied by
// tools/anim_worker_gen.py into a form that runs on a context of its own (generated/anim/anim_worker.cpp).
//
// VCS_NATIVE_ANIM=0: the game's loop as it is; =serial: the copies, on the game thread; =verify: each loop computed by
// the copies, put back, run by the game, and the two compared (`animstate` counts the differences).
#pragma once

class PS2Runtime;

namespace vcs::anim
{
    // Before runtime.run() (applyVcs): the hook on the node loop and the workers.
    void install(PS2Runtime &runtime);
}

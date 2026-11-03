// The world's streamed areas (docs/game-internals.md 6b, docs/FORK.md T2): the level keeps the high-detail geometry of
// a few areas of 3 x 3 cells around the camera; the rest of the city is the level's low-detail image.
#pragma once

class PS2Runtime;

namespace vcs::world
{
    // VCS_TRACE_AREAS=1: every area request, piece read / done and area swap, with its time - how long an area takes
    // from the request to being drawn (the pop-in while driving).
    void install(PS2Runtime &runtime);
}

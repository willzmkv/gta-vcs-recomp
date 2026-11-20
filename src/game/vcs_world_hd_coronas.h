// The street lamps' coronas as far as the detailed city (game/vcs_world_hd.cpp; docs/game-internals.md 9k): the lamps
// the game has no object for (their instances rest in the dummy pool) drawn as sprites with the game's own corona pass.
#pragma once

class PS2Runtime;

namespace vcs::world_hd_coronas
{
    // The hooks on CCoronas::Render, Im2D and RwRenderStateSet (chained after the HUD's); the `farcoronas` command.
    void install(PS2Runtime &runtime);
}

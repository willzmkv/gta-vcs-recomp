// HD world further (FORK.md T2, docs/game-internals.md 9j): the detailed city drawn further than the game's ~260 m -
// the cells around the camera read into the port's memory and drawn through the game's own world draw.
#pragma once

class PS2Runtime;

namespace vcs::world_hd
{
    // Graphics > Distance and population > Detailed city distance (world.hd_radius, VCS_HD_RADIUS; live).
    void registerSettings();
    // The hooks on the area chooser / swap / flush (the game's list back before its streaming) and the world draw (the
    // list with the extra cells); the `hdworld` command (cells, memory, records added).
    void install(PS2Runtime &runtime);
    // The setting's value now (0 Original .. 3 ~720 m); the lamp coronas (game/vcs_world_hd_coronas.cpp) follow it.
    int radius();
}

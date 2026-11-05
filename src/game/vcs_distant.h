// Distant life (docs/DISTANT_LIFE.md): decorative traffic beyond the simulated world, drawn on the real cars' models.
#pragma once

#include <cstdint>
#include <vector>

class PS2Runtime;

namespace vcs::distant
{
    // Graphics > Distance and population > Distant traffic.
    void registerSettings();
    // The proxies' simulation (once a game frame, before CWorld::Process) and the `proxies` command.
    void install(PS2Runtime &runtime);

    // A proxy car as the renderer needs it: its world matrix in the game's layout (rows right, forward, up, position;
    // w 0, 0, 0, 1) and how far from the camera it is.
    struct Proxy
    {
        float m[16];
        float distance;
        uint32_t id;    // its slot (a stable choice of model when its own is not drawn)
        uint32_t model; // the model it wears (0: any)
    };
    // The latest frame's proxies (any thread): copies them out, returns the frame number they belong to (0: none).
    uint32_t snapshot(std::vector<Proxy> &out);
    // Whether distant traffic or distant crowds are on (any thread).
    bool enabled();
    // Distant crowds on: the real peds keep the game's own distance (vcs_distance.cpp).
    bool crowdsOn();

    // Step A2, templates. The game thread, while the RSL world makes its draw items: the group of the vehicle being
    // rendered now (0: none) - each item that draws keeps it.
    uint16_t currentGroup();
    // The RSL world hands a frame's items over (game thread): the frame's groups get their proxies.
    void frameKicked(uint32_t itemFrame);
    // The worker, around each item it runs: the item's frame and group (0, 0 after).
    void workerItem(uint32_t itemFrame, uint16_t group);
    // The worker's item, a template's draw: its car's matrix inverted and the proxies to draw it for (16 floats each,
    // the game's layout).
    struct Proxies
    {
        const float *inverse = nullptr;
        const float *matrices = nullptr;
        uint32_t count = 0;
    };
    bool workerProxies(Proxies &out);
    // Counters for the `proxies` command: a proxy draw's outcome (0 drawn, 1 culled or too small, 2 the draw not on its car)
    void noteProxy(int outcome);
}

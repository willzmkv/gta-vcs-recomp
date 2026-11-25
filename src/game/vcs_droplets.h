// Xbox Rain Droplets (gothamor#212 / FusionRainDroplets): pre-HUD water droplets on the camera lens.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::droplets
{
    // Graphics > Effects: "Rain droplets" (graphics.rain_droplets, VCS_DROPLETS; live).
    void registerSettings();
    void install(PS2Runtime &runtime);
    // Called once per rendered game frame from markFrameKick().
    void onGameFrame(const uint8_t *rdram);
    // Optional splash trigger (e.g., swimming/boat spray particles near the camera).
    void notifySplash(float intensity = 0.45f);
}

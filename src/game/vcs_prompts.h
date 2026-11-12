// Button prompts by input device (Enhancements > Button prompts; docs/game-internals.md 6n): the game's prompts - its
// help text's `~k~ ~ACTION~` controls, the font's ✕ ○ △ □ glyphs, the pause menu's help-bar buttons - shown as the
// keyboard keys and mouse buttons the player bound, an Xbox pad's buttons or a PlayStation pad's, following the device
// used last (Automatic) or fixed. Original leaves the game exactly as it is.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::prompts
{
    // The Enhancements page's Button prompts setting (main.cpp, before the Advanced page).
    void registerSettings();
    // At start-up (applyVcs, after the frontend's hooks: the 2D hooks chain onto theirs): the expander of `~k~`, the
    // font and 2D hooks that find the glyphs drawn, the renderer's glyph classifier, the `prompts` command. Needs the
    // hardware renderer (PS2X_GS=hw); VCS_PROMPTS_HOOKS=0 installs nothing (the setting then does nothing).
    void install(PS2Runtime &runtime);
    // Main thread, each host frame: the glyph atlas made again when the window's size or the bindings change.
    void onHostFrame();
    // Game thread, at the frame kick: the glyph set for the device, the font's glyph table, this frame's glyphs to the
    // renderer.
    void onFrameKick(uint8_t *rdram);
}

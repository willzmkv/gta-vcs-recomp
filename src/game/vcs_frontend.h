// The game's pause menu (the "frontend": Map, Brief, Game, Stats, Controls, Audio, Display and the save / load pages;
// game-internals.md 6m) with the keyboard and the mouse, the PC way: Enter selects (Cross), Escape goes back
// (Triangle; on the tab bar that closes the menu), the cursor shows and points at items, a click selects, sliders
// follow the cursor, the wheel turns choices / scrolls / zooms the map, the map pans by dragging, the right button
// goes back. Everything goes through the game's own pad: the menu sees D-pad steps, Cross and Triangle as if pressed,
// so it moves, sounds and switches pages as it does with a gamepad, whose buttons are left as they are.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::frontend
{
    // At start-up (applyVcs, after installHudHooks: the Im2D hooks chain onto the HUD's): the menu draw, its pages'
    // and items' draws (to know where each item is on screen), the control-channel commands menustate / menumouse.
    // VCS_MENU_MOUSE=0 leaves the menu to the pad (Enter / Escape still select / go back).
    void install(PS2Runtime &runtime);

    bool open(); // the pause menu (or the save menu) is up; any thread

    // Main thread, once per host frame: where the cursor is over the menu's 640x448 picture (the 4:3 area the menu is
    // shown in, game/vcs_hud.cpp keeps it there in widescreen).
    void onHostFrame();

    // Game thread, at each pad read. `user`: the buttons the bindings and the gamepad press, and whether a stick is
    // moved - the player's own input, which takes over from the mouse. Returns the buttons the menu adds (Cross for
    // Enter, Triangle for Escape, the mouse's steps), once per game frame.
    uint16_t padButtons(uint16_t user, bool sticks, bool blocked);

    // Game thread: `key` is the menu's (Escape, Enter, the left / right mouse buttons, the wheel while the menu is up,
    // or still held from then): the bindings leave it out - Escape is Start's key outside the menu, the left button
    // Circle's.
    bool ownsKey(int key);

    void onFrameKick(); // the frame's display list goes out: the menu's drawing is over (game thread)

    // Game thread, inside a 2D draw: the menu item being drawn (its draw entered at a higher stack pointer than `sp`, the
    // caller's), 0 outside one or with the menu's mouse off (VCS_MENU_MOUSE=0). The button prompts' help-bar buttons.
    uint32_t drawingItem(uint32_t sp);
}

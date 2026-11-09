// DualSense extras (docs/CONTROLS.md "DualSense"): the light bar, the adaptive triggers and the finer haptics a PS5
// pad has, driven from the game's state. The game itself only ever asks for the DualShock 2's two rumble motors
// (game/vcs_input.cpp, the actuator sink); everything here is a host-side enhancement on top of that, each an option
// on the Gamepad page, and a no-op on any pad that is not a DualSense (the light bar alone works on a DualShock 4).
//
// What it does, like GTA V's DualSense support:
//   - Light bar: a steady colour by the player's health or a Vice theme, and a red / blue police flash while the
//     player has a wanted level (the "blikání při hledání policií").
//   - Adaptive triggers: with "Drive with the triggers" on, resistance on the throttle (R2) and a firmer brake (L2)
//     while driving; a recoil buzz on the fire trigger (R2) when a gun fires on foot.
//   - Haptics: the game's rumble already reaches the DualSense's actuators through SDL; the trigger buzz adds to it.
//   - Speaker: optionally route the game's sound (so the radio, the police scanner) to the DualSense's speaker.
//
// Threads: onHostFrame runs on the main thread (the host frame, after the pad was read), where it reads a little of
// the game's memory (as the F3 panel does) and sends the light bar / trigger reports. onActuators runs on the game
// thread (the game's motor write); it only stows the levels for the next frame.
#pragma once

class PS2Runtime;

namespace vcs::dualsense
{
    // The Gamepad page's "DualSense" section (settings/settings.h).
    void registerSettings();

    // Grabs RDRAM for the game-state reads and adds the `dualsense` control-channel command.
    void install(PS2Runtime &runtime);

    // Main thread, once per host frame: drives the light bar and adaptive triggers from the game's state. `menuOpen`
    // is true while a host menu (settings / trainer) holds the input.
    void onHostFrame(bool menuOpen);

    // Game thread, at the game's motor write (game/vcs_input.cpp): the DualShock 2 rumble the game asked for, so the
    // trigger buzz can follow impacts. small = the fast motor (on / off), large = the slow one (0..255). The game only
    // ever drives the large motor (game-internals.md 12a).
    void onActuators(unsigned small, unsigned large);

    // Game thread, at each pad read (game/vcs_input.cpp): the player is firing a gun on foot (a gun in hand, attack
    // held), for the fire trigger's recoil buzz - the game does not rumble small-arms fire, so this drives it.
    void onShooting(bool firing);

    // Clears the light bar and releases the triggers now (shutdown, the game paused in the background).
    void reset();
}

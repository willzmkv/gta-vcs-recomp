// The player's controls (docs/CONTROLS.md): keyboard / mouse bindings, the gamepad's button map, stick options, the
// mouse camera and vibration - all turned into the one DualShock 2 the game reads.
#pragma once

class PS2Runtime;

namespace vcs::input
{
    // The settings of the "Keyboard & mouse" and "Gamepad" pages (settings/settings.h), the menu keys included.
    void registerSettings();

    // Feeds the game's pad reads from the bindings (PSPadBackend::setReader) and its motor levels to the gamepad's
    // vibration (setActuatorSink). VCS_INPUT_MAP=0 keeps the runtime's built-in fixed mapping (the reference). The pad
    // reads look at the game's memory: driving or on foot, and the on-foot camera the mouse turns.
    void install(PS2Runtime &runtime);

    // Main thread, once per host frame: the mouse camera captures the mouse while the game has it (no menu open, the
    // window in front); with the window in the background the sound is muted or the game paused (the Game page's "In
    // the background", game.background: 0 keep running, 1 mute, 2 pause).
    void onHostFrame(bool menuOpen);

    // Game thread, at each game frame: holds the game while it is paused in the background.
    void holdInBackground(PS2Runtime *runtime);

    // Stops any vibration now (the game paused in the background).
    void stopVibration();

    // The device the player used last, for the button prompts (game/vcs_prompts.h): a bound key, a menu key or a mouse
    // button pressed, or the mouse moved, against a gamepad button, or a stick / trigger past half way. Taken at the
    // game's pad reads; the keyboard until the gamepad is used. Any thread.
    enum class Device
    {
        Keyboard,
        Gamepad,
    };
    Device lastDevice();

    // The free camera's controls (game/vcs_freecam.h; fixed keys, docs/CONTROLS.md), taken once per camera update on
    // the game thread. While the free camera flies the game's pad reads give it nothing, and the mouse is left for this.
    struct FreeCamInput
    {
        float strafe = 0, forward = 0, rise = 0; // -1..1: right, ahead, up
        float turn = 0, tilt = 0;                // the mouse: radians since the last update (left +, up +)
        float turnRate = 0, tiltRate = 0;        // the right stick: -1..1 (left +, up +)
        float zoom = 0;                          // -1..1: narrower (+) or wider
        bool fast = false, slow = false;
    };
    FreeCamInput takeFreeCameraInput();
}

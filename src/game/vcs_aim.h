// Free aim (docs/CONTROLS.md "Free aim", game-internals.md 12c): with Keyboard > Free aim (or Gamepad > Free aim) on,
// holding the target button with a gun aims where the mouse / right stick points instead of locking on to a ped.
//
// It is the game's own free aim (R1, then L3: a lock-on to an invisible dummy object that the left stick moves, the
// white circle drawn on it), driven by the host: the pad read enters it (R1 + L3 in one report), the mouse turns the
// dummy around Vic and the camera with it, the world update runs with the free-aim bit cleared so Vic moves as he does
// locked on with that weapon (the game's free aim makes him stand still), the camera and HUD run with it set (the free-aim
// camera and its circle), and the player's shots test their line from the camera through the circle (so they hit what
// is under it, not what the gun's own line would). VCS_FREEAIM=0 installs none of it (the reference).
//
// Two scripts start the game's free aim themselves (game-internals.md 12c): in a car a mission's drive-by mode
// (SET_PLAYER_DRIVEBY_MODE: Jive Drive, Vic shooting from the passenger seat while Lance drives), on foot the forced
// free aim (SET_PLAYER_FORCE_FREE_AIMING: Phil's shooting range). There, with free aim on for either device, the mouse
// and the right stick turn its dummy in place of the left stick, with no button to hold. The same for a gun on a vehicle
// a script attaches Vic to (ATTACH_CHAR_TO_CAR: Farewell To Arms, the helicopter's door), whose first-person camera is
// the aim: the mouse and the right stick turn it.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::aim
{
    // Before runtime.run(): the hooks (applyVcs).
    void install(PS2Runtime &runtime);
    // Enhancements > Combat: the free-aim reticle, the game's circle or a dot (main.cpp, with the other settings).
    void registerSettings();

    // What the pad read hands over, once per game frame (game thread).
    struct Input
    {
        bool want = false;        // the target button is held (or hip fire) and free aim is on for that device
        bool on = false;          // free aim is on for either device (the game's own free aim a script starts is taken over)
        float turn = 0, tilt = 0; // radians this frame: left +, up + (the mouse)
        float turnRate = 0, tiltRate = 0; // -1..1: left +, up + (the right stick)
        bool assist = false;      // aim assist (the gamepad's, pad.aim_assist): lock on first, free aim once the right
                                  // stick moves or no one is there to lock on to - GTA V's assisted aim
    };
    // What the pad report gets.
    struct Output
    {
        bool own = false;    // free aim has the target button: R1 is `r1` and L3 `l3`, not the bindings'
        bool camera = false; // and the camera: the right stick centred, the mouse camera left alone
        bool leftStick = false; // and the left stick: centred (in a script's free aim it moves the game's dummy)
        bool padR1Fires = false; // the gamepad's R1 fires (Circle) - in a script's free aim, where the right stick aims
                                 // and Circle is out of the thumb's reach
        bool r1 = false, l3 = false;
    };
    Output update(const Input &in);

    // The player's current weapon is one free aim works with (the guns: not melee, thrown, scopes, the camera).
    bool gunInHand();
    // ... or a scoped one: the sniper rifles and the rocket launcher too (the triggers' fire point, the DualSense's feel).
    bool firearmInHand();
    // Free aim is on (the game's free-aim lock is held, or a script's free aim is ours), as of the last pad read.
    bool aiming();

    // Game thread, right after the CCam `cam`'s update (CCam::Process) and before the frame's camera is built from it
    // (game/vcs_freecam.cpp's hook on FUN_002A2ED0 calls it while the free camera is off): the follow camera, back after
    // free aim, looks where the aim did from its first frame.
    void cameraUpdated(uint32_t cam);
}

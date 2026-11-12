// The mouse on the camera behind a vehicle (docs/CONTROLS.md, game-internals.md 12b): the mouse turns it itself, as on
// foot, and while the mouse moves the camera stays where it was put - neither dragged after the car nor pulled toward
// where it drives. A short time after the mouse stops (Keyboard > Car camera return) it eases back behind the car and
// the game's own camera carries on. VCS_CARCAM=0 leaves it out (the mouse moves the right stick there, as before).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace vcs::carcam
{
    // Hooks the follow-vehicle camera (FUN_0029FAF0).
    void install(PS2Runtime &runtime);

    // Does the mouse turn this camera mode itself (18 behind a vehicle, 22 behind a boat; installed)?
    bool turns(uint32_t mode);

    // Game thread, at the pad read: the mouse's turn since the last one, radians (left +, up +). The camera's next
    // update applies it.
    void mouse(float turn, float tilt);

    // Game thread, at the pad read: the right stick turns the camera (the gamepad or the camera keys). The camera's
    // next update leaves it to the game at once - held or on its way back, the stick would barely move it - and takes
    // no mouse turn meanwhile.
    void stickMoved();

    // Seconds after the mouse stops before the camera goes back behind the car; < 0 never.
    void setReturnDelay(float seconds);
}

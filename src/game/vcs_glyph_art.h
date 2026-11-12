// Button prompt glyphs drawn by code (game/vcs_prompts.h): gamepad buttons, keycaps and the mouse, from shapes and a
// small stroke font - no image files. A glyph is named by a spec string:
//   pad:<family>:<button>  family ps | xbox; button south east west north back start lb rb lt rt ls rs (stick clicks)
//                          dpad dpad_u dpad_d dpad_l dpad_r dpad_ud dpad_lr, lstick[_u|_d|_l|_r|_ud|_lr], rstick[...]
//   keys:<k>,<k>,...       keycaps in a row (platform::Key codes; mouse buttons and the wheel drawn as the mouse)
//   mouse:move             the mouse with arrows (the mouse camera)
#pragma once

#include <cstdint>
#include <string>

namespace vcs::glyphart
{
    // The glyph's width for a height of 1 (its natural shape).
    float width(const std::string &spec);
    // Draws it into w x h pixels at dst (RGBA8, R in the low byte, `stride` pixels a row; straight alpha 0..255, the
    // colour of transparent pixels bled from their neighbours so filtering keeps no dark fringe).
    void render(const std::string &spec, int w, int h, uint32_t *dst, int stride);
}

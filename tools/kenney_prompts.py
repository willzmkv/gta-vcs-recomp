#!/usr/bin/env python3
"""Kenney's Input Prompts (CC0) for the game's button prompts (src/game/vcs_glyph_art.cpp, "Icon style: Kenney").

    python tools/kenney_prompts.py copy KENNEY_DIR     the icons the prompts use (128 px, "Double") into assets/prompts/kenney
    python tools/kenney_prompts.py embed OUT.cpp       those icons as a C++ table (CMake runs it at configure), and the
                                                     subtitles' font (assets/fonts/oswald, OFL) beside them

KENNEY_DIR is the unpacked pack (https://kenney.nl/assets/input-prompts, 1.5a: "Nintendo Switch/Double/..."). Only
the files below are copied, with the pack's License.txt; they are Kenney's, CC0 - not game data - and live in git.
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets", "prompts", "kenney")
SUBTITLE_FONT = os.path.join(ROOT, "assets", "fonts", "oswald", "Oswald-wght.ttf")

PADS = {
    "PlayStation Series": ["playstation_button_color_cross", "playstation_button_color_circle", "playstation_button_color_square",
                           "playstation_button_color_triangle", "playstation_trigger_l1", "playstation_trigger_r1",
                           "playstation_trigger_l2", "playstation_trigger_r2", "playstation_button_l3", "playstation_button_r3",
                           "playstation5_button_create", "playstation5_button_options"],
    "Xbox Series": ["xbox_button_color_a", "xbox_button_color_b", "xbox_button_color_x", "xbox_button_color_y", "xbox_lb", "xbox_rb",
                    "xbox_lt", "xbox_rt", "xbox_stick_l_press", "xbox_stick_r_press", "xbox_button_view", "xbox_button_menu"],
    "Nintendo Switch": ["switch_button_a", "switch_button_b", "switch_button_x", "switch_button_y", "switch_button_l",
                        "switch_button_r", "switch_button_zl", "switch_button_zr", "switch_stick_l_press", "switch_stick_r_press",
                        "switch_button_minus", "switch_button_plus"],
}
# per family: the d-pad and both sticks, plain and with their directions
for folder, prefix in (("PlayStation Series", "playstation"), ("Xbox Series", "xbox"), ("Nintendo Switch", "switch")):
    for part in ("dpad", "stick_l", "stick_r"):
        for d in ("", "_up", "_down", "_left", "_right", "_vertical", "_horizontal"):
            PADS[folder].append("%s_%s%s" % (prefix, part, d))
KEYS = ["keyboard_%s" % c for c in "abcdefghijklmnopqrstuvwxyz0123456789"] + \
       ["keyboard_f%d" % n for n in range(1, 13)] + \
       ["keyboard_" + k for k in ("space", "enter", "tab", "backspace", "escape", "shift", "ctrl", "alt", "arrow_up", "arrow_down",
                                  "arrow_left", "arrow_right", "page_up", "page_down", "home", "end", "insert", "delete", "capslock",
                                  "comma", "period", "minus", "equals", "slash_forward", "slash_back", "semicolon", "apostrophe",
                                  "bracket_open", "bracket_close", "tilde", "numpad_enter", "numpad_plus", "asterisk")] + \
       ["mouse_left", "mouse_right", "mouse_scroll", "mouse_scroll_up", "mouse_scroll_down", "mouse_side_back",
        "mouse_side_forward", "mouse_move"]


def copy(kenney):
    os.makedirs(ASSETS, exist_ok=True)
    n = 0
    for folder, names in list(PADS.items()) + [("Keyboard & Mouse", KEYS)]:
        for name in names:
            src = os.path.join(kenney, folder, "Double", name + ".png")
            if not os.path.isfile(src):
                sys.exit("[kenney] missing %s" % src)
            shutil.copyfile(src, os.path.join(ASSETS, name + ".png"))
            n += 1
    shutil.copyfile(os.path.join(kenney, "License.txt"), os.path.join(ASSETS, "License.txt"))
    print("[kenney] %d icons -> %s" % (n, ASSETS))


def embed(out):
    names = sorted(f[:-4] for f in os.listdir(ASSETS) if f.endswith(".png")) if os.path.isdir(ASSETS) else []
    lines = ["// Made by tools/kenney_prompts.py from assets/prompts/kenney: Kenney's Input Prompts 1.5a (CC0, kenney.nl).",
             "#include <cstddef>", "", "namespace vcs::glyphart", "{",
             "    struct EmbeddedIcon", "    {", "        const char *name;", "        const unsigned char *data;",
             "        size_t size;", "    };"]
    for i, name in enumerate(names):
        data = open(os.path.join(ASSETS, name + ".png"), "rb").read()
        body = ",".join(str(b) for b in data)
        lines.append("    static const unsigned char kIcon%d[] = {%s};" % (i, body))
    lines.append("    extern const EmbeddedIcon kPromptIcons[] = {")
    lines += ['        {"%s", kIcon%d, sizeof(kIcon%d)},' % (n, i, i) for i, n in enumerate(names)]
    lines.append("        {nullptr, nullptr, 0}};")
    lines.append("    extern const size_t kPromptIconCount = %d;" % len(names))
    font = open(SUBTITLE_FONT, "rb").read() if os.path.isfile(SUBTITLE_FONT) else b""
    lines.append("    static const unsigned char kFont[] = {%s};" % (",".join(str(b) for b in font) or "0"))
    lines.append("    extern const unsigned char *const kSubtitleFont = kFont;")
    lines.append("    extern const size_t kSubtitleFontSize = %d;" % len(font))
    lines.append("}")
    text = "\n".join(lines) + "\n"
    if not (os.path.isfile(out) and open(out, encoding="utf-8").read() == text): # (unchanged: no rebuild)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        open(out, "w", encoding="utf-8", newline="\n").write(text)
    print("[kenney] %d prompt icons embedded, the subtitles' font (%d bytes)" % (len(names), len(font)))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "copy":
        copy(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == "embed":
        embed(sys.argv[2])
    else:
        sys.exit(__doc__)

#!/usr/bin/env python3
"""The game's pause menu with the keyboard, the mouse and the gamepad (src/game/vcs_frontend.cpp, game-internals.md 6m).

    python tools/menu_check.py [OUT] [--port N]

One hidden session from a copy of build/rig/saves/fortbaxter. The cursor is set over the menu's 640x448 drawing with
the control channel's `menumouse` (or in window pixels: `menumouse window x y`; the boxes come from `menustate`: where each item was drawn), the buttons and the
wheel are the keys mouseleft / mouseright / wheelup / wheeldown. Checks:
  keys      Escape opens the menu (Start), Enter selects, Escape goes back, Escape on the tab bar closes it; Enter
            outside the menu presses nothing
  window    the cursor in window pixels (the real cursor's mapping onto the menu's 4:3 area, at 16:9) over a tab's
            text - its middle and near its corners - points at that tab
  hover     pointing at a tab gives it the focus (and shows its page)
  click     a tab opens its page; the right button goes back; an item of the page shown under the tab bar opens the
            page and is selected (Game > Delete Game: the slot list), Cancel there
  slider    Audio: a click on a bar sets the level, a drag follows the cursor, the wheel turns it, a click on the label
            only moves the focus; a click on a choice turns it
  display   the Display page has no Widescreen row (the game's own anamorphic mode, kept off): up / down skip it, the
            rows below are moved up into its place and the mouse finds them there
  map       a drag moves the map with the cursor, kept in the D-pad's range; the wheel zooms
  buttons   the mouse buttons reach the game as nothing while the menu is up, nor while still held after it closed
  gamepad   Start opens the menu, the D-pad moves, Cross selects, Triangle goes back and closes it - as before
Screenshots go to OUT (default build/menu_check); OUT/results.json holds the checks. Exit code 1 when one fails.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = rig.ROOT
CIRCLE = 0x2000


def fit_rect(aspect, frame_aspect, width, height):
    """present::fitRect: a picture of `aspect` as large as fits in the frame (the window letterboxed at frame_aspect),
    centred -> x0, y0, w, h"""
    fw, fh = width, height
    if frame_aspect > 0:
        fh = fw / frame_aspect
        if fh > height:
            fh, fw = height, height * frame_aspect
    w = fw
    h = w / aspect
    if h > fh:
        h, w = fh, fh * aspect
    return (width - w) / 2, (height - h) / 2, w, h


class Check:
    def __init__(self, g, out):
        self.g = g
        self.out = out
        self.results = {}

    def ok(self, name, cond, detail=""):
        self.results[name] = {"ok": bool(cond), "detail": str(detail)}
        print("%-4s %s %s" % ("ok" if cond else "FAIL", name, detail), flush=True)
        return cond

    def shot(self, name):
        self.g.cmd("screenshot " + os.path.join(self.out, name + ".png").replace(os.sep, "/"))

    def state(self):
        """menustate as {'open', 'tabbar', 'page', 'focus', 'items': {name: (box, [flags])}}"""
        head, _, items = self.g.cmd("menustate").partition(" items=")
        s = dict(kv.split("=", 1) for kv in head.split() if "=" in kv)
        s["items"] = {}
        for it in items.split(";"):
            if it:
                parts = it.split(":")
                s["items"][parts[0]] = ([float(v) for v in parts[1].split(",")], parts[2:])
        return s

    def flag(self, item, letter, st=None):
        st = st or self.state()
        for f in st["items"].get(item, ([], []))[1]:
            if f.startswith(letter):
                return f[1:]
        return None

    def level(self, item):
        v = self.flag(item, "v")
        return int(v.split("/")[0]) if v else None

    def centre(self, item, st=None):
        b = (st or self.state())["items"][item][0]
        return (b[0] + b[2]) / 2, (b[1] + b[3]) / 2

    def move(self, x, y, wait=0.3):
        self.g.cmd("menumouse %.1f %.1f" % (x, y))
        time.sleep(wait)

    def point(self, item, wait=0.8):
        x, y = self.centre(item)
        self.move(x - 4, y, 0.15)
        self.move(x, y, wait)

    def press(self, key, wait=0.8, ms=80):
        self.g.cmd("press %s %d" % (key, ms))
        time.sleep(wait)

    def click(self, item=None, at=None, button="mouseleft", wait=1.0):
        if item:
            self.point(item, 0.3)
        if at:
            self.move(at[0], at[1], 0.3)
        self.press(button, wait)

    def where(self):
        s = self.state()
        return s.get("open"), s.get("tabbar"), s.get("page"), s.get("focus")

    def buttons(self):
        self.g.wait_frames(3)
        return int(self.g.cmd("padstate").split()[0].split("=")[1], 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "build", "menu_check"))
    ap.add_argument("--port", type=int, default=47660)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    g = rig.Game.start(save=rig.default_save(), name="menu_check",
                       data_dir=os.path.join(out, "data"), port=a.port, env={"VCS_ASPECT": "169"})
    c = Check(g, out)
    try:
        g.wait_ingame()
        # ---- keys ----
        c.press("return", 1.0)
        c.ok("keys: Enter outside the menu presses nothing", c.where()[0] == "0", c.where())
        c.press("escape", 2.0)
        c.ok("keys: Escape opens the menu", c.where() == ("1", "1", "MAP_PAGE", "Map_t"), c.where())
        c.shot("01_open")
        c.press("right")
        c.press("return", 1.2)
        c.ok("keys: Enter selects (Brief)", c.where()[1:3] == ("0", "BRIEF_PAGE"), c.where())
        c.press("escape")
        c.ok("keys: Escape goes back to the tab bar", c.where()[1:] == ("1", "BRIEF_PAGE", "Brief_t"), c.where())
        c.press("escape", 1.5)
        c.ok("keys: Escape on the tab bar closes the menu", c.where()[0] == "0", c.where())

        # ---- the cursor in window pixels: the real cursor's mapping (the 4:3 area of the 16:9 frame) ----
        c.press("escape", 2.0)
        ww, wh = (int(v) for v in c.state()["window"].split("x"))  # (a screenshot may be another size)
        x0, y0, fw, fh = fit_rect(4 / 3, 16 / 9, ww, wh)

        def window_point(item, fx=0.5, fy=0.5):
            """`item`'s box at (fx, fy) of its size, in window pixels"""
            b = c.state()["items"][item][0]
            return x0 + (b[0] + fx * (b[2] - b[0])) / 640 * fw, y0 + (b[1] + fy * (b[3] - b[1])) / 448 * fh
        missed = []
        # the tabs at the bottom corners of the menu and in the middle; their text's middle and near its corners (a
        # mapping off by a few pixels at the edges - the hit areas pulled to the middle - misses those)
        for item in ("Map_t", "Stats_t", "Controls_t", "Display_t", "Audio_t"):
            for fx, fy in ((0.5, 0.5), (0.1, 0.15), (0.9, 0.85)):
                x, y = window_point(item, fx, fy)
                g.cmd("menumouse window %.1f %.1f" % (x - 6, y))
                time.sleep(0.15)
                g.cmd("menumouse window %.1f %.1f" % (x, y))
                time.sleep(0.8)
                if c.where()[3] != item:
                    missed.append("%s@%.1f,%.1f->%s" % (item, fx, fy, c.where()[3]))
        g.cmd("menumouse off")
        c.ok("window: the cursor over a tab's text (window %dx%d, 16:9) points at it" % (ww, wh), not missed,
             " ".join(missed))
        c.press("escape", 1.5)

        # ---- the mouse ----
        c.press("escape", 2.0)
        c.point("Stats_t")
        c.ok("hover: a tab gets the focus and shows its page", c.where()[1:] == ("1", "STATS_PAGE", "Stats_t"), c.where())
        c.point("Controls_t")
        c.ok("hover: across the rows", c.where()[1:] == ("1", "CONTROLS_PAGE", "Controls_t"), c.where())
        c.click("Audio_t", wait=1.5)
        c.ok("click: a tab opens its page", c.where()[1:3] == ("0", "AUDIO_PAGE"), c.where())
        c.shot("02_audio")

        # Audio: the sliders' bars (b<x0>,<x1>)
        st = c.state()
        bars = [float(v) for v in c.flag("Music_Volume_MI", "b", st).split(",")]
        y = c.centre("Music_Volume_MI", st)[1]
        width = (bars[1] - bars[0]) / 8.0
        c.click(at=(bars[0] + 2.5 * width, y))
        c.ok("slider: a click on the third bar sets 3", c.level("Music_Volume_MI") == 3, c.level("Music_Volume_MI"))
        c.move(bars[0] + 2.5 * width, y, 0.1)
        g.cmd("key mouseleft down")
        time.sleep(0.3)
        for i in range(3, 8):
            c.move(bars[0] + (i + 0.5) * width, y, 0.15)
        time.sleep(1.0)
        dragged = c.level("Music_Volume_MI")
        g.cmd("key mouseleft up")
        time.sleep(0.3)
        c.ok("slider: a drag follows the cursor (to the eighth bar)", dragged == 8, dragged)
        before = c.level("Sfx_Volume_MI")
        b = st["items"]["Sfx_Volume_MI"][0]
        c.click(at=(b[0] + 10, (b[1] + b[3]) / 2))
        c.ok("slider: a click on its label only moves the focus",
             c.where()[3] == "Sfx_Volume_MI" and c.level("Sfx_Volume_MI") == before, (c.where(), c.level("Sfx_Volume_MI")))
        c.press("wheeldown", 0.4)
        c.press("wheeldown", 1.0)
        c.ok("slider: the wheel turns it (two down)", c.level("Sfx_Volume_MI") == before - 2, c.level("Sfx_Volume_MI"))
        c.press("wheelup", 1.0)
        mono = c.flag("MONO_STEREO", "c")
        c.click("MONO_STEREO")
        c.ok("choice: a click turns it", c.flag("MONO_STEREO", "c") not in (None, mono), (mono, c.flag("MONO_STEREO", "c")))
        c.click("MONO_STEREO")
        # the music level back as it was (the game's default, 5 bars)
        c.click(at=(bars[0] + 4.5 * width, y))
        c.shot("03_audio_changed")

        c.click(button="mouseright", wait=1.0)
        c.ok("click: the right button goes back", c.where()[1:] == ("1", "AUDIO_PAGE", "Audio_t"), c.where())

        # ---- Display: the game's own Widescreen row is hidden (vcs_frontend.cpp) ----
        c.click("Display_t", wait=1.5)
        st = c.state()
        c.ok("display: the page opens without the Widescreen row",
             st.get("page") == "DISPLAY_PAGE" and "wide_scr_MI" not in st["items"] and "RADAR_MODE_MI" in st["items"],
             sorted(st["items"]))
        c.shot("03b_display")
        walk = [c.where()[3]]
        for _ in range(6):
            c.press("down", 0.6)
            walk.append(c.where()[3])
        rows = ["Brightness_MI", "Subtitles_MI", "RADAR_MODE_MI", "HUD_MODE_MI", "Trails_MI", "screen_pos_MI", "Brightness_MI"]
        c.ok("display: down walks the rows past the hidden one", walk == rows, walk)
        for _ in range(4):  # Brightness (wrapped) -> Screen position, Trails, HUD mode, Radar mode
            c.press("up", 0.6)
        c.press("up", 0.6)
        c.ok("display: up past it (Radar mode -> Subtitles)", c.where()[3] == "Subtitles_MI", c.where())
        sub, radar = (c.state()["items"][n][0] for n in ("Subtitles_MI", "RADAR_MODE_MI"))
        c.ok("display: the rows below it moved up (no gap)", 30 <= radar[1] - sub[1] <= 50, (sub, radar))
        c.point("RADAR_MODE_MI")
        c.ok("display: the mouse points at a moved row", c.where()[3] == "RADAR_MODE_MI", c.where())
        c.click(button="mouseright", wait=1.0)
        c.point("Game_t", 1.0)
        c.click("DeleteGame_MI", wait=2.0)
        c.ok("click: an item of the page under the tab bar opens it and is selected",
             c.where()[1:] == ("0", "DELETE_GAME_SLOT_PAGE", "Cancel"), c.where())
        c.shot("04_delete_slots")
        c.click("Cancel", wait=1.5)
        c.ok("click: Cancel", c.where()[1:3] == ("0", "LOAD_PAGE"), c.where())

        # ---- buttons: none of the mouse's reach the game while the menu has it ----
        c.move(636, 444, 0.3)  # over nothing (a click on the page under the tab bar would open it)
        g.cmd("key mouseleft down")
        held = c.buttons()
        g.cmd("key mouseleft up")
        c.ok("buttons: the left button is not Circle in the menu", not (held & CIRCLE), "%04x" % held)

        # ---- the map ----
        c.click(button="mouseright", wait=1.0)
        c.click("Map_t", wait=2.5)

        def pan():
            return [float(v) for v in c.flag("Map_AE", "m").split(",")]
        p0 = pan()  # (it opens at the right end of its range: dragged to the left)
        c.move(320, 200, 0.2)
        g.cmd("key mouseleft down")
        time.sleep(0.2)
        for i in range(1, 11):
            c.move(320 - i * 8, 200 - i * 4, 0.08)
        time.sleep(0.3)
        g.cmd("key mouseleft up")
        time.sleep(0.4)
        p1 = pan()
        moved = (p1[0] - p0[0], p1[1] - p0[1])
        c.ok("map: a drag moves it with the cursor (80 x 40 pixels: 60 x 24.3 units)",
             abs(moved[0] + 60.0) < 1.0 and abs(moved[1] + 40 * 272 / 448) < 1.0, "%.1f, %.1f" % moved)
        c.move(100, 100, 0.2)
        g.cmd("key mouseleft down")
        for i in range(1, 30):
            c.move(100 + i * 40, 100 + i * 20, 0.04)
        g.cmd("key mouseleft up")
        time.sleep(0.4)
        p2 = pan()
        reach = p2[2] - 100.0
        c.ok("map: kept in the D-pad's range", p2[0] <= 240.0 + reach + 0.1 and p2[1] <= 114.0 + reach + 0.1, p2)
        c.shot("05_map_dragged")
        z0 = pan()[2]
        c.press("wheelup", 1.5)
        z1 = pan()[2]
        c.press("wheeldown", 1.5)
        z2 = pan()[2]
        c.ok("map: the wheel zooms (up in, down out)", z1 > z0 + 20 and z2 < z1 - 20, "%.1f %.1f %.1f" % (z0, z1, z2))

        # ---- buttons after the menu: still held when it closes, the left button stays out of the game ----
        c.click(button="mouseright", wait=1.0)
        c.move(636, 444, 0.2)
        g.cmd("key mouseleft down")
        time.sleep(0.3)
        c.press("escape", 1.5)  # closes it from the tab bar
        closed = c.where()[0] == "0"
        held = c.buttons()
        g.cmd("key mouseleft up")
        time.sleep(0.3)
        g.cmd("key mouseleft down")
        again = c.buttons()
        g.cmd("key mouseleft up")
        c.ok("buttons: held through the close it is not Circle; pressed again it is",
             closed and not (held & CIRCLE) and (again & CIRCLE), "closed %s held %04x again %04x" % (closed, held, again))

        # ---- the gamepad, as before ----
        g.cmd("menumouse off")
        g.cmd("gamepad on")
        time.sleep(0.3)

        def pad(button, wait=0.8):
            g.cmd("gamepad %s 1" % button)
            time.sleep(0.15)
            g.cmd("gamepad %s 0" % button)
            time.sleep(wait)
        pad("start", 2.0)
        c.ok("gamepad: Start opens the menu", c.where() == ("1", "1", "MAP_PAGE", "Map_t"), c.where())
        pad("right")
        pad("right")
        c.ok("gamepad: the D-pad moves along the tabs", c.where()[3] == "Game_t", c.where())
        pad("a", 1.2)
        c.ok("gamepad: Cross selects", c.where()[1:3] == ("0", "LOAD_PAGE"), c.where())
        pad("y")
        c.ok("gamepad: Triangle goes back", c.where()[1:] == ("1", "LOAD_PAGE", "Game_t"), c.where())
        pad("y", 1.5)
        c.ok("gamepad: Triangle on the tab bar closes the menu", c.where()[0] == "0", c.where())
        pad("start", 2.0)
        pad("start", 1.5)
        c.ok("gamepad: Start closes it", c.where()[0] == "0", c.where())
        g.cmd("gamepad off")
    finally:
        g.quit()
    json.dump(c.results, open(os.path.join(out, "results.json"), "w"), indent=1)
    bad = [k for k, v in c.results.items() if not v["ok"]]
    print("%d checks, %d failed%s" % (len(c.results), len(bad), (": " + ", ".join(bad)) if bad else ""))
    return 1 if bad or not c.results else 0


if __name__ == "__main__":
    sys.exit(main())

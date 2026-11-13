#!/usr/bin/env python3
"""Controls and player settings check (docs/CONTROLS.md, src/game/vcs_input.cpp): the bindings, checked in the pad
report they make and in the game, plus the settings registry's presets and reset, with menu screenshots.

    python tools/controls_check.py [OUT] [--port 47673]

Loads the Fort Baxter rig save (a copy, hidden), then: every default key binding (both slots, either Shift / Ctrl,
Keypad Enter, the mouse buttons) gives its PS2 button or stick direction, Alt+Enter is not Start; a rebound key moves
Vic and the old one no longer does; the walk key holds the stick at walking speed; the mouse turns the on-foot camera
by the amount asked (heading and pitch, inverted, off) and is pulled in at a wall as the stick's camera is, right click
locks on to a ped, the Driving keys (W / S / Space
accelerate, brake, handbrake in a car, and nothing else there), the mouse on the vehicle camera (turned 1:1, held while
the mouse moves, standing and driving, then back behind the car; the right stick takes it back at once), on foot again after
leaving the car; free aim (src/game/vcs_aim.cpp: the right button enters the game's free aim, the mouse turns it, a shot
hits a man on the aim line and not one beside it, Vic strafes while aiming, the camera stays where the aim looked, fists
and free aim off still lock on, the gamepad's RB and right stick, the sniper scope turned 1:1 and zoomed by the wheel,
the wheel changing weapons; in a car, the game's drive-by free aim as in Jive Drive: the left stick with free aim off,
the mouse / right stick with it on, the height limits, a shot through the circle, leaving the car; on foot, the game's
forced free aim as in Phil's shooting range: the same, the gamepad's R1 firing, the right button keeping it, the side
and height limits, the script's end; a gun on a vehicle as in Farewell To Arms: its camera turned by the mouse with no
jump to the left, the right stick and R1 with free aim on, the limits, a shot through the circle); a mouse wheel binding; a menu key
refuses a key a control has; the presets and Reset all settings; the screenshot key (F8) saves a PNG; screenshots of the settings menu's pages; Audio >
Music / Sound effects are the game's own levels (set here, shown in its pause menu, changed there, kept by a save
load) (src/game/vcs_volume.cpp). Results: OUT/summary.txt; screenshots OUT/*.png. Default
OUT: build/controls/<date-time>.
"""
import argparse
import math
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# DualShock 2 bits (pressed), as "padstate" reports them
SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT = 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080
L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE = 0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000
KEY_G, KEY_SHIFT, KEY_WHEEL_UP, KEY_F2 = 71, 340, 405, 291

# control-channel key name -> what the default bindings make of it: a button bit, or (axis, byte)
DEFAULTS = [
    ("w", ("ly", 0)), ("s", ("ly", 255)), ("a", ("lx", 0)), ("d", ("lx", 255)),
    ("i", ("ry", 0)), ("k", ("ry", 255)), ("j", ("rx", 0)), ("l", ("rx", 255)),
    ("shift", CROSS), ("rshift", CROSS), ("space", SQUARE), ("f", TRIANGLE), ("ctrl", CIRCLE), ("rctrl", CIRCLE),
    ("mouseleft", CIRCLE), ("q", L1), ("e", R1), ("mouseright", R1), ("1", L2), ("z", L2), ("3", R2), ("x", R2),
    ("c", L3), ("v", R3), ("backspace", SELECT),
    ("up", UP), ("down", DOWN), ("left", LEFT), ("right", RIGHT),
]
# Start (Escape) opens the game's pause menu: checked last. Enter is Cross in the pause menu only (tools/menu_check.py)
NOT_START_KEYS = ["return", "kpenter"]


class Check:
    def __init__(self, out, port):
        self.out = out
        self.port = port
        self.results = []
        self.g = None

    def ok(self, name, cond, detail=""):
        self.results.append((name, bool(cond), detail))
        print("%-4s %s %s" % ("ok" if cond else "FAIL", name, detail), flush=True)
        return cond

    def pad(self):
        """The last pad report: {'buttons': int, 'lx': .., 'ly': .., 'rx': .., 'ry': ..}, after a few game frames."""
        self.g.wait_frames(3)
        line = self.g.cmd("padstate")
        d = dict(re.findall(r"(\w+)=([0-9a-f]+)", line))
        return {k: int(v, 16) if k == "buttons" else int(v) for k, v in d.items()}

    def v_layout(self, car):
        """Gamepad > GTA V buttons (pad.v_layout), in a car: off the game's buttons; on R3 looks behind (L2 + R2), the
        D-pad's down is the sub-mission (R3), the touchpad Select, the D-pad's right / left the next / previous station."""
        g = self.g

        def press(button):
            g.cmd("gamepad %s 1" % button)
            p = self.pad()["buttons"]
            g.cmd("gamepad %s 0" % button)
            g.wait_frames(3)
            return p

        def station():
            return g.peek(car + 0x2BA, 1)[0]

        g.cmd("gamepad on")
        try:
            self.setting("pad.v_layout", 0)
            off = (press("rs"), press("down"), press("touchpad"))
            self.ok("GTA V buttons off: R3, D-pad down, touchpad as before", off == (R3, DOWN, 0), str(off))
            self.setting("pad.v_layout", 1)
            on = (press("rs"), press("down"), press("touchpad"), press("right") & (LEFT | RIGHT))
            self.ok("GTA V buttons: R3 looks behind, D-pad down sub-mission, touchpad Select", on == (L2 | R2, R3, SELECT, 0), str(on))
            g.wait_frames(45)
            s0 = station()
            press("right")
            g.wait_frames(45)
            s1 = station()
            press("left")
            g.wait_frames(45)
            s2 = station()
            self.ok("GTA V buttons: the D-pad steps the radio both ways", s1 == (s0 + 1) % 10 and s2 == s0, "%d -> %d -> %d" % (s0, s1, s2))
            # the right stick turns the camera again (no free aim in a vehicle: it only moved the camera)
            g.cmd("gamepad rx 1")
            p = self.pad()
            g.cmd("gamepad rx 0")
            g.wait_frames(3)
            self.ok("GTA V buttons: the right stick is the camera", p["buttons"] == 0 and p["rx"] != 128, str(p))
            # Drive with the triggers: R2 / L2 instead of the pad's Cross / Square, not as well; with GTA V's buttons
            # L1 / R1 look to the sides and the pad's Cross is the handbrake
            self.setting("pad.trigger_drive", 1)
            trig = (press("a"), press("x"))
            g.cmd("gamepad rt 1")
            rt = self.pad()["buttons"]
            g.cmd("gamepad rt 0")
            g.wait_frames(3)
            self.ok("Drive with the triggers: the pad's Cross / Square do not drive, R2 does", trig == (R1, 0) and rt == CROSS,
                    "cross %04x square %04x r2 %04x" % (trig + (rt,)))
            sides = (press("lb"), press("rb"))
            self.ok("GTA V buttons: L1 / R1 look left / right", sides == (L2, R2), "l1 %04x r1 %04x" % sides)
            # Drive with the triggers alone: the D-pad looks left / right and, down, behind
            self.setting("pad.v_layout", 0)
            look = (press("left"), press("right"), press("down"))
            self.ok("Drive with the triggers: the D-pad looks left / right / behind", look == (L2, R2, L2 | R2),
                    "left %04x right %04x down %04x" % look)
            # the DualSense's triggers stay loose while driving (pad.ds_drive_feel off): shooting only
            ds = g.cmd("dualsense")
            self.ok("DualSense: no trigger resistance while driving", "driving=1" in ds and "trig=off" in ds, ds)
        finally:
            self.setting("pad.trigger_drive", 0)
            self.setting("pad.v_layout", 0)
            g.cmd("gamepad off")

    def weapon_triggers(self):
        """Shoot with the triggers: each gun class its own fire-trigger wall (the `dualsense` command's r2= - the Weapon
        effect 0x25, its zones and strength), each round fired counted (shots=); Enhancements > Free aim reticle: Dot
        rewrites the circle texture (sitem16: its CLUT's first colour) and Circle puts it back."""
        g = self.g

        def ds():
            return dict(re.findall(r"(\w+)=(\S+)", g.cmd("dualsense")))

        self.setting("pad.trigger_shoot", 1)
        walls = {}
        for weapon, want in ((19, "25140006"), (21, "25440007"), (27, "25240004"), (30, "25440007"), (32, "25840007")):
            self.give(weapon)
            g.wait_frames(5)
            walls[weapon] = ds().get("r2")
            if walls[weapon] != want:
                walls[weapon] += "!=" + want
        self.ok("trigger walls by gun: pistol, shotgun, SMG, sniper, rocket launcher", all("!" not in w for w in walls.values()),
                str(walls))
        self.give(19)
        before = int(ds().get("shots", 0))
        g.cmd("gamepad on")
        for _ in range(3):
            g.cmd("gamepad rt 1")
            time.sleep(0.25)
            g.cmd("gamepad rt 0")
            time.sleep(0.4)
        g.cmd("gamepad off")
        fired = int(ds().get("shots", 0)) - before
        self.ok("a round fired is seen (the trigger's kick)", fired >= 2, "%d of 3 pulls" % fired)
        # out of ammo: the trigger slack (the effect Off, 0x05). The gun in hand left with one round (a second
        # GIVE_WEAPON adds its rounds to the slot's, and may leave the weapon in hand as it was)
        ped = g.player()[0]
        at = ped + 0x578 + g.peek(ped + 0x789, 1)[0] * 0x1C
        g.cmd("poke 0x%x 0100000001000000" % (at + 8))  # clip 1, ammo 1
        g.wait_frames(3)
        g.cmd("gamepad on")
        g.cmd("gamepad rt 1")
        time.sleep(1.5)
        g.cmd("gamepad rt 0")
        g.cmd("gamepad off")
        g.wait_frames(5)
        d = ds()
        self.setting("pad.trigger_shoot", 0)
        self.ok("out of ammo: the fire trigger goes slack", d.get("r2", "").startswith("05"), "r2 %s weapon %s" % (d.get("r2"), d.get("weapon")))

        def clut0():
            rec = g.u32(0x6EDD94)
            return g.peek(g.u32(g.u32(rec) + 8) + 0x800, 4).hex()

        circle = clut0()
        self.setting("enhance.reticle", 1)
        g.wait_frames(5)
        dot = clut0()
        self.setting("enhance.reticle", 0)
        g.wait_frames(5)
        back = clut0()
        self.ok("free aim reticle: the dot, then the circle back", circle == "ffffff80" and dot == "00000000" and back == circle,
                "circle %s dot %s back %s" % (circle, dot, back))

    def held(self, *keys):
        for k in keys:
            self.g.cmd("key %s down" % k)
        try:
            return self.pad()
        finally:
            for k in keys:
                self.g.cmd("key %s up" % k)

    def setting(self, key, value=None):
        r = self.g.cmd("setting %s%s" % (key, "" if value is None else " %s" % value))
        return r.split(" ", 1)[1] if r.startswith("ok ") else r

    def shot(self, name):
        self.g.cmd("screenshot " + os.path.join(self.out, name + ".png").replace("\\", "/"))

    def moved(self, key, seconds=1.5):
        """How far Vic moves while `key` is held."""
        before = self.g.player()[1]
        self.g.cmd("key %s down" % key)
        time.sleep(seconds)
        self.g.cmd("key %s up" % key)
        time.sleep(0.5)
        after = self.g.player()[1]
        return math.hypot(after[0] - before[0], after[1] - before[1])

    def keys(self, *seq):
        """Key presses, KEY[@seconds to wait after] (the game's menus take their time)"""
        for k in seq:
            key, _, wait = k.partition("@")
            self.g.cmd("press %s 150" % key)
            time.sleep(float(wait or 1.5))

    def sound(self):
        """soundstate: the game's music / effects levels (0..127)."""
        self.g.wait_frames(3)
        return dict(re.findall(r"(\w+)=([0-9/]+)", self.g.cmd("soundstate")))

    def sound_levels(self):
        """Audio > Music / Sound effects are the game's own levels (src/game/vcs_volume.cpp)."""
        g = self.g
        s = self.sound()
        self.ok("sound levels at the defaults", s.get("music") == "80" and s.get("effects") == "112", str(s))
        self.setting("audio.music_volume", 25)
        self.setting("audio.sfx_volume", 50)
        s = self.sound()
        prefs = g.peek(g.u32(0x48F3D0) + 0x38, 8).hex()  # the menu's prefs: effects, music (u32)
        self.ok("music / effects settings reach the game", s.get("music") == "32" and s.get("effects") == "64" and
                prefs == "4000000020000000", "%s prefs %s" % (s, prefs))
        # loading the card's save (it holds the defaults) keeps the player's levels: pause menu > Game > Load, slot 1
        self.keys("Escape@3", "Right", "Right", "Shift@2", "Shift@3", "Down", "Shift@3", "Up", "Shift@25")
        s = self.sound()
        with open(g.log_path, encoding="utf-8", errors="replace") as f:
            loaded = "the game sets the music level 80 (ra=0x3add90): the player's 32" in f.read()
        self.ok("a save load keeps the player's levels", loaded and s.get("music") == "32" and s.get("effects") == "64",
                "%s, load %s" % (s, "seen" if loaded else "not seen"))
        # the pause menu's Audio page shows them (2 and 4 bars) and its slider changes the setting
        self.keys("Escape@3", "Down@2", "Right@2")
        self.shot("game_audio_page")
        self.keys("Shift@2", "Left@2")
        s = self.sound()
        music = self.setting("audio.music_volume")
        self.shot("game_audio_music_down")
        self.ok("the pause menu's music slider changes the setting", s.get("music") == "16" and music == "13",
                "%s, setting %s" % (s, music))
        self.keys("f", "f@2")  # Triangle: back, close
        self.setting("audio.music_volume", 63)
        self.setting("audio.sfx_volume", 88)

    CAM = 0x6F44D0 + 0x70  # TheCamera's active CCam: +0 mode (4 on foot, 11 locked on, 18 vehicle), +0x78 pitch, +0x7C heading

    def cam(self):
        """(mode, pitch, heading) of the active camera."""
        pitch, heading = self.g.peekf(self.CAM + 0x78, 2)
        return self.g.u32(self.CAM), pitch, heading

    def mouse_moves(self, n, dx, dy):
        """Mouse motion in n small moves (as a real mouse sends it); (heading, pitch) turned once the camera settled."""
        _, p0, h0 = self.cam()
        for _ in range(n):
            self.g.cmd("mouse %d %d" % (dx, dy))
        self.g.wait_frames(20)
        _, p1, h1 = self.cam()
        return (h1 - h0 + math.pi) % (2 * math.pi) - math.pi, p1 - p0

    def mouse_and_driving(self):
        """The mouse camera on foot (heading written in the game, pitch through the stick), right-click lock-on, and the
        Driving keys (W / S / Space as Cross / Square / R1 while driving only), checked in the camera and the car."""
        g = self.g
        S = rig.Scm
        g.teleport(575.0, 175.0, -100.0, 0.0)  # a straight street (the camera starts at its lowest here)
        time.sleep(4.0)
        g.cmd("mouse 0 -150")
        g.wait_frames(30)
        mode = self.cam()[0]
        turn, up = self.mouse_moves(30, 10, -3)  # 400 counts a radian at 100%: -0.75 heading (right), +0.225 pitch (up)
        self.ok("mouse turns the camera on foot", mode == 4 and abs(turn + 0.75) < 0.03 and abs(up - 0.225) < 0.02,
                "mode %d heading %+.3f pitch %+.3f" % (mode, turn, up))
        # up / down at once as left / right: 120 counts up are +0.3 two frames later (through the stick's top speed and
        # the game's easing it was under 0.06) and stay there
        _, p0, _ = self.cam()
        g.cmd("mouse 0 -120")
        g.wait_frames(2)
        _, p1, _ = self.cam()
        g.wait_frames(20)
        _, p2, _ = self.cam()
        self.ok("mouse pitch at once", abs(p1 - p0 - 0.3) < 0.02 and abs(p2 - p1) < 0.02,
                "pitch %+.3f after 2 frames, %+.3f after 22" % (p1 - p0, p2 - p0))
        g.cmd("mouse 0 120")
        g.wait_frames(20)
        self.setting("mouse.invert_y", 1)
        _, up = self.mouse_moves(10, 0, -4)
        self.setting("mouse.invert_y", 0)
        self.ok("invert mouse up / down", abs(up + 0.1) < 0.01, "pitch %+.3f" % up)
        self.setting("mouse.camera", 0)
        turn, up = self.mouse_moves(10, 20, 5)
        self.setting("mouse.camera", 1)
        self.ok("mouse camera off", abs(turn) < 0.005 and abs(up) < 0.005, "heading %+.3f pitch %+.3f" % (turn, up))
        self.keys("Escape@3")  # the game's pause menu: the camera stays
        _, _, h0 = self.cam()
        g.cmd("mouse 400 0")
        time.sleep(1.0)
        _, _, h1 = self.cam()
        self.keys("Escape@3")
        self.ok("the mouse leaves the camera in the pause menu", abs(h1 - h0) < 0.001, "heading %.3f -> %.3f" % (h0, h1))

        # right click locks on to a ped next to Vic (the game's lock-on camera, mode 11), and the mouse does not break it
        _, (x, y, _), _ = g.player()
        g.scm(S.op(S.REQUEST_MODEL, S.i(55)) + S.op(S.LOAD_ALL_MODELS_NOW) +
              S.op(S.CREATE_CHAR, S.i(4), S.i(55), S.f(x + 2.0), S.f(y + 2.0), S.f(-100.0), S.LOCAL0))
        time.sleep(1.5)
        g.cmd("key mouseright down")
        time.sleep(1.0)
        locked = self.cam()[0]
        g.cmd("mouse 400 0")
        time.sleep(0.5)
        still = self.cam()[0]
        g.cmd("key mouseright up")
        time.sleep(1.0)
        self.ok("right click locks on", locked == 11 and still == 11 and self.cam()[0] == 4,
                "held %d, mouse moved %d, released %d" % (locked, still, self.cam()[0]))

        # driving: the Driving keys, and nothing else on those keys
        g.teleport(575.0, 175.0, -100.0, 0.0)
        time.sleep(3.0)
        g.spawn_car(ahead=0.0)
        time.sleep(3.0)
        ped = g.player()[0]
        car = g.u32(ped + 0x480)
        self.ok("in a car", car != 0 and g.u32(ped + 0x8C4) == 0x37 and self.cam()[0] == 18, "car 0x%x mode %d" % (car, self.cam()[0]))
        self.v_layout(car)
        got = {k: self.held(k) for k in ("w", "s", "space", "a", "shift", "mouseright")}
        want = {"w": CROSS, "s": SQUARE, "space": R1, "a": 0, "shift": CROSS, "mouseright": R1}
        bad = ["%s -> %s" % (k, p) for k, p in got.items() if p["buttons"] != want[k] or p["ly"] != 128 or
               p["lx"] != (0 if k == "a" else 128)]
        self.ok("driving keys", not bad, "; ".join(bad))
        self.setting("keys.drive_lean_forward", KEY_G)
        p = self.held("g")
        self.setting("keys.drive_lean_forward", 0)
        self.ok("lean forward rebound", p["ly"] == 0 and p["buttons"] == 0, str(p))

        def forward():
            v, f = g.peekf(car + 0x140, 3), g.peekf(car + 0x10, 3)
            return sum(a * b for a, b in zip(v, f))
        g.cmd("key w down")
        time.sleep(2.0)
        fast = forward()
        g.cmd("key w up")
        g.cmd("key space down")
        time.sleep(1.6)
        stopped = forward()
        g.cmd("key space up")
        self.ok("W drives, Space is the handbrake", fast > 0.15 and abs(stopped) < 0.05, "%.3f then %.3f" % (fast, stopped))
        # the camera behind the car (src/game/vcs_carcam.cpp): the mouse turns it itself (400 counts a radian), it stays
        # where it is put while the mouse moves - standing and driving -, and Car camera return (1.5 s) after the mouse
        # stops it eases back behind the car
        def angle(a, b):
            return (b - a + math.pi) % (2 * math.pi) - math.pi

        def behind():
            """The camera's heading behind the car: opposite to its motion, its nose when slow (vcs_carcam.cpp)."""
            vx, vy = g.peekf(car + 0x140, 2)
            if math.hypot(vx, vy) <= 0.02:
                vx, vy = g.peekf(car + 0x10, 2)
            return math.atan2(-vy, -vx) % (2 * math.pi)
        time.sleep(2.0)  # the car settles after the handbrake
        _, p0, h0 = self.cam()
        for _ in range(10):
            g.cmd("mouse 20 -4")
        g.wait_frames(3)
        _, p1, h1 = self.cam()
        held = g.cmd("carcam")
        time.sleep(0.8)
        _, p2, h2 = self.cam()
        self.ok("mouse turns the vehicle camera, and it stays", abs(angle(h0, h1) + 0.5) < 0.01 and
                abs(p1 - p0 - 0.1) < 0.01 and abs(angle(h1, h2)) < 0.005 and abs(p2 - p1) < 0.005 and held.endswith("state=hold"),
                "heading %+.3f pitch %+.3f, 0.8 s later %+.3f %+.3f, %s" % (angle(h0, h1), p1 - p0, angle(h1, h2), p2 - p1, held))
        time.sleep(3.5)
        _, p3, h3 = self.cam()
        state = g.cmd("carcam")
        self.ok("the vehicle camera goes back behind the car", abs(angle(behind(), h3)) < 0.03 and abs(p3 - p0) < 0.02 and
                state.endswith("state=idle"), "heading %+.3f from behind, pitch %+.3f, %s" % (angle(behind(), h3), p3 - p0, state))
        self.setting("mouse.car_sensitivity", 200)  # on top of Mouse sensitivity: twice as far
        _, _, h0 = self.cam()
        for _ in range(10):
            g.cmd("mouse 20 0")
        g.wait_frames(3)
        _, _, h1 = self.cam()
        self.setting("mouse.car_sensitivity", 100)
        self.ok("car camera sensitivity", abs(angle(h0, h1) + 1.0) < 0.01, "200 counts at 200%%: %+.3f" % angle(h0, h1))
        # the gamepad's right stick right after the mouse turns it at once (the game's camera again, no hold)
        g.cmd("gamepad on")
        g.cmd("gamepad rx 1")
        time.sleep(0.5)
        g.cmd("gamepad rx 0")
        g.wait_frames(3)
        _, _, h2 = self.cam()
        state = g.cmd("carcam")
        g.cmd("gamepad rx -1")  # and back (standing, the game's camera stays where the stick put it)
        time.sleep(0.5)
        g.cmd("gamepad off")
        g.cmd("mouse 1 0")  # the mouse's return puts it right behind the car again
        self.ok("the right stick takes the car camera back from the mouse", angle(h1, h2) < -0.3 and
                state.endswith("state=idle"), "0.5 s of full right: %+.3f, %s" % (angle(h1, h2), state))
        time.sleep(4.0)
        g.cmd("key w down")
        time.sleep(1.5)
        _, _, h0 = self.cam()
        for _ in range(20):
            g.cmd("mouse 20 0")
        g.wait_frames(3)
        _, _, h1 = self.cam()
        time.sleep(0.8)
        _, _, h2 = self.cam()
        time.sleep(1.0)
        _, _, h3 = self.cam()  # on the way back
        time.sleep(3.0)
        _, _, h4 = self.cam()
        back = angle(behind(), h4)
        g.cmd("key w up")
        self.ok("driving, the mouse's view stays, then goes back slowly",
                abs(angle(h0, h1) + 1.0) < 0.01 and abs(angle(h1, h2)) < 0.005 and -0.9 < angle(h0, h3) < -0.1 and
                abs(back) < 0.05, "heading %+.3f, 0.8 s %+.3f, 1.8 s %+.3f, 4.8 s %+.3f from behind" %
                (angle(h0, h1), angle(h0, h2), angle(h0, h3), back))

        # the mouse wheel on the radio (game-internals.md 12a "The radio"): a notch down is the next station, a notch up
        # the one before (the vehicle's +0x2BA: 0..8 the stations, 9 off). The game drops L1 while the radio plays an
        # announcement (15 - 40 s after getting in, in this save), so a step is waited for.
        station = lambda: g.peek(car + 0x2BA, 1)[0]
        s0, t0, took = station(), time.time(), False
        while time.time() - t0 < 80 and not took:
            g.cmd("press wheeldown 100")
            time.sleep(2.0)
            took = station() == (s0 + 1) % 10
            if not took:
                time.sleep(4.0)
        g.cmd("press wheelup 100")
        time.sleep(2.0)
        s1 = station()
        g.cmd("press wheelup 100")
        time.sleep(2.0)
        s2 = station()
        self.ok("wheel: radio stations", took and s1 == s0 and s2 == (s0 + 9) % 10,
                "from %d: down %s, up %d, up %d" % (s0, "took" if took else "not taken in 80 s", s1, s2))

        # out of the car: the on-foot keys and the mouse look again
        g.cmd("key space down")
        time.sleep(1.0)
        g.cmd("key space up")
        g.cmd("press f 300")
        t0 = time.time()
        while time.time() - t0 < 8 and (g.u32(ped + 0x8C4) == 0x37 or self.cam()[0] != 4):
            time.sleep(0.25)
        time.sleep(3.0)  # the camera's move back behind Vic sets its heading itself meanwhile
        turn, _ = self.mouse_moves(30, 10, 0)
        p = self.held("w")
        self.ok("on foot again", self.cam()[0] == 4 and abs(turn + 0.75) < 0.03 and p["ly"] == 0 and p["buttons"] == 0,
                "mode %d heading %+.3f, W %s" % (self.cam()[0], turn, p))

    # ---- the mouse's turn at a wall on foot (src/game/vcs_input.cpp, game-internals.md 12b) ----
    BARRACKS_DOOR = (-1648.87, -261.0)  # a wall right behind Vic here (rig.Game.BARRACKS_DOOR)

    def foot_wall(self):
        """At a wall the game pulls the on-foot camera in toward Vic only for a turned stick; the mouse's turn takes
        that path too. Where the stick's camera is pulled in (the reference), the mouse's is, and it stands where its
        heading says - not at the wall's edge while the heading turns on behind the wall (the user's report)."""
        g = self.g

        def angle(a, b):
            return (b - a + math.pi) % (2 * math.pi) - math.pi

        def beta():
            return g.peekf(self.CAM + 0x7C, 1)[0]

        def stands():
            """(where the camera really stands seen from the point it looks at, how far from it)"""
            s, t = g.peekf(self.CAM + 0x20, 3), g.peekf(self.CAM + 0x1A0, 3)
            return math.atan2(s[1] - t[1], s[0] - t[0]), math.dist(s, t)

        def level(x, y):
            """Vic there, the camera level behind him: after a teleport from far away it looks straight down from above
            him (L1 is the game's "camera behind him")."""
            g.teleport(x, y, -100.0, 0.0)
            time.sleep(3.0)
            g.cmd("press q 1000")
            time.sleep(2.0)

        # the game's far view (Select steps through three; the near one, 2 m, does not reach this wall), taken on
        # open ground where nothing pulls the camera in
        level(575.0, 175.0)
        zoom, steps = stands()[1], 0
        while stands()[1] < 4.0 and steps < 3:
            g.cmd("press backspace 150")
            time.sleep(1.5)
            steps += 1
        level(*self.BARRACKS_DOOR)
        path = os.path.join(self.out, "wall_orbit.txt").replace("\\", "/")
        g.cmd("trace %s 0x%x 1 0x%x 3 0x%x 3" % (path, self.CAM + 0x7C, self.CAM + 0x20, self.CAM + 0x1A0))
        g.cmd("gamepad on")
        g.cmd("gamepad rx 1")
        time.sleep(4.0)
        g.cmd("gamepad rx 0")
        g.cmd("gamepad off")
        g.wait_frames(5)
        g.cmd("trace off")
        orbit = [[float(v) for v in line.split()][3:] for line in open(path)]
        orbit = [(r[0], math.dist(r[1:4], r[4:7])) for r in orbit]
        full = max(d for _, d in orbit)
        near = [b for b, d in orbit if d < 0.8 * full]
        found = self.ok("a wall pulls the stick's camera in", len(near) > 5,
                        "%d of %d frames under %.2f m" % (len(near), len(orbit), 0.8 * full))
        wall = math.atan2(sum(math.sin(b) for b in near), sum(math.cos(b) for b in near)) if found else 0.0
        half = max(abs(angle(wall, b)) for b in near) if found else 0.0
        got = []
        for side in ((-1, 1) if found else ()):
            want = wall + side * (half + 0.5)  # outside the arc, then a radian across it
            for _ in range(6):
                d = angle(beta(), want)
                if abs(d) < 0.004:
                    break
                g.cmd("mouse %d 0" % max(-400, min(400, int(round(-d / 0.0025)))))
                g.wait_frames(8)
            time.sleep(0.6)
            b0 = beta()
            for _ in range(40):  # as a hand moves it: a few counts every game frame
                g.cmd("mouse %d 0" % (side * 10))
                g.wait_frames(1)
            g.wait_frames(10)
            at, dist = stands()
            got.append((angle(b0, beta()) + side * 1.0, angle(beta(), at), dist))
        self.ok("at a wall the camera follows the mouse and is pulled in",
                found and all(abs(t) < 0.01 and abs(o) < 0.02 and d < 0.9 * full for t, o, d in got),
                "; ".join("turn off by %+.3f, the camera %+.3f from its heading, %.2f of %.2f m" % (t, o, d, full)
                          for t, o, d in got))
        level(575.0, 175.0)
        while abs(stands()[1] - zoom) > 0.2 and steps < 6:  # the view as it was
            g.cmd("press backspace 150")
            time.sleep(1.5)
            steps += 1

    # ---- free aim (src/game/vcs_aim.cpp, game-internals.md 12c) ----
    PED_POOL, PED_SIZE = 0x487A94, 0xD20
    PED_TARGET, PED_HEALTH, PED_SLOT = 0x820, 0x4E4, 0x789

    def give(self, weapon, ammo=200):
        """The weapon to Vic, in his hand (GIVE_WEAPON_TO_CHAR selects it)."""
        self.g.scm(rig.Scm.op(rig.Scm.GIVE_WEAPON_TO_CHAR, rig.Scm.PLAYER, rig.Scm.i(weapon), rig.Scm.i(ammo)))
        time.sleep(1.0)

    def ped_at(self, x, y):
        """A civilian standing at (x, y) on the ground: its address."""
        S = rig.Scm
        ints, _ = self.g.scm_outputs(S.op(S.REQUEST_MODEL, S.i(55)) + S.op(S.LOAD_ALL_MODELS_NOW) +
                                     S.op(S.CREATE_CHAR, S.i(4), S.i(55), S.f(x), S.f(y), S.f(-100.0), S.LOCAL0), 1)
        return self.g.u32(self.g.u32(self.PED_POOL)) + (ints[0] >> 8) * self.PED_SIZE

    def ped_ahead(self, ahead, side=0.0):
        """A civilian `ahead` metres in front of Vic and `side` to his right."""
        ped = self.g.player()[0]
        (px, py), (fx, fy) = self.g.peekf(ped + 0x30, 2), self.g.peekf(ped + 0x10, 2)
        n = math.hypot(fx, fy) or 1.0
        fx, fy = fx / n, fy / n
        return self.ped_at(px + fx * ahead + fy * side, py + fy * ahead - fx * side)

    def aim(self):
        """(active, shots) from aimstate; the dummy's heading from Vic (radians)."""
        d = dict(re.findall(r"(\w+)=(\d+)", self.g.cmd("aimstate")))
        return int(d.get("active", 0)), int(d.get("shots", 0))

    def aim_heading(self):
        ped = self.g.player()[0]
        target = self.g.u32(ped + self.PED_TARGET)
        if not target:
            return None
        (tx, ty), (px, py) = self.g.peekf(target + 0x30, 2), self.g.peekf(ped + 0x30, 2)
        return math.atan2(ty - py, tx - px)

    def free_aim(self):
        """Free aim with the mouse and the gamepad (the game's own free aim, driven by the host), the scopes turned by
        the mouse, the wheel's weapons and zoom."""
        g = self.g
        g.teleport(575.0, 175.0, -100.0, 0.0)
        time.sleep(3.0)

        # fists keep the lock-on with free aim on, and with it off the pistol locks on as on the PS2 (a man 2.5 m ahead)
        for weapon, free in ((0, 1), (19, 0)):
            self.setting("mouse.free_aim", free)
            if weapon:
                self.give(weapon)
            else:
                for _ in range(10):  # R2 (3) until the fists are in his hand
                    if g.peek(g.player()[0] + self.PED_SLOT, 1)[0] == 0:
                        break
                    g.cmd("press 3 120")
                    time.sleep(0.6)
            self.ped_ahead(2.5)
            time.sleep(1.5)
            g.cmd("key mouseright down")
            time.sleep(1.0)
            target = g.u32(g.player()[0] + self.PED_TARGET)
            kind = g.u32(target + 0x48) & 0xE if target else 0
            self.ok("%s, free aim %s: lock-on" % ("fists" if not weapon else "pistol", "on" if free else "off"),
                    self.cam()[0] == 11 and kind == 6 and self.aim()[0] == 0, "mode %d target kind %d" % (self.cam()[0], kind))
            g.cmd("key mouseright up")
            time.sleep(1.5)

        self.setting("mouse.free_aim", 1)
        self.give(19)  # a pistol

        # right button: the game's free aim (camera 11 on the dummy object), R1 held and no L3 once in
        g.cmd("key mouseright down")
        time.sleep(1.0)
        active, shots0 = self.aim()
        target = g.u32(g.player()[0] + self.PED_TARGET)
        kind = g.u32(target + 0x48) & 0xE if target else 0
        p = self.pad()
        self.ok("free aim on the right button", active == 1 and self.cam()[0] == 11 and kind == 8 and p["buttons"] == R1,
                "active %d mode %d target kind %d buttons %04x" % (active, self.cam()[0], kind, p["buttons"]))

        # the mouse turns it: 400 counts a radian, the camera with it
        h0, c0 = self.aim_heading(), self.cam()[2]
        for _ in range(20):
            g.cmd("mouse 20 0")
        g.wait_frames(15)
        turn = (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi
        cturn = (self.cam()[2] - c0 + math.pi) % (2 * math.pi) - math.pi
        self.ok("the mouse turns the aim", abs(turn + 1.0) < 0.03 and abs(cturn + 1.0) < 0.15,
                "aim %+.3f camera %+.3f" % (turn, cturn))

        # shots go through the circle: a man on the aim line is hit, one 2 m beside it is not (the aim levelled first: it
        # starts where the camera looked, low here)
        ped = g.player()[0]
        (px, py, pz), (tx, ty, tz) = g.peekf(ped + 0x30, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
        g.cmd("mouse 0 %d" % round(math.atan2(tz - (pz + 0.6), math.hypot(tx - px, ty - py)) * 400))
        g.wait_frames(10)
        (px, py), (tx, ty) = g.peekf(ped + 0x30, 2), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 2)
        ux, uy = tx - px, ty - py
        n = math.hypot(ux, uy)
        ux, uy = ux / n, uy / n
        on = self.ped_at(px + ux * 9, py + uy * 9)
        off = self.ped_at(px + ux * 9 - uy * 2, py + uy * 9 + ux * 2)
        time.sleep(0.3)
        g.cmd("press mouseleft 120")
        time.sleep(0.8)
        hit, missed = g.peekf(on + self.PED_HEALTH, 1)[0], g.peekf(off + self.PED_HEALTH, 1)[0]
        shots = self.aim()[1] - shots0
        self.ok("the shot goes through the circle", hit < 100 and missed == 100 and shots >= 1,
                "on the line %.0f, beside it %.0f, %d shot(s) down the camera line" % (hit, missed, shots))

        # moving while aiming, as locked on with this gun (the pistol walks): the aim keeps its heading
        h0 = self.aim_heading()
        far = self.moved("d", 1.0)
        dh = (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi
        self.ok("moving while aiming", far > 1.0 and abs(dh) < 0.05 and self.aim()[0] == 1, "%.2f m, aim %+.3f" % (far, dh))

        # let go: the follow camera keeps looking where the aim did
        beta = self.cam()[2]
        g.cmd("key mouseright up")
        time.sleep(1.5)
        mode, _, after = self.cam()
        dh = (after - beta + math.pi) % (2 * math.pi) - math.pi
        self.ok("released: the camera stays", mode == 4 and abs(dh) < 0.05 and self.aim()[0] == 0,
                "mode %d heading %+.3f" % (mode, dh))

        self.setting("mouse.free_aim", 0)

        # the gamepad: Gamepad > Free aim, RB (R1) aims, the right stick turns it (2 radians a second at full tilt), the
        # left stick still moves Vic
        self.setting("pad.free_aim", 1)
        self.setting("pad.aim_assist", 0)
        g.cmd("gamepad on")
        g.cmd("gamepad rb 1")
        time.sleep(1.0)
        on_pad = self.aim()[0]
        h0 = self.aim_heading()
        g.cmd("gamepad rx 1")
        time.sleep(0.5)
        g.cmd("gamepad rx 0")
        time.sleep(0.3)
        turn = (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi
        before = g.player()[1]
        g.cmd("gamepad lx 1")
        time.sleep(1.0)
        g.cmd("gamepad lx 0")
        after = g.player()[1]
        g.cmd("gamepad rb 0")
        time.sleep(1.0)
        far = math.hypot(after[0] - before[0], after[1] - before[1])
        self.ok("gamepad free aim", on_pad == 1 and -1.4 < turn < -0.6 and far > 1.0, "aim %+.3f, moved %.2f m" % (turn, far))

        # aim assist (pad.aim_assist, on by default): RB locks on to the man ahead as the game's own aim, the right
        # stick then takes the aim free from there
        self.setting("pad.aim_assist", 1)
        self.ped_ahead(2.5)
        time.sleep(1.5)
        g.cmd("gamepad rb 1")
        time.sleep(1.0)
        target = g.u32(g.player()[0] + self.PED_TARGET)
        locked = (self.cam()[0], g.u32(target + 0x48) & 0xE if target else 0, self.aim()[0])
        g.cmd("gamepad rx 1")
        time.sleep(0.6)
        g.cmd("gamepad rx 0")
        time.sleep(0.4)
        target = g.u32(g.player()[0] + self.PED_TARGET)
        freed = (self.cam()[0], g.u32(target + 0x48) & 0xE if target else 0, self.aim()[0])
        g.cmd("gamepad rb 0")
        time.sleep(1.0)
        g.cmd("gamepad off")
        self.setting("pad.free_aim", 0)
        self.ok("aim assist: locks on, the right stick frees it", locked == (11, 6, 0) and freed == (11, 8, 1),
                "locked (mode, kind, free) %s, then %s" % (locked, freed))

        # the sniper rifle's scope: the mouse turns it directly (no stick speed), the wheel zooms
        self.give(30, 20)
        g.cmd("key mouseright down")
        time.sleep(1.2)
        mode, _, b0 = self.cam()
        for _ in range(20):
            g.cmd("mouse 20 0")
        g.wait_frames(10)
        dh = (self.cam()[2] - b0 + math.pi) % (2 * math.pi) - math.pi
        fov0 = g.peekf(self.CAM + 0x134, 1)[0]
        g.cmd("press wheelup 50")
        time.sleep(0.2)
        g.cmd("press wheelup 50")
        time.sleep(1.5)
        fov1 = g.peekf(self.CAM + 0x134, 1)[0]
        self.shot("scope_zoomed")
        g.cmd("key mouseright up")
        time.sleep(1.0)
        self.ok("the mouse turns the scope, the wheel zooms", mode == 7 and abs(dh + 1.0) < 0.01 and fov1 < fov0 * 0.7,
                "mode %d heading %+.3f, FOV %.1f -> %.1f" % (mode, dh, fov0, fov1))

        # the wheel on foot: down the next weapon, up the previous one (one each notch)
        self.give(24, 500)  # an SMG next to the pistol and the rifle
        slots = [g.peek(g.player()[0] + self.PED_SLOT, 1)[0]]
        for k in ("wheeldown", "wheelup", "wheelup"):
            g.cmd("press %s 50" % k)
            time.sleep(0.6)
            slots.append(g.peek(g.player()[0] + self.PED_SLOT, 1)[0])
        self.ok("the wheel changes weapon", slots[1] != slots[0] and slots[2] == slots[0] and slots[3] not in (slots[0], slots[1]),
                "slots %s" % slots)

        # a gun running dry while free aiming (people around): the game puts the fists in his hand; with the button
        # still held there is no lock-on (no R1 for the game) until it is let go, and the wheel's next gun aims again
        self.setting("mouse.free_aim", 1)
        self.give(19)
        ped = g.player()[0]
        slot = g.peek(ped + self.PED_SLOT, 1)[0]
        g.cmd("poke 0x%x %s" % (ped + 0x578 + slot * 0x1C + 8, (5).to_bytes(4, "little").hex() * 2))  # 5 rounds
        self.ped_ahead(4.0, 1.5)
        self.ped_ahead(5.0, -1.5)
        time.sleep(1.0)
        g.cmd("key mouseright down")
        time.sleep(0.6)
        g.cmd("key mouseleft down")
        time.sleep(2.5)
        g.cmd("key mouseleft up")
        time.sleep(0.5)
        dry = g.peek(ped + self.PED_SLOT, 1)[0], self.aim()[0], self.pad()["buttons"], g.u32(ped + self.PED_TARGET), self.cam()[0]
        for _ in range(4):  # the wheel to the SMG, the button still held
            if g.peek(ped + self.PED_SLOT, 1)[0] == 5:
                break
            g.cmd("press wheeldown 50")
            time.sleep(0.5)
        time.sleep(0.6)
        again = g.peek(ped + self.PED_SLOT, 1)[0], self.aim()[0], self.cam()[0]
        g.cmd("key mouseright up")
        time.sleep(1.0)
        self.setting("mouse.free_aim", 0)
        self.ok("a gun running dry while aiming: no lock-on, the next gun aims",
                dry == (0, 0, 0, 0, 4) and again == (5, 1, 11),
                "dry: slot %d aim %d buttons %04x target 0x%x mode %d; wheel: slot %d aim %d mode %d" % (dry + again))

    def shot_under_circle(self, ped, name):
        """A man under the circle (on the camera's line through it, 8 m past Vic) and one 2 m beside him, the aim tilted
        onto the first one's middle, the left button pressed: (health of the one under it, of the one beside it, shots
        sent down the camera line)."""
        g = self.g
        cam = self.CAM
        (sx, sy, sz), (dx, dy, dz) = g.peekf(cam + 0x20, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
        (px, py, _) = g.peekf(ped + 0x30, 3)
        h = math.hypot(dx - sx, dy - sy)
        ux, uy = (dx - sx) / h, (dy - sy) / h
        t = math.hypot(px - sx, py - sy) + 8.0
        on_ped = self.ped_at(sx + ux * t, sy + uy * t)
        off_ped = self.ped_at(sx + ux * t - uy * 2.0, sy + uy * t + ux * 2.0)
        time.sleep(0.5)
        for _ in range(3):  # the camera's line through his middle: the aim tilted until it does
            (sx, sy, sz), (dx, dy, dz) = g.peekf(cam + 0x20, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
            (qx, qy, qz) = g.peekf(on_ped + 0x30, 3)
            along = math.hypot(qx - sx, qy - sy)
            ray = sz + (dz - sz) * along / math.hypot(dx - sx, dy - sy)
            g.cmd("mouse 0 %d" % round(-math.atan2(qz - ray, along) * 400))
            g.wait_frames(10)
        shots0 = self.aim()[1]
        g.cmd("press mouseleft 300")
        time.sleep(1.0)
        hit, missed = g.peekf(on_ped + self.PED_HEALTH, 1)[0], g.peekf(off_ped + self.PED_HEALTH, 1)[0]
        self.shot(name)
        return hit, missed, self.aim()[1] - shots0

    def car_free_aim(self):
        """Free aim in a car: the game's own free aim in a mission's drive-by mode (SET_PLAYER_DRIVEBY_MODE, as Jive
        Drive sets it: Vic in the passenger seat with an SMG), the left stick turning it with free aim off (the PS2), the
        mouse with it on (the left stick then not), the game's height limits, shots through the circle."""
        g = self.g
        S = rig.Scm
        g.teleport(575.0, 175.0, -100.0, 0.0)
        time.sleep(3.0)
        g.cmd("trainer toggle god 1")
        self.give(25, 500)
        g.autodrive(speed=0.0)  # the car stands, an AI driver at the wheel
        time.sleep(2.0)
        g.scm(S.op(0x0442, S.PLAYER, S.i(1)) + S.op(0x0481, S.PLAYER, S.i(1)))  # SET_CHAR / SET_PLAYER_DRIVEBY_MODE
        time.sleep(2.0)
        ped = g.player()[0]
        target = g.u32(ped + self.PED_TARGET)
        kind = g.u32(target + 0x48) & 0xE if target else 0

        def turned(h0):
            return (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi

        def mouse_turn():
            h0 = self.aim_heading()
            for _ in range(20):
                g.cmd("mouse 20 0")
            g.wait_frames(15)
            return turned(h0)

        def stick_turn():
            h0 = self.aim_heading()
            g.cmd("key d down")
            time.sleep(1.0)
            g.cmd("key d up")
            g.wait_frames(5)
            return turned(h0)

        # free aim off: the game's (the left stick turns it, the mouse does not)
        self.setting("mouse.free_aim", 0)
        g.wait_frames(5)
        off = self.aim()[0], mouse_turn(), stick_turn()
        g.cmd("gamepad on")
        g.cmd("gamepad rb 1")
        rb_off = self.pad()["buttons"]
        g.cmd("gamepad rb 0")
        g.cmd("gamepad off")
        self.ok("in a car, free aim off: the gamepad's R1 is R1", rb_off == R1, "buttons %04x" % rb_off)
        self.ok("in a car, free aim off: the game's drive-by aim (left stick)",
                self.cam()[0] == 11 and kind == 8 and off[0] == 0 and abs(off[1]) < 0.02 and abs(off[2]) > 0.2,
                "mode %d target kind %d, active %d, mouse %+.3f, left stick %+.3f" % ((self.cam()[0], kind) + off))

        # free aim on: the mouse turns it (400 counts a radian) and the camera with it, the left stick does not
        self.setting("mouse.free_aim", 1)
        g.wait_frames(5)
        c0 = self.cam()[2]
        on = self.aim()[0], mouse_turn()
        cturn = (self.cam()[2] - c0 + math.pi) % (2 * math.pi) - math.pi
        still = stick_turn()
        p = self.held("d")
        self.ok("in a car, free aim on: the mouse aims", on[0] == 1 and abs(on[1] + 1.0) < 0.05 and abs(cturn + 1.0) < 0.15 and
                abs(still) < 0.01 and p["lx"] in (127, 128),
                "active %d, mouse %+.3f, camera %+.3f, left stick %+.3f (lx %d)" % (on + (cturn, still, p["lx"])))
        # and the gamepad's right stick (2 radians a second at full tilt), whichever device's setting is on
        g.cmd("gamepad on")
        h0 = self.aim_heading()
        g.cmd("gamepad rx 1")
        time.sleep(0.5)
        g.cmd("gamepad rx 0")
        time.sleep(0.3)
        pad = turned(h0)
        self.ok("in a car: the right stick aims", -1.4 < pad < -0.6, "aim %+.3f" % pad)
        # the gamepad's R1 (RB) fires there, as Circle (the right thumb is on the stick)
        shots0 = self.aim()[1]
        g.cmd("gamepad rb 1")
        rb = self.pad()["buttons"]
        time.sleep(1.0)
        g.cmd("gamepad rb 0")
        time.sleep(0.3)
        fired = self.aim()[1] - shots0
        g.cmd("gamepad off")
        self.ok("in a car: the gamepad's R1 fires", rb == CIRCLE and fired >= 1, "buttons %04x, %d shot(s)" % (rb, fired))

        # up and down within the game's limits there: 20 degrees above level, 5 below
        def elevation():
            (px, py, pz), (tx, ty, tz) = g.peekf(ped + 0x30, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
            return math.degrees(math.atan2(tz - pz, math.hypot(tx - px, ty - py)))
        g.cmd("mouse 0 -400")
        g.wait_frames(10)
        up = elevation()
        g.cmd("mouse 0 %d" % round(math.radians(20.0) * 400))  # level again
        g.wait_frames(10)
        # (seen from Vic: the game pulls the dummy along the camera's line onto what it hits, a degree or so either way)
        self.ok("in a car: the aim's height in the game's limits", 18.0 < up < 22.0, "%.1f deg up" % up)

        hit, missed, shots = self.shot_under_circle(ped, "car_free_aim")
        self.ok("in a car: the shot goes through the circle", hit < 100 and missed == 100 and shots >= 1,
                "under the circle %.0f, beside it %.0f, %d shot(s) down the camera line" % (hit, missed, shots))

        # out of the car: free aim ends with the game's (the drive-by modes switched off keep it until then - the game's
        # own doing, the same with VCS_FREEAIM=0)
        g.scm(S.op(0x0481, S.PLAYER, S.i(0)) + S.op(0x0442, S.PLAYER, S.i(0)))
        g.cmd("press f 300")
        t0 = time.time()
        while time.time() - t0 < 8 and g.u32(ped + 0x480):
            time.sleep(0.25)
        time.sleep(1.5)
        ended = self.aim()[0], self.cam()[0], self.pad()["lx"]
        self.setting("mouse.free_aim", 0)
        g.cmd("trainer toggle god 0")
        self.ok("in a car: leaving it ends it", ended[0] == 0 and ended[1] == 4 and not g.u32(ped + 0x480),
                "active %d mode %d" % ended[:2])

    def forced_free_aim(self):
        """The forced free aim on foot (SET_PLAYER_FORCE_FREE_AIMING, as Phil's shooting range sets it: a pistol, Vic
        standing): the left stick turns it with free aim off (the PS2), the mouse with it on (the left stick then not),
        the right button pressed and let go keeps it (our own free aim's R1 + L3 used to end it), the gamepad's R1
        fires, the game's limits (110 degrees either side, 40 up), shots through the circle, the script's end ends it."""
        g = self.g
        S = rig.Scm
        g.teleport(575.0, 175.0, -100.0, 0.0)
        time.sleep(3.0)
        self.give(19)
        g.scm(S.op(0x04D6, S.PLAYER, S.i(1)))  # SET_PLAYER_FORCE_FREE_AIMING
        time.sleep(1.5)
        ped = g.player()[0]
        target = g.u32(ped + self.PED_TARGET)
        kind = g.u32(target + 0x48) & 0xE if target else 0
        forced = bool(g.u32(ped + 0xCCC) & 0x200000)

        def turned(h0):
            return (self.aim_heading() - h0 + math.pi) % (2 * math.pi) - math.pi

        def mouse_turn(counts=400):
            h0 = self.aim_heading()
            for _ in range(20):
                g.cmd("mouse %d 0" % (counts // 20))
            g.wait_frames(15)
            return turned(h0)

        def stick_turn():
            h0 = self.aim_heading()
            g.cmd("key d down")
            time.sleep(0.5)
            g.cmd("key d up")
            g.wait_frames(5)
            return turned(h0)

        def aiming():
            target = g.u32(ped + self.PED_TARGET)
            return self.cam()[0] == 11 and target and g.u32(target + 0x48) & 0xE == 8

        # free aim off: the game's (the left stick turns it, the mouse does not)
        self.setting("mouse.free_aim", 0)
        g.wait_frames(5)
        off = self.aim()[0], mouse_turn(), stick_turn()
        self.ok("forced free aim, free aim off: the game's (left stick)",
                forced and self.cam()[0] == 11 and kind == 8 and off[0] == 0 and abs(off[1]) < 0.02 and abs(off[2]) > 0.2,
                "forced %d mode %d target kind %d, active %d, mouse %+.3f, left stick %+.3f" % ((forced, self.cam()[0], kind) + off))

        # free aim on: the mouse turns it (400 counts a radian) and the camera with it, the left stick does not
        self.setting("mouse.free_aim", 1)
        g.wait_frames(5)
        c0 = self.cam()[2]
        on = self.aim()[0], mouse_turn()
        cturn = (self.cam()[2] - c0 + math.pi) % (2 * math.pi) - math.pi
        still = stick_turn()
        p = self.held("d")
        self.ok("forced free aim: the mouse aims", on[0] == 1 and abs(on[1] + 1.0) < 0.05 and abs(cturn + 1.0) < 0.15 and
                abs(still) < 0.01 and p["lx"] in (127, 128),
                "active %d, mouse %+.3f, camera %+.3f, left stick %+.3f (lx %d)" % (on + (cturn, still, p["lx"])))

        # the right button held and let go: still the script's free aim (no R1 + L3 of ours), the mouse still aims
        g.cmd("key mouseright down")
        time.sleep(0.8)
        g.cmd("key mouseright up")
        time.sleep(1.0)
        after = self.aim()[0], aiming(), mouse_turn(-400)
        self.ok("forced free aim: the right button keeps it", after[0] == 1 and after[1] and abs(after[2] - 1.0) < 0.05,
                "active %d aiming %d, mouse %+.3f" % after)

        # the gamepad: the right stick aims, R1 (RB) fires as Circle (the right thumb is on the stick), as in the car
        g.cmd("gamepad on")
        h0 = self.aim_heading()
        g.cmd("gamepad rx 1")
        time.sleep(0.5)
        g.cmd("gamepad rx 0")
        time.sleep(0.3)
        pad = turned(h0)
        shots0 = self.aim()[1]
        g.cmd("gamepad rb 1")
        rb = self.pad()["buttons"]
        time.sleep(1.0)
        g.cmd("gamepad rb 0")
        time.sleep(0.5)
        fired = self.aim()[1] - shots0
        g.cmd("gamepad off")
        mouse_turn(round(pad * 400))  # back to where it was
        self.ok("forced free aim: the right stick aims, R1 fires", -1.4 < pad < -0.6 and rb == CIRCLE and fired >= 1 and aiming(),
                "aim %+.3f, buttons %04x, %d shot(s)" % (pad, rb, fired))

        # the game's limits on foot: 110 degrees either side of Vic's heading, 40 above level
        fx, fy = g.peekf(ped + 0x10, 2)
        heading = math.atan2(fy, fx)
        mouse_turn(1600)  # 4 radians to the right
        side = math.degrees((self.aim_heading() - heading + math.pi) % (2 * math.pi) - math.pi)
        mouse_turn(round(math.radians(side) * 400))  # back along his heading
        g.cmd("mouse 0 -800")
        g.wait_frames(10)
        (px, py, pz), (tx, ty, tz) = g.peekf(ped + 0x30, 3), g.peekf(g.u32(ped + self.PED_TARGET) + 0x30, 3)
        up = math.degrees(math.atan2(tz - (pz + 0.6), math.hypot(tx - px, ty - py)))
        g.cmd("mouse 0 %d" % round(math.radians(up) * 400))  # level again
        g.wait_frames(10)
        # (seen from Vic: the game pulls the dummy along the camera's line onto what it hits, a few degrees either way)
        self.ok("forced free aim: the game's limits", -112.0 < side < -106.0 and 38.0 < up < 42.0,
                "%.1f deg to the side, %.1f deg up" % (side, up))

        hit, missed, shots = self.shot_under_circle(ped, "forced_free_aim")
        self.ok("forced free aim: the shot goes through the circle", hit < 100 and missed == 100 and shots >= 1,
                "under the circle %.0f, beside it %.0f, %d shot(s) down the camera line" % (hit, missed, shots))

        # the script ends it (the range's last SET_PLAYER_FORCE_FREE_AIMING 0)
        g.scm(S.op(0x04D6, S.PLAYER, S.i(0)))
        time.sleep(1.5)
        ended = self.aim()[0], self.cam()[0]
        self.setting("mouse.free_aim", 0)
        self.ok("forced free aim: the script's end ends it", ended == (0, 4), "active %d mode %d" % ended)

    def mounted_gun(self):
        """A gun on a vehicle (ATTACH_CHAR_TO_CAR as Farewell To Arms attaches Vic to its helicopter's door: 1.6 1 0,
        facing its right, 70 degrees either side, an M249; here a car standing still): its first-person camera (mode 45)
        is the aim. Its heading is signed, the turn from the side Vic faces: a mouse turn to the right of the middle used
        to jump to the limit on the left (the user's report; the scope's mouse wrapped it into 0..2 pi). Free aim off: the
        mouse and the left stick turn it; on: the mouse and the right stick (the left stick centred), the gamepad's R1
        fires; the game's limit either side, a shot through the circle, the end of the attachment."""
        g = self.g
        S = rig.Scm
        # away from the car the drive-by check left at (575, 175): the new one landed on it and moved while aiming
        g.teleport(575.0, 125.0, -100.0, 0.0)
        time.sleep(3.0)
        g.cmd("trainer toggle god 1")
        _, (x, y, _), _ = g.player()
        g.scm(S.op(S.GIVE_WEAPON_TO_CHAR, S.PLAYER, S.i(34), S.i(999)) +
              S.op(S.REQUEST_MODEL, S.i(211)) + S.op(S.LOAD_ALL_MODELS_NOW) +
              S.op(S.CREATE_CAR, S.i(211), S.f(x + 4.0), S.f(y), S.f(-100.0), S.LOCAL0) +
              S.op(0x02B6, S.PLAYER, S.LOCAL0, S.f(1.6), S.f(1.0), S.f(0.0), S.i(3), S.f(70.0), S.i(34)) +  # ATTACH_CHAR_TO_CAR
              S.op(0x04D0, S.PLAYER, S.f(55.0)) + S.op(0x04CF, S.PLAYER, S.f(10.0)))  # its vertical limits
        time.sleep(2.0)
        ped = g.player()[0]
        limit = g.peekf(ped + 0x760, 1)[0]  # the attachment's limit either side (70 degrees)

        def cam():
            return 0x6F44D0 + 0x70 + g.peek(0x6F44D0 + 0x50, 1)[0] * 0x270

        def beta():
            g.wait_frames(5)
            return g.peekf(cam() + 0x7C, 1)[0]

        def mouse(counts):
            b0 = beta()
            for _ in range(10):
                g.cmd("mouse %d 0" % (counts // 10))
            return beta() - b0

        def stick(key):
            b0 = beta()
            g.cmd("key %s down" % key)
            time.sleep(0.5)
            g.cmd("key %s up" % key)
            return beta() - b0

        mode = g.u32(cam()) & 0xFFFF
        # free aim off: the scope's mouse (400 counts a radian) and the game's left stick
        self.setting("mouse.free_aim", 0)
        g.wait_frames(5)
        off = self.aim()[0], mouse(40), mouse(-80), stick("d")
        mouse(round(beta() * 400))  # to the middle
        self.ok("a gun on a vehicle, free aim off: the mouse turns it, no jump to the left",
                mode == 45 and abs(limit - math.radians(70.0)) < 0.01 and off[0] == 0 and abs(off[1] + 0.1) < 0.01 and
                abs(off[2] - 0.2) < 0.01 and off[3] < -0.2,
                "mode %d limit %.1f deg, active %d, mouse right %+.3f, left %+.3f, left stick %+.3f" % (
                    (mode, math.degrees(limit)) + off))

        # free aim on: the mouse and the right stick, not the left stick
        self.setting("mouse.free_aim", 1)
        g.wait_frames(5)
        on = self.aim()[0], mouse(40), stick("d")
        p = self.held("d")
        g.cmd("gamepad on")
        b0 = beta()
        g.cmd("gamepad rx 1")
        time.sleep(0.3)
        g.cmd("gamepad rx 0")
        pad = beta() - b0
        g.cmd("gamepad rb 1")
        rb = self.pad()["buttons"]
        g.cmd("gamepad rb 0")
        g.cmd("gamepad off")
        self.ok("a gun on a vehicle, free aim on: the mouse and the right stick aim, R1 fires",
                on[0] == 1 and abs(on[1] + 0.1) < 0.01 and abs(on[2]) < 0.005 and p["lx"] in (127, 128) and
                -0.9 < pad < -0.3 and rb == CIRCLE,
                "active %d, mouse %+.3f, left stick %+.3f (lx %d), right stick %+.3f, buttons %04x" % (
                    on + (p["lx"], pad, rb)))

        # the game's limit either side: far to the right stays on the right
        mouse(1600)
        right = beta()
        mouse(-3200)
        left = beta()
        mouse(round(left * 400))
        self.ok("a gun on a vehicle: the game's limit either side", abs(right + limit) < 0.01 and abs(left - limit) < 0.01,
                "%+.1f / %+.1f deg" % (math.degrees(right), math.degrees(left)))

        # a man 10 m along the camera's line, one 2 m beside him: the aim tilted onto the first one's middle, then fired
        (sx, sy, _), (fx, fy, _) = g.peekf(cam() + 0x20, 3), g.peekf(cam() + 0x10, 3)
        h = math.hypot(fx, fy)
        ux, uy = fx / h, fy / h
        on_ped = self.ped_at(sx + ux * 10.0, sy + uy * 10.0)
        off_ped = self.ped_at(sx + ux * 10.0 - uy * 2.0, sy + uy * 10.0 + ux * 2.0)
        time.sleep(1.0)
        for _ in range(3):  # the camera's line through his middle: turned and tilted until it does
            (sx, sy, sz), (fx, fy, fz) = g.peekf(cam() + 0x20, 3), g.peekf(cam() + 0x10, 3)
            (qx, qy, qz) = g.peekf(on_ped + 0x30, 3)
            along = math.hypot(qx - sx, qy - sy)
            side = (math.atan2(qy - sy, qx - sx) - math.atan2(fy, fx) + math.pi) % (2 * math.pi) - math.pi
            g.cmd("mouse %d %d" % (round(-side * 400),
                                   round(-(math.atan2(qz - sz, along) - math.atan2(fz, math.hypot(fx, fy))) * 400)))
            g.wait_frames(10)
        g.cmd("press mouseleft 300")
        time.sleep(1.0)
        hit, missed = g.peekf(on_ped + self.PED_HEALTH, 1)[0], g.peekf(off_ped + self.PED_HEALTH, 1)[0]
        self.shot("mounted_gun")
        self.ok("a gun on a vehicle: the shot goes through the circle", hit < 100 and missed == 100,
                "under the circle %.0f, beside it %.0f" % (hit, missed))

        # detached (DETACH_CHAR_FROM_CAR): free aim lets go
        g.scm(S.op(0x02B7, S.PLAYER))
        time.sleep(1.5)
        ended = self.aim()[0], g.u32(ped + 0x858), g.u32(cam()) & 0xFFFF
        self.setting("mouse.free_aim", 0)
        g.cmd("trainer toggle god 0")
        self.ok("a gun on a vehicle: detached, free aim lets go", ended[0] == 0 and not ended[1] and ended[2] != 45,
                "active %d attached %#x mode %d" % ended)

    def run(self):
        # the screenshot key's check counts the PNGs here: none left from an earlier run into the same OUT
        shutil.rmtree(os.path.join(self.out, "screenshot_key"), ignore_errors=True)
        g = self.g = rig.Game.start(save=rig.default_save(), name="controls_check",
                                    port=self.port, data_dir=os.path.join(ROOT, "build", "controls", "data"),
                                    env={"VCS_FPS": "30", "VCS_TIMING": "",  # timing unlocked: the presets set it
                                         "VCS_SCREENSHOT_DIR": os.path.join(self.out, "screenshot_key")})
        try:
            g.wait_ingame()
            # the player's own vcs.ini is read by hidden runs too (never written): start from the defaults
            self.ok("start from the defaults", self.setting("advanced.reset_all") == "pressed")
            g.prime()
            g.teleport(75.0, 175.0, -100.0)
            time.sleep(2)
            idle = self.pad()
            self.ok("idle report", idle.get("buttons") == 0 and all(idle.get(a) in (127, 128) for a in ("lx", "ly", "rx", "ry")), str(idle))

            # every default binding
            bad = []
            for key, want in DEFAULTS:
                p = self.held(key)
                if isinstance(want, int):
                    good = p["buttons"] == want
                else:
                    good = p[want[0]] == want[1] and p["buttons"] == 0
                if not good:
                    bad.append("%s -> %s" % (key, p))
            self.ok("default bindings (%d keys)" % len(DEFAULTS), not bad, "; ".join(bad))
            p = self.held("w", "d", "shift", "e")
            self.ok("keys together", p["ly"] == 0 and p["lx"] == 255 and p["buttons"] == CROSS | R1, str(p))

            # the game sees them: Vic runs forward
            far = self.moved("w")
            self.ok("W moves Vic", far > 1.0, "%.2f m" % far)

            # rebinding: forward on G, W does nothing then
            self.setting("keys.move_up", KEY_G)
            far_g = self.moved("g")
            far_w = self.moved("w")
            self.ok("rebound key moves Vic, the old one doesn't", far_g > 1.0 and far_w < 0.2, "G %.2f m, W %.2f m" % (far_g, far_w))
            self.setting("keys.move_up", 87)
            self.setting("keys.cross", KEY_G)
            p_g, p_shift = self.held("g"), self.held("shift")
            self.ok("Cross rebound", p_g["buttons"] == CROSS and p_shift["buttons"] == 0, "G %s, Shift %s" % (p_g, p_shift))
            self.setting("keys.cross", KEY_SHIFT)

            # walk: the stick at the walking speed while held
            self.setting("keys.walk", KEY_G)
            p = self.held("w", "g")
            want = round(128 - 0.45 * 128)
            self.ok("walk key", abs(p["ly"] - want) <= 1, "ly %d (want %d)" % (p["ly"], want))
            walk = self.setting("keys.walk", 0)
            self.ok("walk unbound", walk == "0", walk)

            self.mouse_and_driving()
            self.free_aim()
            self.weapon_triggers()
            self.car_free_aim()
            self.forced_free_aim()
            self.mounted_gun()

            # a wheel notch as L1 (second slot); the menu key keeps F2 (the menu refuses keys a control has)
            self.setting("keys.l12", KEY_WHEEL_UP)
            p = self.held("wheelup")
            self.ok("wheel binding", p["buttons"] == L1, str(p))
            self.setting("keys.l12", 0)

            # settings: presets and reset
            self.setting("presets.enhanced")
            got = {k: self.setting(k) for k in ("display.aspect", "display.hud", "display.msaa", "display.timing")}
            self.ok("Enhanced preset", got == {"display.aspect": "0", "display.hud": "2", "display.msaa": "4", "display.timing": "1"}, str(got))
            self.setting("presets.authentic")
            got = {k: self.setting(k) for k in ("display.aspect", "display.hud", "display.msaa", "display.timing", "display.fps")}
            self.ok("Authentic preset", got == {"display.aspect": "43", "display.hud": "0", "display.msaa": "1", "display.timing": "0",
                                                "display.fps": "30"}, str(got))
            self.setting("keys.cross", KEY_G)
            self.setting("audio.volume", 40)
            self.setting("pad.vibration", 0)
            self.setting("advanced.reset_all")
            got = {k: self.setting(k) for k in ("keys.cross", "audio.volume", "pad.vibration", "controls.menu_key")}
            self.ok("Reset all settings", got == {"keys.cross": str(KEY_SHIFT), "audio.volume": "100", "pad.vibration": "100",
                                                  "controls.menu_key": str(KEY_F2)}, str(got))
            p = self.held("shift")
            self.ok("bindings after the reset", p["buttons"] == CROSS, str(p))

            # the screenshot key (F8): a PNG of the window in VCS_SCREENSHOT_DIR (written on a thread of its own: the log
            # line says when), the pad left alone
            shots = os.path.join(self.out, "screenshot_key")
            p = self.held("f8")
            saved = None
            for _ in range(50):
                with open(g.log_path, encoding="utf-8", errors="replace") as f:
                    saved = re.search(r"\[screenshot\] (.+\.png) \((\d+)x(\d+)", f.read())
                if saved:
                    break
                time.sleep(0.1)
            pngs = [n for n in os.listdir(shots) if n.endswith(".png")] if os.path.isdir(shots) else []
            self.ok("screenshot key", saved is not None and len(pngs) == 1 and p["buttons"] == 0 and
                    os.path.getsize(os.path.join(shots, pngs[0])) > 10000,
                    "%s, %s, pad %s" % (saved.group(0) if saved else "no [screenshot] line", pngs, p))

            # the settings menu's pages
            for page in ("Display", "Graphics", "Audio", "Game", "Keyboard", "Gamepad", "Enhancements", "Advanced"):
                g.cmd("menu " + page)
                g.wait_frames(4)
                self.shot("menu_" + page.lower())
                if page == "Keyboard":  # its later sections
                    g.cmd("menu scroll 0.6")
                    g.wait_frames(4)
                    self.shot("menu_keyboard_lower")
            g.cmd("menu close")
            p = self.held("shift")
            self.ok("input back after the menu", p["buttons"] == CROSS, str(p))

            # the mouse's camera at a wall. Here, after the weapon checks, not among them: it moves Vic across the map
            # and back, the city around him loads anew, and with it before them the shot and wheel checks failed
            # now and then (a 50 ms wheel press lost to a long frame, a shot that missed its man)
            self.foot_wall()

            # vibration: an explosion next to Vic (the game shakes the pad; god mode so he lives)
            g.cmd("trainer toggle god 1")
            x, y, z = g.player()[1]
            g.scm(rig.Scm.op(0x013C, rig.Scm.f(x + 2.0), rig.Scm.f(y), rig.Scm.f(z), rig.Scm.i(0)))
            time.sleep(2.0)
            with open(g.log_path, encoding="utf-8", errors="replace") as f:
                log = f.read()
            m = re.search(r"\[input\] the game vibrates the pad \(small (\d+), large (\d+)\)", log)
            self.ok("the game's vibration reaches the host", m is not None, m.group(0) if m else "no vibration logged")
            self.ok("player bindings in use", "[input] player bindings" in log)
            # the sound levels with their save load here, after play in the city with god mode on: the load used to hang
            # on "Loading..." (the trainer cancelled a script the load had started in its slot, game-internals.md 11d)
            self.sound_levels()

            # Start last (it opens the pause menu); Alt+Enter is full screen, not Start
            p = self.held("alt", "return")
            self.ok("Alt+Enter is not Start", not p["buttons"] & START, str(p))
            for key in NOT_START_KEYS:
                p = self.held(key)
                self.ok("%s presses nothing outside the pause menu" % key, p["buttons"] == 0, str(p))
                time.sleep(0.5)
            # Escape is Start: held, the menu opens and has the key from then on (the report shows nothing after)
            self.held("escape")
            time.sleep(1.0)
            self.ok("escape is Start (the pause menu opens)", g.cmd("menustate").startswith("open=1"), g.cmd("menustate")[:40])
            self.ok("still running", g.alive())
        finally:
            g.quit()
        fails = [r for r in self.results if not r[1]]
        with open(os.path.join(self.out, "summary.txt"), "w", encoding="utf-8") as f:
            for name, good, detail in self.results:
                f.write("%-4s %s %s\n" % ("ok" if good else "FAIL", name, detail))
            f.write("%d checks, %d failed\n" % (len(self.results), len(fails)))
        print("%d checks, %d failed -> %s" % (len(self.results), len(fails), self.out))
        return not fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--port", type=int, default=47673)
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "controls", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    sys.exit(0 if Check(out, a.port).run() else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Purple Haze (mission 94, GON_C3) check: the cutscene that stuck on "Loading..." above 30 fps (GitHub issue 41), and
the haze that left the sides of a widescreen picture clear (issue 93). Fixed 2026-11-10.

    python tools/haze_check.py [OUT] [--port 47633] [--timing 0|1] [--fps 60]

From the Fort Baxter card at 16:9 with the HUD at the screen edges, original timing at 60 fps (where it stuck every
time): start the mission, go to the lock-up, take the van to the meeting place and watch the truck ram it. Passes when
- the mission gets past the cutscene (its state $G1504 reaches 60: Vic comes to in the haze) without sitting in the
  main script's wait for an audio line to load (0xA9F9 - 0xAA12),
- neither thug was hurt (the truck is parked before they are created: `[script-fix] Purple Haze` in the log),
- every piece of the haze is drawn across the whole picture (its HUD anchor is "stretch") while the clock and weapon
  stay at the right edge (VCS_TRACE_HUDANCHOR; grouped with the haze they went to the middle).
Results: OUT/result.json and screenshots (default OUT: build/haze_check/<date-time>). Exit code 0 = pass.

What broke (docs/game-internals.md 11h, 6f "Tunnel vision"): with a frame shorter than the PS2's the truck's hit turns
the van instead of pushing it, the truck rolls on and over the spot where the first thug is created; his dialogue
line's slot is freed with him and the mission waited for the line to load for ever, behind the faded-out screen.
VCS_SCRIPT_FIXES=0 / VCS_HAZE_FILL=0 bring the old behaviour back (the check then fails).
"""
import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hud_check  # noqa: E402  (read_anchors)
import rig  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT_SPACE = 0x1DE0800           # MAIN.SCM in memory: global n is at SCRIPT_SPACE + 4 n (game-internals.md 11b)
STATE = 0x1504                     # the mission's state: 15 lock-up, 20 van, 40 drive, 50 the cutscene, 60 the haze
LOCKUP = (0x159D, 0x15A0, 0x15A3)  # globals: the lock-up's marker
MEETING = (0x159B, 0x159E, 0x15A1)
VAN, THUGS = 0x1513, (0x1602, 0x1603)
AUDIO_WAIT = (0xA9F9, 0xAA12)      # main script: wait until a mission audio slot is loaded
PED_POOL, PED_SIZE, PED_HEALTH = 0x487A94, 0xD20, 0x4E4
PART_SCREEN = 4                    # vcs_hud.cpp Part::Screen: a piece of a screen effect


def check(out, port=None, timing="0", fps="60", env=None):
    os.makedirs(out, exist_ok=True)
    S = rig.Scm
    e = {"VCS_TIMING": timing, "VCS_FPS": fps, "VCS_ASPECT": "169", "VCS_HUD": "2", "VCS_TRACE_HUDANCHOR": "1000000",
         "VCS_TRACE_IM2D_DELAY": "0"}
    e.update(env or {})
    g = rig.Game.start(save=rig.default_save(), name="haze_check", port=port, env=e)
    gi = lambda n: struct.unpack("<i", g.peek(SCRIPT_SPACE + 4 * n, 4))[0]
    gf = lambda n: g.peekf(SCRIPT_SPACE + 4 * n, 1)[0]
    gv = lambda n: bytes([0xCD + (n >> 8), n & 0xFF])  # a global as a script argument

    def wait_state(want, seconds):
        t0 = time.time()
        while time.time() - t0 < seconds:
            if gi(STATE) == want:
                return True
            time.sleep(0.5)
        return False

    def ped(handle):
        """a char handle's ped, 0 when there is none (game-internals.md 12: pools)"""
        if handle <= 0:
            return 0
        pool = g.u32(PED_POOL)
        slot = handle >> 8
        return g.u32(pool) + slot * PED_SIZE if g.peek(g.u32(pool + 4) + slot, 1)[0] == (handle & 0xFF) else 0

    def car_to(x, y, z):
        g.scm(S.op(S.LOAD_SCENE, S.f(x), S.f(y), S.f(z)))
        car = g.u32(g.player()[0] + 0x450)
        for _ in range(3):
            g.cmd("pokef 0x%x 1 0 0 0 0 1 0 0 0 0 1 0 %f %f %f" % (car, x, y, z))
            g.cmd("pokef 0x%x 0 0 0 0 0 0 0 0" % (car + 0x140))
            time.sleep(0.05)

    res = {"timing": timing, "fps": fps, "reached": [], "audio_wait_s": 0.0, "thug_health_min": None}
    try:
        g.wait_ingame()
        g.prime()
        g.mission(94)
        if wait_state(15, 90):  # the opening line plays first
            res["reached"].append(15)
            g.teleport(gf(LOCKUP[0]), gf(LOCKUP[1]), gf(LOCKUP[2]))
        if wait_state(20, 60):  # the lock-up's door has opened
            res["reached"].append(20)
            g.scm(S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, gv(VAN)))
        if wait_state(40, 30):
            res["reached"].append(40)
            x, y, z = (gf(n) for n in MEETING)
            car_to(x + 6.0, y - 6.0, z + 1.5)  # beside the marker first, then onto it
            time.sleep(3)
            car_to(x, y, z + 1.0)
            t0, waiting, health = time.time(), None, []
            while time.time() - t0 < 75:
                state = gi(STATE)
                if state == 60:
                    res["reached"] += [50, 60]
                    break
                ips = [ip for _, _, ip, mission in g.scripts() if mission]
                if any(AUDIO_WAIT[0] <= ip <= AUDIO_WAIT[1] for ip in ips):
                    waiting = waiting or time.time()
                    res["audio_wait_s"] = max(res["audio_wait_s"], round(time.time() - waiting, 1))
                    if time.time() - waiting > 15:
                        break  # stuck: a line is loaded in a second or two
                else:
                    waiting = None
                health += [g.peekf(p + PED_HEALTH, 1)[0] for p in (ped(gi(n)) for n in THUGS) if p]
                time.sleep(0.2)
            res["thug_health_min"] = min(health) if health else None
            g.cmd("screenshot " + os.path.join(out, "1_after_cutscene.png"))
        if 60 in res["reached"]:
            time.sleep(4)  # the fade in; the haze is on
            g.cmd("screenshot " + os.path.join(out, "2_haze.png"))
            time.sleep(2)
    finally:
        g.quit()
    with open(g.log_path, errors="replace") as f:
        res["truck_parked"] = sum(1 for line in f if line.startswith("[script-fix] Purple Haze"))
    anchors = hud_check.read_anchors(g.log_path)
    haze = [a for a in anchors if a[1] == PART_SCREEN]
    haze_frames = {a[0] for a in haze}
    right = {a[0] for a in anchors if a[0] in haze_frames and a[4] == "right"}
    # without the fix the quarters are ordinary HUD pieces: one element as wide as the screen, kept in the middle
    grouped = {a[0] for a in anchors if a[1] == 0 and a[2] <= 0 and a[3] >= 640 and a[4] == "centre"}
    res.update({"haze_pieces": len(haze), "haze_not_stretched": len([a for a in haze if a[4] != "stretch"]),
                "haze_frames": len(haze_frames), "haze_frames_hud_at_right_edge": len(right),
                "frames_with_screen_wide_centre_element": len(grouped)})
    res["pass"] = bool(60 in res["reached"] and res["audio_wait_s"] < 10 and res["thug_health_min"] is not None and
                       res["thug_health_min"] >= 100.0 and res["truck_parked"] >= 1 and res["haze_pieces"] > 0 and
                       res["haze_not_stretched"] == 0 and res["haze_frames_hud_at_right_edge"] >= res["haze_frames"] * 0.9)
    json.dump(res, open(os.path.join(out, "result.json"), "w"), indent=1)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--port", type=int)
    ap.add_argument("--timing", default="0", help="VCS_TIMING: 0 original (the default: where it stuck), 1 independent")
    ap.add_argument("--fps", default="60")
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "haze_check", time.strftime("%Y%m%d-%H%M%S")))
    res = check(out, a.port, a.timing, a.fps)
    print(json.dumps(res))
    print("PASS" if res["pass"] else "FAIL", "-", out)
    return 0 if res["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())

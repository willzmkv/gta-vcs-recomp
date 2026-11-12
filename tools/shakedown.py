#!/usr/bin/env python3
"""Shakedown (mission 50, MAR_A1) check: the shop entry that stuck on "Loading..." (fixed 2026-10-25).

    python tools/shakedown.py [OUT] [--port 47620]

From the Fort Baxter card: start the mission, skip the opening cutscene (a screenshot of it first: the cutscene where
the characters floated / sank above 100 fps), put Marty's truck on the shop marker, beat the two gang members who
start a fight there (their health set to 0), walk Vic through the shop door and look inside 5 - 10 s later (after that
the mission's fight in the shop starts). Passes when the interior shows (area 2, not the dark loading screen), the shopkeeper
(model 152) is alive and the mission script is not waiting for his audio line (the main-script loop at 0xa9f9 -
0xaa12). The game runs at 144 fps with frame-rate independent timing, as the user plays it. Results: OUT/result.json
and the screenshots (default OUT: build/shakedown/<date-time>). Exit code 0 = pass.

What broke (docs/game-internals.md 12): the recompiler tested the low word in compare-with-zero branches, so
CPed::ProcessControl read bit 31 of the ped flags as bFadeOut and deleted the shopkeeper the mission had just created;
the mission audio monitor then dropped his line and the mission waited for it forever.
"""
import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import tour  # noqa: E402  (picture_stats)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOP_MARKER = (-1005.5, -356.0)    # MAR_A1's first shop (globals 0x15E6..0x15E8 when "Head over to the shop" shows)
SHOP_DOOR = (-1009.25, -334.0, 10.6)
AUDIO_WAIT = (0xA9F9, 0xAA12)      # main script: wait until a mission audio slot is loaded
PED_POOL = 0x487A94
SHOPKEEPER_MODEL = 152
MARTY_MODEL = 142
PED_HEALTH = 0x4E4                 # float (found by SET_CHAR_HEALTH and a diff of the player's ped)


def car_to(g, x, y, z):
    """Move the player's vehicle upright to (x, y, z) after a scene load; z a little above the road, so it drops on its
    wheels (placed at road height it lands inside the road and flips)."""
    S = rig.Scm
    g.scm(S.op(S.LOAD_SCENE, S.f(x), S.f(y), S.f(z)))
    car = g.u32(g.player()[0] + 0x450)
    if not car:
        raise RuntimeError("the player is not in a vehicle")
    for _ in range(3):
        g.cmd("pokef 0x%x 1 0 0 0 0 1 0 0 0 0 1 0 %f %f %f" % (car, x, y, z))
        g.cmd("pokef 0x%x 0 0 0 0 0 0 0 0" % (car + 0x140))  # move and turn speed
        time.sleep(0.05)


def live_peds(g, model):
    """[(handle, ped address)] of the live peds of a model (game-internals.md 12: pools)."""
    pool = g.u32(PED_POOL)
    entries, flags, size = g.u32(pool), g.u32(pool + 4), g.u32(pool + 8)
    f = g.peek(flags, size)
    out = []
    for i in range(size):
        if not f[i] & 0x80:
            p = entries + i * 0xD20
            if struct.unpack("<h", g.peek(p + 0x56, 2))[0] == model:
                out.append(((i << 8) | f[i], p))
    return out


def beat_gang(g, near, radius=30.0):
    """Health 0 for the mission's peds (created by 2, +0x8F4) near `near` - the gang members who fight Vic outside the
    shop (model 94; the second turns up after the first) - but not Marty, the player or the dead. Returns how many."""
    pool = g.u32(PED_POOL)
    entries, flags, size = g.u32(pool), g.u32(pool + 4), g.u32(pool + 8)
    f = g.peek(flags, size)
    player = g.player()[0]
    n = 0
    for i in range(size):
        p = entries + i * 0xD20
        if f[i] & 0x80 or p == player or g.u32(p + 0x8F4) != 2 or g.u32(p + 0x8C4) in (0x39, 0x3A):
            continue
        if struct.unpack("<h", g.peek(p + 0x56, 2))[0] == MARTY_MODEL:
            continue
        x, y, _ = g.peekf(p + 0x30, 3)
        if (x - near[0]) ** 2 + (y - near[1]) ** 2 < radius * radius and g.peekf(p + PED_HEALTH, 1)[0] > 0:
            g.cmd("pokef 0x%x 0" % (p + PED_HEALTH))
            n += 1
    return n


def waiting_scripts(g):
    return [(name, ip) for _, name, ip, _ in g.scripts() if AUDIO_WAIT[0] <= ip < AUDIO_WAIT[1]]


def check(out, port=None):
    os.makedirs(out, exist_ok=True)
    shot = lambda n: (g.cmd("screenshot " + os.path.join(out, n + ".png")), tour.picture_stats(os.path.join(out, n + ".png")))[1]
    res, waits = {}, []
    g = rig.Game.start(save=rig.default_save(), name="shakedown", port=port,
                       env={"VCS_TIMING": "1", "VCS_FPS": "144"})
    try:
        g.wait_ingame()
        g.invincible(True)
        g.prime()
        g.mission(50)
        time.sleep(12)
        shot("1_cutscene")  # Vic and Marty by the trailer: on the ground, not above / below it
        time.sleep(8)
        g.cmd("press Shift 150")
        time.sleep(2)
        g.cmd("press Shift 150")
        time.sleep(6)
        car_to(g, SHOP_MARKER[0] + 5.5, SHOP_MARKER[1] - 6.0, 13.0)  # next to the marker first, then onto it
        time.sleep(5)
        car_to(g, SHOP_MARKER[0], SHOP_MARKER[1], 12.5)
        time.sleep(25)  # the scripted camera, Vic gets out, the gang members start a fight
        res["at_shop_mission_running"] = g.player()[2]
        shot("2_shop")
        res["gang_outside"] = 0
        for _ in range(6):
            res["gang_outside"] += beat_gang(g, SHOP_MARKER)
            g.health(200)
            time.sleep(2.5)
        time.sleep(4)
        S = rig.Scm
        g.scm(S.op(S.SET_CHAR_COORDINATES, S.PLAYER, S.f(SHOP_DOOR[0]), S.f(SHOP_DOOR[1]), S.f(SHOP_DOOR[2])))
        time.sleep(3)
        g.cmd("press W 700")  # into the door marker
        time.sleep(5)  # the loading screen is over in 1-3 s
        lumas, keepers = [], []
        for k in range(3):
            lumas.append(round(shot("3_inside%d" % k)[0], 1))
            waits.append(waiting_scripts(g))
            keepers.append(len(live_peds(g, SHOPKEEPER_MODEL)))
            time.sleep(2.5)
        res.update({"area": g.area(), "mission_running": g.player()[2], "luma": lumas, "shopkeepers": keepers,
                    "audio_wait": waits[-1]})
    finally:
        g.quit()
    res["pass"] = bool(res.get("at_shop_mission_running") and res.get("area") == 2 and min(res.get("luma", [0])) > 18
                       and min(res.get("shopkeepers", [0])) > 0 and waits and not all(waits))
    json.dump(res, open(os.path.join(out, "result.json"), "w"), indent=1)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--port", type=int)
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "shakedown", time.strftime("%Y%m%d-%H%M%S")))
    res = check(out, a.port)
    print(json.dumps(res))
    print("PASS" if res["pass"] else "FAIL", "-", out)
    return 0 if res["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())

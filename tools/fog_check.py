#!/usr/bin/env python3
"""Fog settings check (src/game/vcs_fog.cpp, game-internals.md 6t): Graphics > Fog scales the game's fog range, and the
game's own range goes through untouched at 100 %.

    python tools/fog_check.py [OUT] [--save DIR] [--port 47630]

A new game from an empty card (or the card in --save DIR; hidden). At each step the `fogstate` command gives the range
the game asked for and the one that went into the setter FUN_002F6008, and VU 53 (0x742B28) has to hold what the
setter makes of the latter: 53.w = -255 / (end - start), 53.z = (start / (end - start) + 1) * 255. Steps: 100 % (used
= the game's), Fog distance 40 % (both ends x 0.4), Fog start 0 % on top (start 0), back to 100 %. Results:
OUT/result.json and a screenshot per step (default OUT: build/fog_check/<date-time>). Exit code 0 = pass.
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE = re.compile(r"game=(-?[\d.]+),(-?[\d.]+) used=(-?[\d.]+),(-?[\d.]+)")


def close(a, b, rel=0.01, abs_=0.05):
    return abs(a - b) <= max(abs_, rel * abs(b))


def step(g, out, name, distance, start):
    g.cmd("setting fog.distance %d" % distance)
    g.cmd("setting fog.start %d" % start)
    g.wait_frames(10)
    m = STATE.search(g.cmd("fogstate"))
    bias, scale = g.peekf(0x742B28, 2)
    g.cmd("screenshot " + os.path.join(out, name + ".png"))
    gs, ge, us, ue = (float(v) for v in m.groups()) if m else (0.0, 0.0, 0.0, 0.0)
    k = distance / 100.0
    want_end = ge * k
    want_start = min(gs * k * start / 100.0, want_end * 0.95) if (distance, start) != (100, 100) else gs
    r = {"step": name, "distance": distance, "start": start, "game": [gs, ge], "used": [us, ue], "vu53": [bias, scale]}
    r["range_ok"] = bool(m) and ge > gs and close(ue, want_end) and close(us, want_start)
    # the setter's own arithmetic on what went in (the game's start drifts a little a frame: 1 % slack)
    r["vu53_ok"] = ue > us and close(-scale, 255.0 / (ue - us)) and close(bias, (us / (ue - us) + 1.0) * 255.0)
    return r


def check(out, save=None, port=None):
    os.makedirs(out, exist_ok=True)
    g = rig.Game.start(save=save, name="fog_check", port=port)
    try:
        if save:
            g.wait_ingame()
        else:
            g.new_game()
        steps = [step(g, out, "original", 100, 100), step(g, out, "distance40", 40, 100),
                 step(g, out, "distance40_start0", 40, 0), step(g, out, "back", 100, 100)]
    finally:
        g.quit()
    res = {"steps": steps, "pass": all(s["range_ok"] and s["vu53_ok"] for s in steps)}
    json.dump(res, open(os.path.join(out, "result.json"), "w"), indent=1)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--save")
    ap.add_argument("--port", type=int)
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "fog_check", time.strftime("%Y%m%d-%H%M%S")))
    res = check(out, a.save, a.port)
    for s in res["steps"]:
        print("%-18s game %s used %s vu53 %s -> %s" % (s["step"], s["game"], s["used"], s["vu53"],
                                                       "ok" if s["range_ok"] and s["vu53_ok"] else "FAIL"))
    print("PASS" if res["pass"] else "FAIL", "-", out)
    return 0 if res["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())

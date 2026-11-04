#!/usr/bin/env python3
"""Draw distance settings check (src/game/vcs_distance.cpp, game-internals.md 9h): Graphics > Distance and population >
View distance (0..100: 100 % .. 1000 %) scales the camera's LOD distance multiplier through CRenderer::ms_lodDistScale,
and 0 is the game's own.

    python tools/distance_check.py [OUT] [--save DIR] [--port 47630]

A new game from an empty card (or the card in --save DIR; hidden), the free camera over Vice Point beach looking along
it. At each step the `distancestate` command gives the scale written into 0x4887F8 and the camera's LOD /
generation multipliers (0x6F4CAC / 0x6F4CA0: LOD = generation x scale after CCamera::Process); the renderer's visible
entity list (count 0x4CD598) has to grow with the distance. Steps: 0 (100 %), 28 (352 %), back to 0. Results:
OUT/result.json and a screenshot per step (default OUT: build/distance_check/<date-time>). Exit code 0 = pass.
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
STATE = re.compile(r"view=(\d+) detail=\d+ scale=([\d.]+) lod=([\d.]+) gen=([\d.]+)")
SPOT = (575.0, 175.0)                          # the player, Vice Point beach
CAMERA = (575.0, 120.0, 25.0, 90.0, -8.0)      # x y z yaw pitch: along the beach, past the hotels
VISIBLE_COUNT = 0x4CD598                        # CRenderer's visible entities (the list at 0x742B80)


def close(a, b, rel=0.01):
    return abs(a - b) <= rel * max(abs(b), 1e-6)


def step(g, out, name, slider):
    """Graphics > Distance and population > View distance at `slider` (0..100: 100 % .. 1000 % of the game's)."""
    percent = 100 + 9 * slider
    g.cmd("setting world.view_distance %d" % slider)
    time.sleep(3)  # the newly near entities stream in
    counts = []
    for _ in range(5):
        g.wait_frames(5)
        counts.append(g.u32(VISIBLE_COUNT))
    m = STATE.search(g.cmd("distancestate"))
    g.cmd("screenshot " + os.path.join(out, name + ".png"))
    objects, scale, lod, gen = (float(v) for v in m.groups()) if m else (0.0, 0.0, 0.0, 0.0)
    r = {"step": name, "percent": percent, "scale": scale, "lod": lod, "gen": gen, "visible": max(counts)}
    r["ok"] = bool(m) and int(objects) == percent and close(scale, percent / 100.0) and close(lod, gen * scale)
    return r


def check(out, save=None, port=None):
    os.makedirs(out, exist_ok=True)
    # the population distance at the game's own whatever the player's vcs.ini says: LODDistMultiplier is the generation
    # multiplier times the scale only then (the population scales the generation multiplier's readers)
    g = rig.Game.start(save=save, name="distance_check", port=port, data_dir=os.path.join(out, "data"),
                       env={"VCS_POPULATION_DISTANCE": "0"})
    try:
        if save:
            g.wait_ingame()
        else:
            g.new_game()
        g.set_time(12)
        g.weather(0)
        g.invincible(True)
        for _ in range(3):
            g.teleport(SPOT[0], SPOT[1], -100.0)
            time.sleep(3)
        g.cmd("campose %.1f %.1f %.1f %.1f %.1f" % CAMERA)
        time.sleep(3)
        steps = [step(g, out, "original", 0), step(g, out, "view28", 28), step(g, out, "back", 0)]
        g.cmd("campose off")
    finally:
        g.quit()
    grows = steps[1]["visible"] > steps[0]["visible"]
    res = {"steps": steps, "visible_grows": grows, "pass": all(s["ok"] for s in steps) and grows}
    json.dump(res, open(os.path.join(out, "result.json"), "w"), indent=1)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--save")
    ap.add_argument("--port", type=int)
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "distance_check", time.strftime("%Y%m%d-%H%M%S")))
    res = check(out, a.save, a.port)
    for s in res["steps"]:
        print("%-12s %3d%%: scale %.3f lod %.3f gen %.3f, %d visible entities -> %s" % (
            s["step"], s["percent"], s["scale"], s["lod"], s["gen"], s["visible"], "ok" if s["ok"] else "FAIL"))
    print("visible entities grow with the distance:", "yes" if res["visible_grows"] else "NO")
    print("PASS" if res["pass"] else "FAIL", "-", out)
    return 0 if res["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())

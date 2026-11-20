#!/usr/bin/env python3
"""Game-thread benchmark: fixed spots from the Fort Baxter card, perf numbers per spot (milestone 7).

    python tools/bench.py [OUT] [--fps 60] [--name bench] [ENV=VAL ...]

From a copy of build/rig/saves/fortbaxter (or a new game from an empty card when there is none): noon, sunny, invincible; outside the barracks door, then teleports to the
busiest standing spots of the world tour (tools/tour.py: most game time / draws), each settled for a few seconds and
measured over ~8 s (4 perf readings); last an AI-driven car (rig autodrive) from the first city spot - the route is
the AI's, so that one varies more between runs. Prints a table (game fps, game / worker / GS stage / render ms, draws) and writes
OUT/bench.json (OUT defaults to build/bench/<name>). VCS_EXE=<path> runs another build (tools/run.py), e.g. the
instrumented one during tools/build.py --opt pgo. Extra ENV=VAL pairs go to the game (VCS_FPS defaults to --fps).
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = rig.ROOT
# (name, x, y, heading) - z found on the ground; picked from a world-tour run (2026-10-23)
SPOTS = [
    ("city_575_175", 575.0, 175.0, 0.0),
    ("city_-675_1425", -675.0, 1425.0, 90.0),
    ("city_1325_-325", 1325.0, -325.0, 180.0),
    ("city_-1425_-575", -1425.0, -575.0, 270.0),
]
KEYS = ("game_fps", "frame_ms", "game_ms", "render_ms", "worker_ms", "gs_ms", "iop_ms", "wait_ms", "draws")


def parse_perf(line):
    out = {}
    for item in line.split():
        key, sep, value = item.partition("=")
        if sep:
            try:
                out[key] = float(value)
            except ValueError:
                pass
    return out


def measure(g, readings=4, gap=2.0):
    rows = []
    for _ in range(readings):
        time.sleep(gap)
        rows.append(parse_perf(g.cmd("perf")))
    return {k: round(sum(r.get(k, 0.0) for r in rows) / len(rows), 2) for k in KEYS}


def run(out, fps, name, extra_env, settle=6.0):
    env = {"VCS_ASPECT": "43", "VCS_FPS": str(fps)}
    env.update(extra_env)
    # the Fort Baxter card copy when there is one; a new game from an empty card otherwise (a machine without the rig's
    # saves: the first mission runs meanwhile, the spots and the drive are the same)
    save = rig.default_save()
    have_save = os.path.isdir(os.path.join(save, "mc0"))
    if not have_save:
        print("[bench] no %s: starting a new game instead" % os.path.relpath(save, ROOT))
    g = rig.Game.start(save=save if have_save else None, name=name, port=47660,
                       data_dir=os.path.join(ROOT, "build", "bench", "data_" + name), env=env)
    results = {}
    try:
        if have_save:
            g.wait_ingame()
        else:
            g.new_game()
        g.set_time(12)
        g.weather(0)
        g.invincible()
        if have_save:
            g.prime()
        time.sleep(settle)
        results["fortbaxter"] = measure(g)
        for spot, x, y, heading in SPOTS:
            g.teleport(x, y, -100.0, heading=heading)
            time.sleep(settle)
            results[spot] = measure(g)
        g.teleport(SPOTS[0][1], SPOTS[0][2], -100.0, heading=SPOTS[0][3])
        time.sleep(3.0)
        g.autodrive(speed=40.0)
        time.sleep(settle)
        results["drive"] = measure(g, readings=6)
    finally:
        g.quit()
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "bench.json"), "w", encoding="utf-8") as f:
        json.dump({"fps": fps, "env": env, "results": results}, f, indent=1)
    print("%-18s %7s %7s %7s %7s %7s %7s %6s" % ("spot", "fps", "game", "worker", "gs", "render", "frame", "draws"))
    for spot, r in results.items():
        print("%-18s %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f %6.0f" % (spot, r["game_fps"], r["game_ms"], r["worker_ms"], r["gs_ms"],
                                                                  r["render_ms"], r["frame_ms"], r["draws"]))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--fps", type=int, default=60, help="the game's frame rate setting (VCS_FPS)")
    ap.add_argument("--name", default="bench")
    ap.add_argument("env", nargs="*", help="ENV=VAL for the game")
    a, rest = ap.parse_known_args()
    pairs = [p for p in (a.env + rest) if "=" in p]
    if a.out and "=" in a.out:
        pairs.append(a.out)
        a.out = None
    run(a.out or os.path.join(ROOT, "build", "bench", a.name), a.fps, a.name, dict(p.split("=", 1) for p in pairs))


if __name__ == "__main__":
    main()

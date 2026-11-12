#!/usr/bin/env python3
"""Drive loop (milestone 1 of NATIVE_PORT_ROADMAP.md): put the player in a car and drive it hard around the map to
stress streaming, collision and the renderer at speed.

    python tools/drive.py [OUT] [--seconds 120] [--model 211] [--spots all|N,...] [--hour 12]

For each start spot: teleport, spawn a car (game script commands), accelerate with the steering alternating every
few seconds; every 5 s a line with position, speed (distance per 5 s), frame time and the brightness probe's black /
dip frame counters, plus a screenshot. The game log's "[frames] dip" / "[frames] black run" lines say when the
picture went dark for a few frames. OUT defaults to build/drive/<date-time>.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Outdoor road spots seen in the first world tour (build/tour/full1).
SPOTS = [(-600.0, 600.0), (325.0, 425.0), (75.0, 175.0), (-1175.0, -325.0), (575.0, -75.0), (-1425.0, -825.0)]


def counters(g):
    p = g.cmd("perf")
    get = lambda k: float(re.search(k + r"=([\d.]+)", p).group(1)) if re.search(k + r"=([\d.]+)", p) else 0.0
    return {k: get(k) for k in ("frame_ms", "black_frames", "dip_frames", "game_fps")}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--seconds", type=float, default=120.0, help="per spot")
    ap.add_argument("--model", type=int, default=211)
    ap.add_argument("--spots", default="all")
    ap.add_argument("--hour", type=int, default=12)
    ap.add_argument("--auto", action="store_true", help="an AI driver drives, the player rides along (realistic)")
    ap.add_argument("--speed", type=float, default=30.0, help="cruise speed with --auto")
    ap.add_argument("--save", default=rig.default_save())
    args = ap.parse_args()
    out = os.path.abspath(args.out or os.path.join(ROOT, "build", "drive", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    spots = SPOTS if args.spots == "all" else [SPOTS[int(i)] for i in args.spots.split(",")]

    g = rig.Game.start(save=args.save, name="drive", seconds=len(spots) * args.seconds + 600)
    try:
        g.wait_ingame()
        g.invincible(True)
        g.prime()
        for si, (x, y) in enumerate(spots):
            g.set_time(args.hour, 0)
            g.teleport(x, y, -100.0)
            time.sleep(1.0)
            if args.auto:
                g.autodrive(args.model, args.speed)
            else:
                g.spawn_car(args.model)
            time.sleep(1.0)
            start = counters(g)
            if not args.auto:
                g.cmd("key Shift down")  # accelerate (Cross)
            t0 = time.time()
            last = g.player()[1]
            steer = None
            while time.time() - t0 < args.seconds:
                t = time.time() - t0
                want = None if args.auto else ("A" if int(t / 4) % 3 == 0 else ("D" if int(t / 4) % 3 == 1 else None))
                if want != steer:
                    if steer:
                        g.cmd("key %s up" % steer)
                    if want:
                        g.cmd("key %s down" % want)
                    steer = want
                time.sleep(1.0)
                if int(t) % 5 == 4:
                    pos = g.player()[1]
                    c = counters(g)
                    dist = sum((a - b) ** 2 for a, b in zip(pos, last)) ** 0.5
                    last = pos
                    g.cmd("screenshot " + os.path.join(out, "s%d_%03d.png" % (si, int(t))))
                    print("spot %d t=%3.0f pos=(%.0f, %.0f, %.0f) moved=%.0f frame_ms=%.1f black=%d dips=%d" %
                          (si, t, pos[0], pos[1], pos[2], dist, c["frame_ms"], c["black_frames"] - start["black_frames"],
                           c["dip_frames"] - start["dip_frames"]))
            for k in ("Shift", "A", "D"):
                g.cmd("key %s up" % k)
    finally:
        g.quit()
    log = open(g.log_path, errors="replace").read().splitlines()
    events = [l for l in log if l.startswith("[frames]")]
    print("%d brightness events in %s" % (len(events), g.log_path))
    for l in events[-30:]:
        print(" ", l)
    return 0


if __name__ == "__main__":
    sys.exit(main())

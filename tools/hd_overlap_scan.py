#!/usr/bin/env python3
"""The HD world drawing a second version of what is near the camera: pictures at radius 3 and 0, compared near the camera.

    python tools/hd_overlap_scan.py [OUT] [--step 250] [--hour 0] [--limit N] [--save DIR] [--at=X,Y ...] [--route R ...]

Over a grid of the map (the sea skipped: the player's height), at each point: the free camera 3 m above the ground,
looking 15 degrees down in four directions, a picture with Detailed city distance at ~720 m (world.hd_radius 3) and one
at Original (0), switched live. The extra cells only add the city far away, so near the camera the two must match: the
lower half of each picture (the ground and the buildings close by) is compared, and the pixels that differ are
counted. A neighbour's low-detail copy of a building over the detailed one (the player's flickering walls and
pavements, 2026-11-17; game-internals.md 9j) shows as a count there. Night by default (0:00), where it shows most.

Writes OUT/results.jsonl (one row a view: point, yaw, differing pixels), OUT/shots/<x>_<y>_<yaw>_hd{3,0}.png and
OUT/worst.txt (the views ranked). Look at the worst pairs: traffic passing between the two pictures, the radar, or a
player the game moved (into the sea) count too. First run (2026-11-17, night, --route havana_downtown.txt --route
city.txt --step 150): 200 views at 50 points, none of the worst a second version of a building.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

BOUNDS = (-2175.0, -1925.0, 1560.0, 1925.0)  # as tools/tour.py


def lower_diff(a_path, b_path, threshold=40):
    """Pixels of the lower half (below the HUD's radar band excluded: the bottom 15 %) that differ between two pictures."""
    w, h, ra = tc.read_png(a_path)
    _, _, rb = tc.read_png(b_path)
    bpp = len(ra[0]) // w
    n = 0
    for y in range(h // 2, int(h * 0.85), 2):
        pa, pb = ra[y], rb[y]
        for x in range(int(w * 0.2) * bpp, w * bpp, 2 * bpp): # (the radar's corner left out)
            if abs(pa[x] - pb[x]) + abs(pa[x + 1] - pb[x + 1]) + abs(pa[x + 2] - pb[x + 2]) > threshold:
                n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(rig.ROOT, "build", "hd_overlap", time.strftime("%Y%m%d-%H%M")))
    ap.add_argument("--step", type=float, default=250.0)
    ap.add_argument("--hour", type=int, default=0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--save", default=None, help="card copy to start from (default: rig.default_save())")
    ap.add_argument("--at", action="append", default=[], help="only this point, X,Y (repeatable: --at=-1188,-1214)")
    ap.add_argument("--route", action="append", default=[], help="points every --step m along a tools/routes file "
                                                                 "instead of the grid (repeatable)")
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)
    shots = os.path.join(a.out, "shots")
    os.makedirs(shots, exist_ok=True)
    if a.at:
        points = [tuple(float(v) for v in p.split(",")) for p in a.at]
    elif a.route:
        points = []
        for r in a.route:
            path = r if os.path.isfile(r) else os.path.join(rig.ROOT, "tools", "routes", r)
            pts = [tuple(float(v) for v in l.split()[:2]) for l in open(path) if l.strip() and not l.startswith("#")]
            carry = 0.0
            for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
                seg = ((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5
                t = carry
                while t < seg:
                    points.append((round(x0 + (x1 - x0) * t / seg), round(y0 + (y1 - y0) * t / seg)))
                    t += a.step
                carry = t - seg
    else:
        points = []
        y = BOUNDS[1]
        row = 0
        while y <= BOUNDS[3]:
            xs = []
            x = BOUNDS[0]
            while x <= BOUNDS[2]:
                xs.append(x)
                x += a.step
            points += [(x, y) for x in (xs if row % 2 == 0 else reversed(xs))]
            y += a.step
            row += 1
    g = rig.Game.start(save=a.save or rig.default_save(), name="hd_overlap", seconds=6 * 3600,
                       env={"VCS_PERF_OVERLAY": "0"})
    results = []
    try:
        g.wait_ingame(timeout=240)
        g.invincible(True)
        g.cmd("setting world.vehicle_density 0") # (the game's own numbers: the sliders' 0)
        g.cmd("setting world.pedestrian_density 0")
        done = 0
        for x, y in points:
            if a.limit and done >= a.limit:
                break
            g.set_time(a.hour)
            g.cmd("setting world.hd_radius 3")
            g.teleport(x, y, -100.0)
            time.sleep(5)
            _, (px, py, pz), _ = g.player()
            if pz < 4.0 or abs(px - x) > 60 or abs(py - y) > 60: # the sea, or the game moved the player (a door)
                continue
            done += 1
            views = []
            for yaw in (0, 90, 180, 270):
                g.cmd("campose %.2f %.2f %.2f %d -15" % (px, py, pz + 3.0, yaw))
                time.sleep(5.0 if yaw == 0 else 2.5) # (the first: "Free camera on" gone from the screen)
                views.append((yaw, os.path.join(shots, "%d_%d_%d_hd3.png" % (x, y, yaw)).replace("\\", "/")))
                g.cmd("screenshot " + views[-1][1])
            g.cmd("setting world.hd_radius 0")
            time.sleep(5)
            for yaw, hd3 in views:
                g.cmd("campose %.2f %.2f %.2f %d -15" % (px, py, pz + 3.0, yaw))
                time.sleep(2.0)
                hd0 = hd3.replace("_hd3.png", "_hd0.png")
                g.cmd("screenshot " + hd0)
                time.sleep(0.5)
                rec = {"point": [x, y], "ground": [round(px, 1), round(py, 1), round(pz, 1)], "yaw": yaw,
                       "differ": lower_diff(hd3, hd0)}
                results.append(rec)
                with open(os.path.join(a.out, "results.jsonl"), "a") as f:
                    f.write(json.dumps(rec) + "\n")
            g.cmd("campose off")
            print("[hd_overlap] %d,%d: %s" % (x, y, [r["differ"] for r in results[-4:]]), flush=True)
    finally:
        g.quit()
    worst = sorted(results, key=lambda r: -r["differ"])
    with open(os.path.join(a.out, "worst.txt"), "w") as f:
        for r in worst:
            f.write("%6d  %s yaw %d  ground %s\n" % (r["differ"], r["point"], r["yaw"], r["ground"]))
    print("[hd_overlap] %d views at %d points; the worst: %s (%s)" % (len(results), len(results) // 4,
                                                                      worst[:5] and [(r["point"], r["yaw"], r["differ"]) for r in worst[:5]], a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""World tour: teleport over a grid covering the map and check every point (milestone 1 of NATIVE_PORT_ROADMAP.md).

    python tools/tour.py [OUT] [--step 250] [--settle 2.5] [--hour 12] [--save DIR] [--limit N] [--every K]

At each point: teleport to the ground (LOAD_SCENE + SET_CHAR_COORDINATES with z = -100), let the area settle while
sampling the player's height, take a screenshot and the perf line. Each point is classified:

    ok          on the ground, picture looks normal
    water       in the sea (nothing under the ped: swimming)
    relocated   the game moved the player: map-edge clamp, or the point is on an entrance marker (doors warp
                the player, e.g. (-1200, -300) leads to the Fort Baxter barracks)
    falling     nothing under the ped and it sank far below sea level (no water / collision there)
    dark        all three pictures mostly black (black world, missing world, deep water)
    flicker     some of the three pictures black (the picture went black for a moment)
    slow        game frame time over budget (default 40 ms)
    error       the game stopped answering / crashed (it is restarted and the tour goes on)

Writes OUT/results.jsonl (one JSON object per point), OUT/map.png (points coloured by class, needs Pillow),
OUT/shots/<x>_<y>.png and OUT/summary.txt. Default OUT: build/tour/<date-time>. Starts from a copy of a memory card
(default build/rig/saves/fortbaxter) so the game auto-loads into gameplay.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# World extent: the game clamps teleports to about x -2178..1564, y ..1926 (first tour, 2026-10-22); the south edge
# is assumed symmetric. Inside that: all three islands and the sea around them.
BOUNDS = (-2175.0, -1925.0, 1560.0, 1925.0)
COLOURS = {"ok": (60, 180, 75), "flicker": (145, 30, 180), "water": (70, 130, 220), "relocated": (160, 160, 160), "falling": (230, 25, 75),
           "dark": (20, 20, 20), "slow": (245, 130, 48), "error": (255, 0, 255)}


def picture_stats(path):
    """(mean luma 0..255, fraction of near-black pixels) of the picture area, HUD margins excluded; None without
    Pillow."""
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(path).convert("L")
    w, h = im.size
    box = im.crop((int(w * 0.2), int(h * 0.25), int(w * 0.8), int(h * 0.8))).resize((120, 80))
    px = list(box.tobytes())
    return sum(px) / len(px), sum(1 for p in px if p < 12) / len(px)


def perf_fields(line):
    out = {}
    for item in line.split():
        if "=" in item:
            k, v = item.split("=", 1)
            try:
                out[k] = float(v)
            except ValueError:
                pass
    return out


def parse_gsdraws(reply):
    """The rig's `gsdraws` answer -> {"frames", "draws", "vertices", "kinds": [[draws, vertices, kind], ...]}: the draws
    still made from GS primitives per frame, by kind (gsHwDrawSourceReport in the renderer)."""
    out = {"frames": 0, "draws": 0.0, "vertices": 0.0, "kinds": []}
    for line in reply.split(" ;; "):
        w = line.split()
        if len(w) >= 6 and w[1] == "frames:":
            out["frames"], out["draws"], out["vertices"] = int(w[0]), float(w[2]), float(w[5])
        elif len(w) >= 5 and w[1] == "draws" and w[3] == "verts":
            out["kinds"].append([float(w[0]), float(w[2]), " ".join(w[4:])])
    return out


def summarize_gsdraws(recs, top=15):
    """Lines for a summary: the GS-path draw kinds over all records with a "gsdraws" entry, by mean draws per frame."""
    recs = [r["gsdraws"] for r in recs if r.get("gsdraws", {}).get("frames")]
    if not recs:
        return []
    kinds = {}
    for g in recs:
        for draws, verts, kind in g["kinds"]:
            k = kinds.setdefault(kind, [0.0, 0.0, 0])
            k[0] += draws
            k[1] += verts
            k[2] += 1
    n = len(recs)
    lines = ["GS-path draws per frame: mean %.1f, max %.1f (%d samples); kinds by mean draws per frame (seen in N samples):"
             % (sum(g["draws"] for g in recs) / n, max(g["draws"] for g in recs), n)]
    for kind, (draws, verts, seen) in sorted(kinds.items(), key=lambda kv: -kv[1][0])[:top]:
        lines.append("  %7.2f draws %8.1f verts  [%3d]  %s" % (draws / n, verts / n, seen, kind))
    return lines


def classify(rec, args):
    if rec.get("error"):
        return "error"
    tx, ty = rec["target"]
    x, y, z = rec["pos"]
    if abs(x - tx) > 30 or abs(y - ty) > 30:
        return "relocated"
    zs = rec["z_samples"]
    if zs and zs[-1] < -30 and len(zs) >= 3 and zs[-1] < zs[-2] < zs[-3]:
        return "falling"  # still dropping far below sea level (no water / collision there)
    if z < 1.5:
        return "water"
    dark = [bool(st) and (st[0] < 18 or st[1] > 0.7) for st in rec.get("pictures") or [rec.get("picture")]]
    if dark and all(dark):
        return "dark"
    if any(dark):
        return "flicker"
    if rec.get("perf", {}).get("frame_ms", 0) > args.slow_ms:
        return "slow"
    return "ok"


def draw_map(results, path, step):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return False
    x0, y0, x1, y1 = BOUNDS
    scale = 0.25
    W, H = int((x1 - x0) * scale) + 1, int((y1 - y0) * scale) + 1
    im = Image.new("RGB", (W, H), (235, 235, 235))
    d = ImageDraw.Draw(im)
    r = max(3, int(step * scale * 0.45))
    for rec in results:
        tx, ty = rec["target"]
        cx, cy = (tx - x0) * scale, (y1 - ty) * scale  # north up
        d.rectangle([cx - r, cy - r, cx + r, cy + r], fill=COLOURS.get(rec["class"], (0, 0, 0)))
    for i, (name, c) in enumerate(COLOURS.items()):
        d.rectangle([8, 8 + i * 16, 20, 20 + i * 16], fill=c)
        d.text((26, 8 + i * 16), name, fill=(0, 0, 0))
    im.save(path)
    return True


def start_game(args, n):
    g = rig.Game.start(save=args.save, name="tour%d" % n, seconds=6 * 3600)
    g.wait_ingame()
    g.invincible(True)
    g.prime()  # the safehouse script's one-time move back to the barracks door (rig.Game.prime)
    g.set_time(args.hour, 0)
    return g


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--step", type=float, default=250.0)
    ap.add_argument("--settle", type=float, default=2.5)
    ap.add_argument("--hour", type=int, default=12)
    ap.add_argument("--save", default=rig.default_save())
    ap.add_argument("--limit", type=int, default=0, help="stop after N points")
    ap.add_argument("--every", type=int, default=1, help="take every K-th point (quick subset)")
    ap.add_argument("--slow-ms", type=float, default=40.0)
    args = ap.parse_args()
    out = os.path.abspath(args.out or os.path.join(ROOT, "build", "tour", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(os.path.join(out, "shots"), exist_ok=True)

    x0, y0, x1, y1 = BOUNDS
    points = []
    y = y1
    row = 0
    while y >= y0:
        xs = []
        x = x0
        while x <= x1:
            xs.append(x)
            x += args.step
        points += [(x, y) for x in (xs if row % 2 == 0 else reversed(xs))]  # serpentine: short hops
        y -= args.step
        row += 1
    points = points[::max(1, args.every)]
    if args.limit:
        points = points[:args.limit]
    print("tour: %d points -> %s" % (len(points), out))

    results = []
    restarts = 0
    g = start_game(args, restarts)
    with open(os.path.join(out, "results.jsonl"), "w") as log:
        for i, (tx, ty) in enumerate(points):
            rec = {"target": [tx, ty]}
            try:
                if not g.alive():
                    raise RuntimeError("game exited")
                g.set_time(args.hour, 0)  # the clock runs on (1 game minute per second): keep it daytime
                t0 = time.time()
                g.teleport(tx, ty, -100.0)
                rec["teleport_s"] = round(time.time() - t0, 2)
                zs = []
                n = max(2, int(args.settle / 0.5))
                for _ in range(n):
                    time.sleep(args.settle / n)
                    zs.append(round(g.player()[1][2], 2))
                if zs and zs[-1] < -5:
                    # below sea level: a long drop into deep water plunges before it surfaces - watch a bit longer
                    for _ in range(6):
                        time.sleep(0.5)
                        zs.append(round(g.player()[1][2], 2))
                rec["z_samples"] = zs
                g.cmd("gsdraws reset")  # draw sources from here to the perf line
                ped, pos, _ = g.player()
                rec["pos"] = [round(v, 2) for v in pos]
                # three pictures 0.7 s apart: one dark picture is a flicker, three are a dark place
                pics = []
                for k in range(3):
                    shot = os.path.join(out, "shots", "%d_%d%s.png" % (tx, ty, "" if k == 0 else "_%d" % k))
                    g.cmd("screenshot " + shot)
                    pics.append(picture_stats(shot))
                    time.sleep(0.7)
                rec["picture"] = pics[0]
                rec["pictures"] = pics
                rec["perf"] = perf_fields(g.cmd("perf"))
                rec["gsdraws"] = parse_gsdraws(g.cmd("gsdraws reset"))
            except Exception as e:  # crash / hang: record, restart, go on
                rec["error"] = str(e)[:200]
                rec.setdefault("pos", [0, 0, 0])
                rec.setdefault("z_samples", [])
                g.quit()
                restarts += 1
                try:
                    g = start_game(args, restarts)
                except Exception as e2:
                    rec["error"] += " | restart failed: " + str(e2)[:100]
            rec["class"] = classify(rec, args)
            results.append(rec)
            log.write(json.dumps(rec) + "\n")
            log.flush()
            print("%3d/%d (%6.0f, %6.0f) %-9s pos=%s" % (i + 1, len(points), tx, ty, rec["class"], rec.get("pos")))
    g.quit()

    counts = {}
    for r in results:
        counts[r["class"]] = counts.get(r["class"], 0) + 1
    fts = sorted(r["perf"].get("frame_ms", 0) for r in results if r.get("perf"))
    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write("points %d, restarts %d\n" % (len(results), restarts))
        for k, v in sorted(counts.items()):
            f.write("%-10s %d\n" % (k, v))
        if fts:
            f.write("frame_ms median %.1f, p90 %.1f, max %.1f\n" % (fts[len(fts) // 2], fts[int(len(fts) * 0.9)], fts[-1]))
        for line in summarize_gsdraws(results):
            f.write(line + "\n")
    draw_map(results, os.path.join(out, "map.png"), args.step)
    print(open(os.path.join(out, "summary.txt")).read())
    return 0


if __name__ == "__main__":
    sys.exit(main())

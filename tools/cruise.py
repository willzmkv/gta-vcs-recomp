#!/usr/bin/env python3
"""Stutter rig: the player's car moved along a fixed route through the city at a fixed speed, every frame timed.

    python tools/cruise.py run [OUT] [--route R] [--speed 40] [--laps 1] [--fps 60] [--shots S] [ENV=VAL ...]
    python tools/cruise.py report perf.csv [--from S] [--to S] [--json OUT]
    python tools/cruise.py record ROUTE [--minutes 8] [--spot 0] [--speed 40] [--style 3]

run: from a copy of build/rig/saves/fortbaxter (else saves/user, a copy of the player's card; noon, sunny,
invincible), a car at the route's start, then the rig's `cruise` (src/debug/rig.cpp) puts the car each game frame at
the route's point `speed * step` further on, facing along it, with that move speed - the same path at the same pace
every run, so streaming, the camera and the renderer get the
same load and two builds (or two settings) can be compared. The game writes its per-frame CSV (PS2X_PERF_LOG) to
OUT/perf.csv; the report follows (OUT/stutter.json). Defaults: frame-rate independent timing at a 60 cap (what most
players run), the brightness probe off (PS2X_FRAME_STATS=0: its GPU read-back every frame would hide GPU-side
hitches). Extra ENV=VAL pairs go to the game, e.g. VCS_ASPECT=169 PS2X_HW_SCALE=3 PS2X_HW_MSAA=4 PS2X_HW_ANISO=8 (close
to issue #29's settings). --shots S: a screenshot every S s to look at the route (each one is a hitch of its own).

The default route (tools/routes/city.txt, "x y z" per line, straight lines between the points) is a low flight - 35 m,
over the roofs - through the busiest cells of the world tour on both sides of the bay, ~6 km (150 s at 40 m/s = 144
km/h): new areas stream in at driving speed, which is what makes texture bursts (issue #29). It is not on the roads:
the AI drivers the rig can make stood more than they drove (record).

report: hitches (a frame over twice the running median of the last 60 and 8 ms longer than it), frames over 25 / 33 /
50 / 100 ms, percentiles, and what each hitch had in its own and the two frames before: texture uploads, the GS stage's
decode time, the main thread's texture create time, its GPU submit time and its longest host frame.

record: a road route from AI drives (rig autodrive, the player as passenger): the car's position every 0.2 s; a car that
stands 30 s is replaced by a new one beside it. In practice the AI often stands (traffic, poles): short routes only.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_report  # noqa: E402
import rig  # noqa: E402

ROOT = rig.ROOT
DEFAULT_ROUTE = os.path.join(ROOT, "tools", "routes", "city.txt")
PED_VEHICLE = 0x480
# Start spots for recording: road spots from the world tour on both islands (tools/drive.py's), downtown first
RECORD_SPOTS = [(-600.0, 600.0), (325.0, 425.0), (75.0, 175.0), (-1175.0, -325.0), (575.0, -75.0), (-1425.0, -825.0),
                (575.0, 175.0)]


def load_route(path):
    pts = []
    with open(path) as f:
        for line in f:
            w = line.split()
            if len(w) >= 3 and not line.startswith("#"):
                pts.append(tuple(float(v) for v in w[:3]))
    return pts


def vehicle_pos(g):
    ped = g.player()[0]
    veh = g.u32(ped + PED_VEHICLE)
    if not veh:
        return None
    return tuple(g.peekf(veh + 0x30, 3))


# ---- report -------------------------------------------------------------------------------------------------------
def analyse(rows, window=60):
    """Hitches against a running median of the previous `window` frames."""
    hitches = []
    recent = []
    for i, r in enumerate(rows):
        ms = r["frame_ms"]
        if len(recent) >= 10:
            med = sorted(recent)[len(recent) // 2]
            if ms > 2.0 * med and ms > med + 8.0:
                prev = rows[max(0, i - 2):i + 1]
                hitches.append({"t": round(r["t_s"], 2), "ms": round(ms, 1), "median": round(med, 1),
                                "tex": int(sum(p.get("tex_uploads", 0) for p in prev)),
                                "draws": int(r.get("draws", 0)), "game_ms": round(r.get("game_ms", 0), 1),
                                "wait_ms": round(r.get("wait_ms", 0), 1), "gl_ms": round(max(p.get("gl_ms", 0) for p in prev), 1),
                                "host_max": round(max(p.get("host_max_ms", 0) for p in prev), 1),
                                "create_ms": round(sum(p.get("tex_create_us", 0) for p in prev) / 1000.0, 1),
                                "decode_ms": round(sum(p.get("tex_decode_us", 0) for p in prev) / 1000.0, 1)})
        recent.append(ms)
        if len(recent) > window:
            recent.pop(0)
    return hitches


def report(rows, out_json=None, label=""):
    ms = sorted(r["frame_ms"] for r in rows)
    secs = sum(ms) / 1000.0
    pct = lambda q: perf_report.pct(ms, q)  # noqa: E731
    hitches = analyse(rows)
    over = {k: sum(1 for v in ms if v > k) for k in (25, 33, 50, 100)}
    excess = sum(h["ms"] - h["median"] for h in hitches)
    tex_bins = []
    for lo in (0, 5, 20, 50):
        sel = [r for r in rows if r.get("tex_uploads", 0) >= lo]
        slow = sum(1 for r in sel if r["frame_ms"] > 20.0)
        tex_bins.append({"tex_at_least": lo, "frames": len(sel), "over_20ms": slow})
    s = {"label": label, "frames": len(rows), "seconds": round(secs, 1), "fps": round(len(rows) / secs, 1) if secs else 0,
         "p50": pct(.5), "p95": pct(.95), "p99": pct(.99), "p999": pct(.999), "max": ms[-1] if ms else 0,
         "over_ms": over, "hitches": len(hitches), "hitch_excess_ms_per_min": round(excess / secs * 60.0, 1) if secs else 0,
         "tex_uploads_per_s": round(sum(r.get("tex_uploads", 0) for r in rows) / secs, 1) if secs else 0,
         "tex_bins": tex_bins, "worst": sorted(hitches, key=lambda h: -h["ms"])[:25]}
    print("%s%d frames over %.1f s: %.1f fps  p50 %.1f  p95 %.1f  p99 %.1f  p99.9 %.1f  max %.1f ms" %
          (label + ": " if label else "", s["frames"], secs, s["fps"], s["p50"], s["p95"], s["p99"], s["p999"], s["max"]))
    print("frames over 25 / 33 / 50 / 100 ms: %d / %d / %d / %d;  hitches %d (%.1f ms lost a minute);  %.0f texture uploads/s" %
          (over[25], over[33], over[50], over[100], len(hitches), s["hitch_excess_ms_per_min"], s["tex_uploads_per_s"]))
    print("texture uploads per frame -> frames over 20 ms: " +
          ", ".join("%d+: %d/%d" % (b["tex_at_least"], b["over_20ms"], b["frames"]) for b in tex_bins))
    if hitches:
        print("worst hitches:   t      ms  median   tex  decode_ms  create_ms  draws  game  wait   gl  host_max"
              "  (tex, decode, create, gl, host: 3 frames)")
        for h in s["worst"][:12]:
            print("            %7.2f %6.1f %6.1f %5d %9.1f %9.1f %6d %5.1f %5.1f %5.1f %7.1f" %
                  (h["t"], h["ms"], h["median"], h["tex"], h.get("decode_ms", 0), h.get("create_ms", 0), h["draws"],
                   h["game_ms"], h["wait_ms"], h["gl_ms"], h.get("host_max", 0)))
    if out_json:
        with open(out_json, "w", encoding="utf-8") as f:
            json.dump(s, f, indent=1)
    return s


# ---- run ----------------------------------------------------------------------------------------------------------
def run(out, route, speed, laps, fps, extra_env, name, settle=4.0, shots=0.0, save_name=None, wait=0.0):
    out = os.path.abspath(out)
    pts = load_route(route)
    os.makedirs(out, exist_ok=True)
    csv_path = os.path.join(out, "perf.csv")
    if os.path.exists(csv_path):
        os.remove(csv_path)
    env = {"VCS_ASPECT": "43", "VCS_TIMING": "1", "VCS_FPS": str(fps), "PS2X_FRAME_STATS": "0",
           "PS2X_PERF_LOG": csv_path}
    env.update(extra_env)
    # the Fort Baxter card copy, else a copy of the player's card (build/rig/saves/user, copied from the live card - never
    # the live card itself). A new game does not do: its first mission keeps the player out of the cruise's car
    # (--save NAME: that copy - user for the player's own progress, the city's full population)
    saves = os.path.join(ROOT, "build", "rig", "saves")
    save = next((os.path.join(saves, s) for s in ((save_name,) if save_name else ("fortbaxter", "user"))
                 if os.path.isdir(os.path.join(saves, s, "mc0"))), None)
    if not save:
        sys.exit("[cruise] no card copy in %s (fortbaxter/ or user/, each holding mc0/): copy a card with a save "
                 "past the first mission there" % os.path.relpath(saves, ROOT))
    print("[cruise] from the card copy %s" % os.path.relpath(save, ROOT))
    g = rig.Game.start(save=save, name=name, port=47670, data_dir=os.path.join(out, "data"), env=env)
    run_secs = 0.0
    try:
        g.wait_ingame()
        g.set_time(12)
        g.weather(0)
        g.invincible()
        g.prime()
        x, y, z = pts[0]
        g.teleport(x, y, z)
        time.sleep(1.0)
        S = rig.Scm
        car = S.LOCAL0
        g.scm(S.op(S.REQUEST_MODEL, S.i(211)) + S.op(S.LOAD_ALL_MODELS_NOW) +
              S.op(S.CREATE_CAR, S.i(211), S.f(x), S.f(y), S.f(z + 1.0), car) +
              S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, car) + S.op(S.SET_CAR_PROOFS, car, *([S.i(1)] * 5)) +
              S.op(S.MARK_MODEL_AS_NO_LONGER_NEEDED, S.i(211)))
        if wait > 0:  # --wait: the population fills around the start first (it takes the game a minute or two)
            time.sleep(wait)
        reply = g.cmd("cruise start %s %.1f%s" % (os.path.abspath(route), speed, " loop" if laps > 1 else ""))
        total = float(reply.split()[1])
        mark = next_shot = time.time()
        drive_secs = total * laps / speed
        while time.time() - mark < drive_secs * 1.5 + 60.0:
            st = g.cmd("cruise status").split()
            if st[1] == "1" or (laps > 1 and time.time() - mark >= drive_secs):
                break
            if not g.alive():
                raise RuntimeError("the game exited during the cruise")
            if shots and time.time() >= next_shot:  # a GPU read-back each: a hitch of its own (not for measuring)
                g.cmd("screenshot " + os.path.join(out, "shot_%04.0f.png" % (time.time() - mark)))
                next_shot += shots
            time.sleep(1.0)
        run_secs = time.time() - mark
        g.cmd("cruise stop")
    finally:
        g.quit()
    rows = perf_report.load(csv_path, None, None)
    # from `settle` s after the cruise started: the CSV's clock starts with the first game frame; the cruise was the
    # last run_secs of it before the quit (about a second; its last frames are left out too)
    if rows:
        t_end = rows[-1]["t_s"]
        rows = [r for r in rows if t_end - run_secs - 1.0 + settle <= r["t_s"] <= t_end - 2.0]
    return report(rows, os.path.join(out, "stutter.json"), label=name)


def record(route, minutes, spot, speed=40.0, style=3):
    """AI drives one after another: when the car stands (traffic, a pole - tools/soak.py sees it too), a new AI car
    beside it carries on from there (a few metres' step in the route: the cruise goes straight from point to point)."""
    os.makedirs(os.path.dirname(os.path.abspath(route)), exist_ok=True)
    save = rig.default_save()
    g = rig.Game.start(save=save, name="cruise_record", port=47671,
                       data_dir=os.path.join(ROOT, "build", "cruise", "record_data"), env={"VCS_TIMING": "1", "VCS_FPS": "60"})
    pts = []
    try:
        g.wait_ingame()
        g.set_time(12)
        g.weather(0)
        g.invincible()
        g.prime()
        x, y = RECORD_SPOTS[spot]
        g.teleport(x, y, -100.0)
        time.sleep(2.0)
        t_end = time.time() + minutes * 60.0
        for car in range(60):
            if time.time() >= t_end:
                break
            g.autodrive(speed=speed, style=style, release=True)
            time.sleep(2.0)
            seg, still, t0 = [], 0, time.time()
            while time.time() < t_end:
                p = vehicle_pos(g)
                if p:
                    if not seg or abs(p[0] - seg[-1][0]) + abs(p[1] - seg[-1][1]) > 0.5:
                        seg.append(p)
                        still = 0
                    else:
                        still += 1
                if still > 150:  # 30 s standing (a red light, a jam: shorter)
                    break
                if time.time() - t0 > 25.0 and seg and max(abs(q[0] - seg[0][0]) + abs(q[1] - seg[0][1]) for q in seg) < 40.0:
                    break  # going nowhere (round in circles against something)
                time.sleep(0.2)
            pts += seg
            done = sum(abs(b[0] - a[0]) + abs(b[1] - a[1]) for a, b in zip(seg, seg[1:]))
            print("[cruise] car %d: %d points, ~%.0f m, now at (%.0f, %.0f)" % (car, len(seg), done, *(seg[-1][:2] if seg else (0, 0))))
    finally:
        g.quit()
    with open(route, "w") as f:
        for p in pts:
            f.write("%.2f %.2f %.2f\n" % p)
    print("%d points written to %s" % (len(pts), route))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("out", nargs="?")
    r.add_argument("--route", default=DEFAULT_ROUTE)
    r.add_argument("--speed", type=float, default=40.0, help="m/s (40 = 144 km/h)")
    r.add_argument("--laps", type=int, default=1)
    r.add_argument("--fps", type=int, default=60, help="VCS_FPS (0: unlimited)")
    r.add_argument("--name", default="cruise")
    r.add_argument("--shots", type=float, default=0.0, help="a screenshot every S seconds (checking the route; each "
                   "is a hitch of its own, so not while measuring)")
    r.add_argument("--save", help="the card copy in build/rig/saves to start from (default: fortbaxter, else user)")
    r.add_argument("--wait", type=float, default=0.0, help="seconds at the start before driving (the population fills)")
    r.add_argument("env", nargs="*", help="ENV=VAL for the game")
    p = sub.add_parser("report")
    p.add_argument("csv")
    p.add_argument("--from", dest="t_from", type=float)
    p.add_argument("--to", dest="t_to", type=float)
    p.add_argument("--json")
    c = sub.add_parser("record")
    c.add_argument("route")
    c.add_argument("--minutes", type=float, default=8.0)
    c.add_argument("--spot", type=int, default=0, help="start spot (RECORD_SPOTS index)")
    c.add_argument("--speed", type=float, default=40.0, help="the AI's cruise speed")
    c.add_argument("--style", type=int, default=3, help="the AI's driving style (SET_CAR_DRIVING_STYLE)")
    a, rest = ap.parse_known_args()
    if a.cmd == "run":
        pairs = [x for x in (a.env + rest) if "=" in x]
        if a.out and "=" in a.out:
            pairs.append(a.out)
            a.out = None
        run(a.out or os.path.join(ROOT, "build", "cruise", a.name), a.route, a.speed, a.laps, a.fps,
            dict(x.split("=", 1) for x in pairs), a.name, shots=a.shots, save_name=a.save, wait=a.wait)
    elif a.cmd == "report":
        report(perf_report.load(a.csv, a.t_from, a.t_to), a.json)
    else:
        record(a.route, a.minutes, a.spot, a.speed, a.style)


if __name__ == "__main__":
    main()

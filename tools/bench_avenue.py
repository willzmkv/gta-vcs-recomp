#!/usr/bin/env python3
"""The player's benchmark (2026-11-13): the Little Havana avenue from the purple-error spot north to downtown's police
station (tools/routes/havana_downtown.txt, the game's road nodes), driven at a steady pace with the player's settings -
their card's progress (build/rig/saves/user: a copy of their card, never the live one), their vcs.ini (unlimited frame
rate, 16:9, internal resolution at the window's), a 4K window, the population governor off so the city is as full as
they see it - after the population has filled around the start.

    python tools/bench_avenue.py [OUT] [--rounds 2] [--speed 15] [--wait 100] [ENV=A_VALUE[,B_VALUE] ...]

With an ENV=a,b pair the rounds alternate a and b (A / B: a change against the code without it); the averages of every
frame on the drive per variant: frame / game-thread / host ms, the 99th percentile, frames over 25 and 33 ms."""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_report  # noqa: E402
import toolchain as tc  # noqa: E402

ROUTE = os.path.join(tc.ROOT, "tools", "routes", "havana_downtown.txt")
PLAYER = {"VCS_ASPECT": "", "PS2X_HIDDEN_WINDOW": "3840x2160", "VCS_POPULATION_FPS": "0"}


def route_length():
    import math
    pts = [tuple(map(float, l.split())) for l in open(ROUTE) if l.strip() and not l.startswith("#")]
    return sum(math.dist(a[:2], b[:2]) for a, b in zip(pts, pts[1:]))


def drive(out, speed, wait, env):
    cmd = tc.PY + [tc.tool("cruise.py"), "run", out, "--route", ROUTE, "--speed", str(speed), "--fps", "0",
                   "--save", "user", "--wait", str(wait), "--name", os.path.basename(out)]
    cmd += ["%s=%s" % kv for kv in env.items()]
    if subprocess.run(cmd, cwd=tc.ROOT, stdout=subprocess.DEVNULL).returncode != 0:
        print("[bench_avenue] the cruise failed (its frames left out): %s" % out)
        return None
    csv_path = os.path.join(out, "perf.csv")
    rows = perf_report.load(csv_path, None, None) if os.path.isfile(csv_path) else []
    if not rows:
        return None
    # the drive: its last route-length / speed seconds before the quit, less its first 4 s (the car getting going: the
    # cruise's own settle) and its last 2
    secs = route_length() / speed
    t_end = rows[-1]["t_s"]
    rows = [r for r in rows if t_end - secs - 1.0 + 4.0 <= r["t_s"] <= t_end - 2.0]
    if not rows:
        return None
    ms = sorted(r["frame_ms"] for r in rows)
    return {"frames": len(rows), "frame_ms": perf_report.mean(rows, "frame_ms"), "game_ms": perf_report.mean(rows, "game_ms"),
            "host_ms": perf_report.mean(rows, "host_ms") if "host_ms" in rows[0] else 0.0, "p99_ms": perf_report.pct(ms, 0.99),
            "over25": sum(1 for v in ms if v > 25.0), "over33": sum(1 for v in ms if v > 33.3), **population(out)}


def population(out):
    """The peds on the way, from cruise's popview.log: the pool's total and those in view within 200 m, averaged."""
    total, view = [], []
    path = os.path.join(out, "popview.log")
    lines = []
    if os.path.isfile(path):
        with open(path) as f:
            lines = f.readlines()
    for line in lines:
        f = line.split()
        if f and f[1] == "ok":  # (the rig's cmd drops the reply's "ok")
            f = f[:1] + f[2:]
        if len(f) > 4 and f[1].startswith("peds="):
            total.append(int(f[1][5:]))
            view.append(int(f[4]))
    return {"peds": sum(total) / len(total) if total else 0.0, "peds_view200": sum(view) / len(view) if view else 0.0}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(tc.ROOT, "build", "bench_avenue"))
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--speed", type=float, default=15.0, help="m/s (15 = 54 km/h: the population keeps up)")
    ap.add_argument("--wait", type=float, default=100.0, help="seconds at the start first (the population fills)")
    ap.add_argument("env", nargs="*", help="ENV=VAL for every run; ENV=A,B alternates A and B")
    a, rest = ap.parse_known_args()  # (ENV=VAL pairs may follow the options)
    pairs = [x for x in a.env + rest if "=" in x]
    if "=" in a.out:
        pairs.append(a.out)
        a.out = os.path.join(tc.ROOT, "build", "bench_avenue")
    fixed, ab = dict(PLAYER), None
    for kv in pairs:
        k, v = kv.split("=", 1)
        if "," in v:
            ab = (k, v.split(",", 1))
        else:
            fixed[k] = v
    variants = [(None, None)] if not ab else [(ab[0], ab[1][0]), (ab[0], ab[1][1])]
    results = {}
    for r in range(a.rounds):
        for key, val in (variants if r % 2 == 0 else variants[::-1]):
            env = dict(fixed)
            label = "base" if key is None else "%s=%s" % (key, val)
            if key is not None:
                env[key] = val
            out = os.path.join(a.out, "%s_r%d" % (label.replace("=", "_"), r))
            res = drive(out, a.speed, a.wait, env)
            print("%-28s round %d: %s" % (label, r, res), flush=True)
            if res:
                results.setdefault(label, []).append(res)
    print("\naverages over the rounds (every frame on the drive)")
    for label, rs in results.items():
        avg = {k: sum(x[k] for x in rs) / len(rs) for k in rs[0]}
        print("%-28s frame %5.2f ms  game %5.2f ms  host %5.2f ms  p99 %5.1f ms  >25ms %4.0f  >33ms %4.0f  peds %3.0f "
              "(in view <200 m %2.0f)  (%d rounds)" % (label, avg["frame_ms"], avg["game_ms"], avg["host_ms"], avg["p99_ms"],
                                                      avg["over25"], avg["over33"], avg["peds"], avg["peds_view200"], len(rs)))


if __name__ == "__main__":
    main()

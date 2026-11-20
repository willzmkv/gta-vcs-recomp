#!/usr/bin/env python3
"""Soak run (milestone 2 "done when": several hours without crash, hang, leak or falling through the world).

    python tools/soak.py [OUT] [--hours 3] [--leg 240] [--port N]

From a copy of the Fort Baxter card: invincible player, an AI-driven car (rig autodrive) with the player riding
along, moved to the next road spot every --leg seconds; game time runs (a game day is ~24 minutes). Every 30 s a row
in OUT/soak.csv: game frames, frame time, the process's private / working-set memory, the player's position and
classes - "hang" (no game frames for 30 s), "falling" (z below -40 away from the sea: tools/tour.py's rule),
"crash" (the process is gone). OUT/summary.txt has the memory trend (MB per hour after the first 20 minutes) and the
counts. A screenshot per leg. OUT defaults to build/soak/<date-time>.
"""
import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = rig.ROOT
SPOTS = [(-600.0, 600.0), (325.0, 425.0), (75.0, 175.0), (-1175.0, -325.0), (575.0, -75.0), (-1425.0, -825.0)]


def game_pid(runner_pid):
    """vcs.exe started by run.py (the rig's process)."""
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-CimInstance Win32_Process -Filter 'ParentProcessId=%d' | Where-Object Name -eq 'vcs.exe')"
                          ".ProcessId" % runner_pid], capture_output=True, text=True).stdout.strip()
    return int(out.split()[0]) if out else None


def memory_mb(pid):
    """(private bytes, working set) in MB"""
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "$p = Get-Process -Id %d -ErrorAction SilentlyContinue; if ($p) { \"$($p.PrivateMemorySize64) "
                          "$($p.WorkingSet64)\" }" % pid], capture_output=True, text=True).stdout.split()
    return (int(out[0]) / 2**20, int(out[1]) / 2**20) if len(out) == 2 else (None, None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--hours", type=float, default=3.0)
    ap.add_argument("--leg", type=float, default=240.0, help="seconds per spot")
    ap.add_argument("--port", type=int, default=47650)
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "soak", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    g = rig.Game.start(save=rig.default_save(), name="soak",
                       seconds=a.hours * 3600 + 600, data_dir=os.path.join(out, "data"), port=a.port)
    pid = game_pid(g.proc.pid)
    rows, events = [], []
    t0 = time.time()
    f = open(os.path.join(out, "soak.csv"), "w", newline="")
    w = csv.writer(f)
    w.writerow(["t_s", "frames", "frame_ms", "private_mb", "working_mb", "x", "y", "z", "spot", "class"])
    try:
        g.wait_ingame()
        g.invincible(True)
        g.prime()
        leg, last_frames, next_leg = -1, None, 0.0
        prev, still, retries = (0.0, 0.0), 0, 0
        while time.time() - t0 < a.hours * 3600:
            now = time.time() - t0
            if not g.alive():
                events.append((round(now), "crash"))
                break
            if now >= next_leg:
                leg += 1
                x, y = SPOTS[leg % len(SPOTS)]
                try:
                    g.teleport(x, y, -100.0)
                    time.sleep(3)
                    g.invincible(True)
                    g.autodrive(speed=30.0, release=True)
                    time.sleep(2)
                    g.cmd("screenshot " + os.path.join(out, "leg%03d.png" % leg))
                except Exception as e:
                    events.append((round(now), "leg %d setup failed: %s" % (leg, str(e)[:120])))
                next_leg = now + a.leg
                still, retries = 0, 0
            cls = "ok"
            try:
                frames = int(g.cmd("frames"))
                perf = g.cmd("perf")
                m = re.search(r"frame_ms=([\d.]+)", perf)
                frame_ms = float(m.group(1)) if m else None
                _, (px, py, pz), _ = g.player()
            except Exception as e:
                events.append((round(now), "control channel: %s" % str(e)[:120]))
                frames, frame_ms, px, py, pz = None, None, 0.0, 0.0, 0.0
                cls = "no_reply"
            if frames is not None and last_frames is not None and frames <= last_frames:
                cls = "hang"
            elif pz < -40.0 and -1900 < px < 1500 and -1900 < py < 1900:
                cls = "falling"
            last_frames = frames if frames is not None else last_frames
            # a car that does not get going (rig autodrive sometimes stands): another one, twice per leg at most
            moved = abs(px - prev[0]) + abs(py - prev[1]) if cls == "ok" else 99.0
            prev = (px, py)
            still = still + 1 if moved < 5.0 else 0
            if still >= 2 and retries < 2 and cls == "ok":
                retries += 1
                still = 0
                try:  # a few metres on: the spawn spot (4 m beside the player) may be against a pole or a parked car
                    g.teleport(px + 10.0 * retries, py + 10.0, -100.0)
                    time.sleep(3)
                    g.autodrive(speed=30.0, release=True)
                    events.append((round(now), "leg %d: car stood, new one" % leg))
                except Exception as e:
                    events.append((round(now), "leg %d: retry failed: %s" % (leg, str(e)[:120])))
            priv, ws = memory_mb(pid) if pid else (None, None)
            row = [round(now), frames, frame_ms, round(priv, 1) if priv else None, round(ws, 1) if ws else None,
                   round(px, 1), round(py, 1), round(pz, 1), leg, cls]
            w.writerow(row)
            f.flush()
            rows.append(row)
            if cls != "ok":
                events.append((round(now), cls))
            time.sleep(max(0.0, 30.0 - (time.time() - t0 - now)))
    finally:
        g.quit()
        f.close()
        # the next soak reuses build/run/soak.log: keep this run's next to its numbers
        log = os.path.join(ROOT, "build", "run", "soak.log")
        if os.path.exists(log):
            shutil.copyfile(log, os.path.join(out, "soak.log"))
    # memory trend after warm-up (20 min): least squares MB/h on private bytes
    pts = [(r[0] / 3600.0, r[3]) for r in rows if r[3] is not None and r[0] > 1200]
    slope = None
    if len(pts) > 4 and pts[-1][0] - pts[0][0] >= 0.5:  # a trend over less than 30 minutes is noise (streaming saw-tooth)
        n = len(pts)
        mx, my = sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n
        den = sum((p[0] - mx) ** 2 for p in pts)
        slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / den if den else None
    counts = {}
    for r in rows:
        counts[r[9]] = counts.get(r[9], 0) + 1
    fms = sorted(r[2] for r in rows if r[2])
    summary = {"hours": round((time.time() - t0) / 3600.0, 2), "samples": len(rows), "classes": counts,
               "crashed": any(e[1] == "crash" for e in events), "private_mb_first": rows[0][3] if rows else None,
               "private_mb_last": rows[-1][3] if rows else None,
               "private_mb_per_hour_after_20min": round(slope, 2) if slope is not None else None,
               "private_mb_range_after_20min": [min(p[1] for p in pts), max(p[1] for p in pts)] if pts else None,
               "frame_ms_median": fms[len(fms) // 2] if fms else None, "frame_ms_max": fms[-1] if fms else None,
               "events": events[:200]}
    json.dump(summary, open(os.path.join(out, "summary.json"), "w"), indent=1)
    print(json.dumps({k: v for k, v in summary.items() if k != "events"}, indent=1))
    print("%d events (summary.json)" % len(events))
    return 1 if summary["crashed"] or counts.get("hang") or counts.get("falling") else 0


if __name__ == "__main__":
    sys.exit(main())

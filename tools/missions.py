#!/usr/bin/env python3
"""Mission smoke test (milestone 1 of NATIVE_PORT_ROADMAP.md): start each mission with the game's own loader and
watch it for a while.

    python tools/missions.py [OUT] [--only 45,46] [--from N] [--seconds 45] [--port 47620] [--save DIR]

For every mission (default 6..100: everything but the six INIT scripts the main script runs at start): load the
save (a fresh game each time, so one mission cannot break the next), launch the mission, take a screenshot and a
perf line every 5 s, and record whether the game crashed or hung, whether the mission script is still running,
how dark the pictures were and the worst frame time. Results: OUT/results.jsonl, OUT/summary.txt,
OUT/<n>_<name>/<t>.png. Default OUT: build/missions/<date-time>.

Mission names come from MAIN.SCM (each mission's SCRIPT_NAME); the disc is read from the ISO the build uses.
"""
import argparse
import json
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import toolchain as tc  # noqa: E402
import tour  # noqa: E402  (picture_stats, perf_fields)
import vcs_disc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def mission_names():
    """[(index, name)] from MAIN.SCM's mission table (game-internals.md 11a)."""
    path = os.path.join(ROOT, "build", "analysis", "scm", "MAIN.SCM")
    if not os.path.isfile(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        vcs_disc.cmd_extract(tc.find_iso(), "MAIN.SCM", path)
    d = open(path, "rb").read()
    count = struct.unpack_from("<I", d, 0x767C)[0]
    offs = [struct.unpack_from("<I", d, 0x7680 + 4 * i)[0] for i in range(count)]
    out = []
    for i, o in enumerate(offs):
        end = offs[i + 1] if i + 1 < count else len(d) - 8
        m = re.search(rb"\x38\x02\x0a([A-Z0-9_]{1,15})\x00", d[o + 8:end + 8])
        out.append((i, m.group(1).decode() if m else "m%d" % i))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--only", help="comma-separated mission numbers")
    ap.add_argument("--from", dest="first", type=int, default=6)
    ap.add_argument("--seconds", type=float, default=45.0)
    ap.add_argument("--port", type=int, default=47620)
    ap.add_argument("--save", default=rig.default_save())
    args = ap.parse_args()
    out = os.path.abspath(args.out or os.path.join(ROOT, "build", "missions", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)

    names = mission_names()
    if args.only:
        wanted = {int(x) for x in args.only.split(",")}
        names = [m for m in names if m[0] in wanted]
    else:
        names = [m for m in names if m[0] >= args.first]

    with open(os.path.join(out, "results.jsonl"), "a") as log:
        for index, name in names:
            rec = {"mission": index, "name": name}
            shots = os.path.join(out, "%03d_%s" % (index, name))
            os.makedirs(shots, exist_ok=True)
            g = None
            try:
                g = rig.Game.start(save=args.save, name="mission%d" % index, seconds=args.seconds + 180, port=args.port)
                g.wait_ingame()
                g.invincible(True)
                # start outdoors: the save loads inside the barracks (interior area 2); a mission launched there
                # draws the outdoor world around an interior (black) and fails as "left the area". prime() ends
                # outside the barracks door.
                g.prime()
                g.mission(index)
                g.cmd("gsdraws reset")  # draw sources over the mission's run (tour.parse_gsdraws)
                t0 = time.time()
                worst = 0.0
                vu1 = 0.0
                pics = []
                running = True
                while time.time() - t0 < args.seconds:
                    time.sleep(5.0)
                    if not g.alive():
                        raise RuntimeError("game exited")
                    t = int(time.time() - t0)
                    path = os.path.join(shots, "%03d.png" % t)
                    g.cmd("screenshot " + path)
                    st = tour.picture_stats(path)
                    if st:
                        pics.append(st)
                    perf = tour.perf_fields(g.cmd("perf"))
                    worst = max(worst, perf.get("frame_ms", 0.0))
                    vu1 = max(vu1, perf.get("vu1_programs", 0.0))  # VU1 programs a frame (0: everything native)
                    running = g.player()[2]
                rec["gsdraws"] = tour.parse_gsdraws(g.cmd("gsdraws reset"))
                rec["max_vu1_programs"] = vu1
                rec["mission_running"] = running
                rec["worst_frame_ms"] = worst
                rec["dark_shots"] = sum(1 for p in pics if p[0] < 18 or p[1] > 0.7)
                rec["shots"] = len(pics)
                rec["result"] = "ok"
            except Exception as e:
                rec["result"] = "error"
                rec["error"] = str(e)[:300]
            finally:
                if g:
                    g.quit()
            log.write(json.dumps(rec) + "\n")
            log.flush()
            print("%3d %-8s %s %s" % (index, name, rec["result"],
                                       {k: v for k, v in rec.items() if k not in ("mission", "name", "result", "gsdraws")}))
    return 0


if __name__ == "__main__":
    sys.exit(main())

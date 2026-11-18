#!/usr/bin/env python3
"""Check every WRLD mission-switched building location in HD world (VCS_HD_RADIUS=3 vs 0).

    python tools/hdworld_missions.py [OUT]

Probes every non-hour conditional building location found in WRLD (issue #61), approaching from two
sides in both switch states (val=0 and val=1), comparing VCS_HD_RADIUS=3 with a VCS_HD_RADIUS=0
reference and checking every [hdprobe] build against the game's requested version(s). Writes
OUT/result.json (default: build/hdworld_missions/result.json) and exits 0 if every place passes.
"""
import argparse
import json
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import toolchain as tc  # noqa: E402


# Each entry: (name, target_level, kind, probe_str, look_xyz, off_ids, on_ids, approaches)
# Approaches stay strictly on land and on the target level's island; z=-100 uses the game's ground finder.
PLACES = [
    (
        "k01_warehouse", 2, 1, "-1210.0,-267.0,20.0,1", (-1210.0, -267.0, 15.0),
        {3167}, {6285},
        [
            ("N", [(-1210.0, 113.0, -100.0), (-1210.0, -47.0, -100.0), (-1210.0, -202.0, -100.0)]),
            ("S", [(-1210.0, -647.0, -100.0), (-1210.0, -487.0, -100.0), (-1210.0, -332.0, -100.0)]),
        ],
    ),
    (
        "k03_haiti", 2, 3, "-974.5,59.5,20.0,1", (-974.5, 59.5, 12.0),
        {3329}, {6286, 6287},
        [
            ("N", [(-974.5, 439.5, -100.0), (-974.5, 279.5, -100.0), (-974.5, 124.5, -100.0)]),
            ("S", [(-974.5, -320.5, -100.0), (-974.5, -160.5, -100.0), (-974.5, -5.5, -100.0)]),
        ],
    ),
    (
        "k09_mainla", 2, 9, "-1030.0,-950.0,45.0,1", (-1040.0, -945.0, 22.0),
        {2762, 2801}, {6296, 6297, 6298, 6299},
        [
            ("N", [(-1040.0, -565.0, -100.0), (-1040.0, -725.0, -100.0), (-1040.0, -880.0, -100.0)]),
            ("W", [(-1420.0, -945.0, -100.0), (-1260.0, -945.0, -100.0), (-1105.0, -945.0, -100.0)]),
        ],
    ),
    (
        "k11_havana", 2, 11, "-1028.0,-348.5,15.0,1", (-1028.0, -348.5, 16.6),
        {277}, {6301},
        [
            ("N", [(-1028.0, 31.5, -100.0), (-1028.0, -128.5, -100.0), (-1028.0, -283.5, -100.0)]),
            ("S", [(-1028.0, -728.5, -100.0), (-1028.0, -568.5, -100.0), (-1028.0, -413.5, -100.0)]),
        ],
    ),
    (
        "k12_beach", 1, 12, "-254.0,-1681.5,20.0,1", (-254.0, -1681.5, 10.0),
        {1052}, {7137},
        [
            ("N", [(-100.0, -1301.5, -100.0), (-160.0, -1461.5, -100.0), (-230.0, -1616.5, -100.0)]),
            ("E", [(0.0, -1550.0, -100.0), (-120.0, -1610.0, -100.0), (-205.0, -1660.0, -100.0)]),
        ],
    ),
    (
        "k32_downtown", 2, 32, "-736.0,1204.0,260.0,1", (-736.0, 1204.0, 60.0),
        {766}, {6326, 6327},
        [
            ("W", [(-926.0, 956.0, -100.0), (-926.0, 1080.0, -100.0), (-926.0, 1185.0, -100.0)]),
            ("S", [(-620.0, 690.0, -100.0), (-620.0, 820.0, -100.0), (-620.0, 950.0, -100.0)]),
        ],
    ),
]

BAD_LOG_TOKENS = ("VU1 reserved", "list record", "hdworld-draw")


def set_switch(g, kind, val):
    S = rig.Scm
    g.scm(S.op(0x03DA, S.i(kind), S.i(val)))
    time.sleep(2.5)


def wait_level(g, target_level, timeout=25.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        wanted = g.u32(0x4CD118)
        lvl = g.u32(0x48F4F8)
        loaded = g.u32(lvl + 0x18) if lvl else 0
        if wanted == target_level and loaded == target_level:
            time.sleep(3.5)
            return True
        time.sleep(0.5)
    return False


def enter_level(g, target_level):
    if target_level == 1:
        g.teleport(351.4, 339.0, -100.0)
    else:
        g.teleport(-655.5, 755.6, -100.0)
    if not wait_level(g, target_level):
        raise RuntimeError("timed out waiting for level %d" % target_level)


def cam_pos_toward(look_xyz, px, py):
    lx, ly, lz = look_xyz
    dx, dy = px - lx, py - ly
    dist = math.hypot(dx, dy)
    if dist < 1.0:
        cx, cy = lx + 60.0, ly
    else:
        scale = min(max(dist, 55.0), 320.0) / dist
        cx, cy = lx + dx * scale, ly + dy * scale
    yaw = math.degrees(math.atan2(ly - cy, lx - cx))
    return cx, cy, lz + 22.0, yaw


def parse_probes(log_path):
    builds = []
    cur = None
    with open(log_path, errors="replace") as f:
        for line in f:
            if "[hdprobe] build:" in line:
                if cur is not None:
                    builds.append(cur)
                cur = {"header": line.strip(), "records": []}
            elif "[hdprobe]" in line and cur is not None and "->" in line:
                parts = line.split()
                if "id" not in parts or "flag" not in parts:
                    continue
                try:
                    sid = int(parts[parts.index("id") + 1])
                    flag = int(parts[parts.index("flag") + 1])
                except (ValueError, IndexError):
                    continue
                src = "game" if parts[1] == "game" else "cell"
                verdict = line.split("->", 1)[1].strip()
                cur["records"].append({
                    "id": sid,
                    "src": src,
                    "flag": flag,
                    "verdict": verdict,
                })
    if cur is not None:
        builds.append(cur)
    return builds


def check_builds(builds, off_ids, on_ids):
    """Verify every [hdprobe] build obeys the game's version choice."""
    all_ids = off_ids | on_ids
    failures = []
    relevant = 0
    for idx, b in enumerate(builds):
        recs = [r for r in b["records"] if r["id"] in all_ids]
        if not recs:
            continue
        relevant += 1
        game_want_off = any(r["src"] == "game" and r["flag"] == 0 and r["id"] in off_ids for r in recs)
        game_want_on = any(r["src"] == "game" and r["flag"] == 0 and r["id"] in on_ids for r in recs)
        drawn_off = any(r["id"] in off_ids and r["verdict"] in ("drawn", "added", "added, switched on") for r in recs)
        drawn_on = any(r["id"] in on_ids and r["verdict"] in ("drawn", "added", "added, switched on") for r in recs)
        reasons = []
        for r in recs:
            if r["src"] == "game" and r["flag"] == 0 and r["verdict"] == "HIDDEN (replaced)":
                reasons.append("game id %d hidden by replaced()" % r["id"])
            if r["src"] == "cell" and r["verdict"] == "coarser, replaced":
                if not (r["id"] in on_ids and game_want_on) and not (r["id"] in off_ids and game_want_off):
                    reasons.append("cell id %d skipped as coarser, replaced" % r["id"])
        if game_want_off and not drawn_off:
            reasons.append("game requested off_ids but none drawn")
        if game_want_on and not drawn_on:
            reasons.append("game requested on_ids but none drawn")
        if drawn_off and drawn_on and not (game_want_off and game_want_on):
            reasons.append("both versions drawn without game list requesting both")
        if reasons:
            failures.append({
                "build_index": idx,
                "header": b["header"],
                "reasons": reasons,
                "records": recs[:12],
            })
    return relevant, failures


def check_bad_tokens(log_path):
    counts = {tok: 0 for tok in BAD_LOG_TOKENS}
    with open(log_path, errors="replace") as f:
        for line in f:
            for tok in BAD_LOG_TOKENS:
                if tok in line:
                    counts[tok] += 1
    return counts


def compare_pair(r0_path, r3_path):
    if not (os.path.isfile(r0_path) and os.path.isfile(r3_path)):
        return {"compared": False, "reason": "missing screenshot"}
    try:
        from PIL import Image, ImageChops
    except ImportError:
        return {"compared": False, "reason": "Pillow not installed"}
    with Image.open(r0_path).convert("RGB") as a, Image.open(r3_path).convert("RGB") as b:
        if a.size != b.size:
            b = b.resize(a.size)
        diff = ImageChops.difference(a, b)
        hist = diff.histogram()
        total = a.size[0] * a.size[1] * 3
        mad = sum((i % 256) * c for i, c in enumerate(hist)) / float(total)
        return {"compared": True, "mean_abs_diff": round(mad, 3)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", nargs="?", default=os.path.join("build", "hdworld_missions"),
                    help="output directory (default: build/hdworld_missions)")
    args = ap.parse_args()
    out_dir = os.path.abspath(args.out if os.path.isabs(args.out) else os.path.join(tc.ROOT, args.out))
    shots_dir = os.path.join(out_dir, "shots")
    os.makedirs(shots_dir, exist_ok=True)
    save_path = rig.default_save()

    base_env = {
        "PS2X_HIDDEN_WINDOW": "1600x900",
        "PS2X_CAPTURE_SIZE": "960x672",
        "VCS_PERF_OVERLAY": "0",
        "VCS_HUD": "0",
    }

    # 1. R=0 baseline screenshots across all switched places (val=0 and val=1, both sides)
    print("[hdworld_missions] capturing VCS_HD_RADIUS=0 baseline...", flush=True)
    env0 = dict(base_env)
    env0["VCS_HD_RADIUS"] = "0"
    g0 = rig.Game.start(save=save_path, name="hdwm_r0", port=17386,
                        data_dir=os.path.join(out_dir, "data_r0"), env=env0)
    try:
        g0.wait_ingame()
        g0.prime()
        g0.invincible(True)
        g0.set_time(12, 0)
        g0.weather(0)
        cur_lvl = None
        for name, tgt_lvl, kind, _probe, look, _off, _on, approaches in PLACES:
            if cur_lvl != tgt_lvl:
                enter_level(g0, tgt_lvl)
                cur_lvl = tgt_lvl
            for side, steps in approaches:
                px, py, pz = steps[-1]
                g0.teleport(px, py, pz)
                time.sleep(0.5)
                cx, cy, cz, yaw = cam_pos_toward(look, px, py)
                g0.cmd("campose %.1f %.1f %.1f %.1f -10" % (cx, cy, cz, yaw))
                time.sleep(2.0)
                for val in (0, 1):
                    set_switch(g0, kind, val)
                    time.sleep(1.5)
                    shot = os.path.join(shots_dir, "%s_v%d_%s_r0.png" % (name, val, side)).replace("\\", "/")
                    g0.cmd("screenshot " + shot)
                set_switch(g0, kind, 0)
            g0.cmd("campose off")
    finally:
        g0.quit()

    # 2. R=3 probe per switched place approaching from two sides in val=0 and val=1
    results = []
    all_passed = True
    for name, tgt_lvl, kind, probe, look, off_ids, on_ids, approaches in PLACES:
        print("[hdworld_missions] probing %s (kind=%d level=%d)..." % (name, kind, tgt_lvl), flush=True)
        env3 = dict(base_env)
        env3["VCS_HD_RADIUS"] = "3"
        env3["VCS_HD_PROBE"] = probe
        g = rig.Game.start(save=save_path, name="hdwm_" + name, port=17387,
                           data_dir=os.path.join(out_dir, "data_" + name), env=env3)
        try:
            g.wait_ingame()
            g.prime()
            g.invincible(True)
            g.set_time(12, 0)
            g.weather(0)
            enter_level(g, tgt_lvl)
            for val in (0, 1):
                set_switch(g, kind, val)
                for side, steps in approaches:
                    for idx, (px, py, pz) in enumerate(steps):
                        g.teleport(px, py, pz)
                        cx, cy, cz, yaw = cam_pos_toward(look, px, py)
                        g.cmd("campose %.1f %.1f %.1f %.1f -10" % (cx, cy, cz, yaw))
                        time.sleep(3.5)
                        if idx == len(steps) - 1:
                            shot = os.path.join(shots_dir, "%s_v%d_%s_r3.png" % (name, val, side)).replace("\\", "/")
                            g.cmd("screenshot " + shot)
                g.cmd("campose off")
            set_switch(g, kind, 0)
        finally:
            g.quit()

        builds = parse_probes(g.log_path)
        relevant, failures = check_builds(builds, off_ids, on_ids)
        bad_tokens = check_bad_tokens(g.log_path)
        shot_diffs = {}
        for val in (0, 1):
            for side, _steps in approaches:
                key = "v%d_%s" % (val, side)
                r0_shot = os.path.join(shots_dir, "%s_%s_r0.png" % (name, key))
                r3_shot = os.path.join(shots_dir, "%s_%s_r3.png" % (name, key))
                shot_diffs[key] = compare_pair(r0_shot, r3_shot)
        place_ok = (relevant > 0 and len(failures) == 0 and all(v == 0 for v in bad_tokens.values()))
        if not place_ok:
            all_passed = False
        results.append({
            "name": name,
            "level": tgt_lvl,
            "kind": kind,
            "probe": probe,
            "off_ids": sorted(off_ids),
            "on_ids": sorted(on_ids),
            "total_builds": len(builds),
            "relevant_builds": relevant,
            "failure_count": len(failures),
            "failures": failures[:10],
            "bad_log_tokens": bad_tokens,
            "shot_diffs": shot_diffs,
            "log": g.log_path,
            "passed": place_ok,
        })
        print("  %-14s builds=%d relevant=%d failures=%d bad_log=%s -> %s" %
              (name, len(builds), relevant, len(failures), bad_tokens, "PASS" if place_ok else "FAIL"), flush=True)

    summary = {
        "passed": all_passed,
        "places": results,
    }
    res_path = os.path.join(out_dir, "result.json")
    with open(res_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    print("[hdworld_missions] wrote %s (passed=%s)" % (res_path, all_passed), flush=True)
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())

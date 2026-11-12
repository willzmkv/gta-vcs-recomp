#!/usr/bin/env python3
"""Traffic / pedestrian density and draw distance (Enhancements > World; src/game/vcs_world_detail.cpp,
game-internals.md 6o and 12d).

    python tools/world_check.py [OUT] [--port N] [--seconds S]

One hidden session from a card copy (rig.default_save), at noon in the sun at a busy city spot. Checks:
  original   at 100% the spawners' factors, caps and spawn-chance table and the LOD scale hold the executable's values,
             bit for bit
  inview     the in-view test (TheCamera's matrix) sees the player in front of the camera
  peds       at 300% / 300% at least 25% more peds than at 100% (means over S seconds at the same spot; standing, the
             cars come and go at the same few spawn spots - `traffic` reports them while driving)
  poolroom   at 300% the pools keep room: never fewer than 8 free ped / vehicle slots (where the game deletes the
             closest one), and a script's CREATE_CHAR and CREATE_CAR still get a handle
  script0    the scripts' own multipliers at 0 (as missions do to clear the streets) still stop random spawning
  lod        at 300% draw distance the LOD scale is 3 and LODDistMultiplier (TheCamera+0x7DC) the original times it;
             reported: the draws in an emptied street (the scripts' density 0) at 100 / 300%, perf at 100..300%
  packets    during a drive at 300%: the packet buffer's peak stays clear of where the game drops sectors
  traffic    (reported only) random cars (0x48A96C) during the drive at 100% and 300% traffic
  vanish     no car in view disappeared during the drive (the module's detector, `vanished` in `worlddetail`)
  back       back at 100% every value is the executable's again
Screenshots go to OUT (default build/world_check); OUT/results.json holds the checks and the counts / perf numbers.
Exit code 1 when one fails.
"""
import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

ROOT = rig.ROOT
S = rig.Scm
SET_CAR_DENSITY_MULTIPLIER = 0x0125
SET_PED_DENSITY_MULTIPLIER = 0x0264
LOD_MULTIPLIER = 0x6F44D0 + 0x7DC  # TheCamera.LODDistMultiplier
# address, executable's bytes (game-internals.md 12d / 6o)
ORIGINAL = {
    "car_factor": (0x48A680, struct.pack("<f", 0.7)),
    "ped_factor": (0x48A67C, struct.pack("<f", 0.4)),
    "car_cap": (0x48A9A8, struct.pack("<i", 16)),
    "ped_cap_out": (0x488004, struct.pack("<i", 25)),
    "ped_cap_in": (0x488008, struct.pack("<i", 30)),
    "lod_scale": (0x4887F8, struct.pack("<f", 1.0)),
    "spawn_chance": (0x487498, bytes([0, 4, 7, 15])),
}
SPOT = (-675.0, 1425.0, 90.0)  # bench.py's second city spot: traffic and people at noon (its first has none)
VIEW = SPOT


def state(g):
    """`worlddetail` -> dict"""
    return dict(kv.split("=", 1) for kv in g.cmd("worlddetail").split() if "=" in kv)


def perf(g):
    out = {}
    for item in g.cmd("perf").split():
        k, sep, v = item.partition("=")
        if sep:
            try:
                out[k] = float(v)
            except ValueError:
                pass
    return out


def diff_fraction(a, b):
    """share of sampled pixels that differ by more than 25 in a channel"""
    wa, ha, ra = tc.read_png(a)
    wb, hb, rb = tc.read_png(b)
    if (wa, ha) != (wb, hb):
        return 1.0
    bpp = len(ra[0]) // wa
    n = changed = 0
    for y in range(0, ha, 4):
        la, lb = ra[y], rb[y]
        for x in range(0, wa * bpp, 4 * bpp):
            n += 1
            if max(abs(la[x + c] - lb[x + c]) for c in range(3)) > 25:
                changed += 1
    return changed / max(n, 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "build", "world_check"))
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--seconds", type=float, default=25.0, help="sampling time per density setting")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    results, data = {}, {}

    def ok(name, cond, detail=""):
        results[name] = {"ok": bool(cond), "detail": str(detail)}
        print("[world_check] %-9s %s  %s" % (name, "ok" if cond else "FAILED", detail), flush=True)

    def originals_hold():
        bad = {k: g.peek(a, 4).hex() for k, (a, v) in ORIGINAL.items() if g.peek(a, 4) != v}
        return not bad, bad

    g = rig.Game.start(save=rig.default_save(), name="world_check", port=args.port)
    try:
        g.wait_ingame()
        g.prime()
        g.invincible()
        g.set_time(12)
        g.weather(0)

        def shot(name):
            path = os.path.join(out, name + ".png")
            g.cmd("screenshot " + path)
            return path

        def setting(key, v):
            g.cmd("setting %s %d" % (key, v))

        def sample(label):
            """at SPOT: the pools over args.seconds (after a settle), perf at the end"""
            g.teleport(*SPOT[:2], -100.0, heading=SPOT[2])
            time.sleep(6.0)
            peds, cars, ped_free, veh_free = [], [], [], []
            t0 = time.time()
            while time.time() - t0 < args.seconds:
                st = state(g)
                peds.append(int(st["pedsused"]))
                cars.append(int(st["vehused"]))
                ped_free.append(int(st["pedfree"]))
                veh_free.append(int(st["vehfree"]))
                time.sleep(0.5)
            row = {"peds": sum(peds) / len(peds), "vehicles": sum(cars) / len(cars), "min_ped_free": min(ped_free),
                   "min_vehicle_free": min(veh_free), "perf": perf(g), "state": state(g)}
            shot("density_" + label)
            data["density_" + label] = row
            print("[world_check] %s: peds %.1f vehicles %.1f (fewest free %d / %d) game %.2f ms" % (
                label, row["peds"], row["vehicles"], row["min_ped_free"], row["min_vehicle_free"],
                row["perf"].get("game_ms", 0)), flush=True)
            return row

        # original: from the defaults whatever the player's vcs.ini holds (hidden runs read it, never write it)
        for key in ("enhance.traffic", "enhance.peds", "enhance.draw_distance"):
            setting(key, 100)
        g.wait_frames(5)
        st = state(g)
        held, bad = originals_hold()
        ok("original", st.get("valid") == "1" and held and st.get("lod") == "1.000", bad or st)
        # the in-view test (cars kept, the vanish detector) reads TheCamera's matrix: the player is in front of it
        ok("inview", st.get("playerinview") == "1", "playerinview=%s" % st.get("playerinview"))

        # density
        base = sample("100")
        setting("enhance.traffic", 300)
        setting("enhance.peds", 300)
        g.wait_frames(5)
        st = state(g)
        dense = sample("300")
        # standing, the cars come and go at the same few spawn spots (game-internals.md 12d): only peds are checked here
        ok("peds", dense["peds"] > base["peds"] * 1.25,
           "peds %.1f -> %.1f, vehicles %.1f -> %.1f, caps %s / %s, spawn chance %s / %s" % (
               base["peds"], dense["peds"], base["vehicles"], dense["vehicles"], st["carcap"], st["pedcap"],
               st["carchance"], st["pedchance"]))

        # poolroom
        _, (x, y, z), _ = g.player()
        code = (S.op(S.REQUEST_MODEL, S.i(55)) + S.op(S.REQUEST_MODEL, S.i(211)) + S.op(S.LOAD_ALL_MODELS_NOW) +
                S.op(S.CREATE_CHAR, S.i(4), S.i(55), S.f(x + 3.0), S.f(y), S.f(-100.0), S.LOCAL0) +
                S.op(S.CREATE_CAR, S.i(211), S.f(x - 6.0), S.f(y), S.f(-100.0), b"\x0e") +
                S.op(S.MARK_CHAR_AS_NO_LONGER_NEEDED, S.LOCAL0) + S.op(S.MARK_CAR_AS_NO_LONGER_NEEDED, b"\x0e"))
        handles, _ = g.scm_outputs(code, 2)
        # the game deletes the closest car / ped while fewer than 8 slots are free: never reached
        st = state(g)
        ok("poolroom", dense["min_ped_free"] >= 8 and dense["min_vehicle_free"] >= 8 and all(h > 0 for h in handles),
           "fewest free peds %d vehicles %d (since the last query: %s / %s), handles %s" % (
               dense["min_ped_free"], dense["min_vehicle_free"], st["minpedfree"], st["minvehfree"],
               [hex(h) for h in handles]))

        # script0
        g.scm(S.op(SET_CAR_DENSITY_MULTIPLIER, S.f(0.0)) + S.op(SET_PED_DENSITY_MULTIPLIER, S.f(0.0)))
        time.sleep(3.0)
        st0 = state(g)
        time.sleep(15.0)
        st1 = state(g)
        g.scm(S.op(SET_CAR_DENSITY_MULTIPLIER, S.f(1.0)) + S.op(SET_PED_DENSITY_MULTIPLIER, S.f(1.0)))
        grew = (int(st1["pedsused"]) - int(st0["pedsused"]), int(st1["vehused"]) - int(st0["vehused"]))
        ok("script0", grew[0] <= 1 and grew[1] <= 1, "over 15 s at 0: peds %+d vehicles %+d" % grew)

        # lod
        setting("enhance.traffic", 100)
        setting("enhance.peds", 100)
        g.teleport(*VIEW[:2], -100.0, heading=VIEW[2])
        time.sleep(6.0)
        lod100 = g.peekf(LOD_MULTIPLIER, 1)[0]
        near = shot("draw_100")
        rows = []
        for _ in range(3):
            time.sleep(1.1)
            rows.append(perf(g))
        perf100 = {k: sum(r.get(k, 0) for r in rows) / 3 for k in ("game_ms", "worker_ms", "gs_ms", "draws", "frame_ms")}
        print("[world_check] draw 100%%: game %.2f ms worker %.2f ms gs %.2f ms draws %.0f" % (
            perf100["game_ms"], perf100["worker_ms"], perf100["gs_ms"], perf100["draws"]), flush=True)
        sweep = {}
        for pct in (150, 200, 250, 300):
            setting("enhance.draw_distance", pct)
            g.wait_frames(int(pct * 2))  # the scale grows 0.01 a frame
            time.sleep(1.0)
            rows = []
            for _ in range(3):
                time.sleep(1.1)
                rows.append(perf(g))
            sweep[pct] = {k: sum(r.get(k, 0) for r in rows) / 3 for k in ("game_ms", "worker_ms", "gs_ms", "draws")}
            sweep[pct]["lod"] = state(g)["lod"]
            print("[world_check] draw %d%%: game %.2f ms worker %.2f ms gs %.2f ms draws %.0f (scale %s)" % (
                pct, sweep[pct]["game_ms"], sweep[pct]["worker_ms"], sweep[pct]["gs_ms"], sweep[pct]["draws"],
                sweep[pct]["lod"]), flush=True)
        data["draw_sweep"] = sweep
        st = state(g)
        lod300 = g.peekf(LOD_MULTIPLIER, 1)[0]
        far = shot("draw_300")
        perf300 = perf(g)
        frac = diff_fraction(near, far)
        allowed = float(st["lod"])
        # draws with the street emptied (the scripts' density 0, as a mission does) so walkers don't change the count;
        # 100 / 300 alternated twice
        g.scm(S.op(SET_CAR_DENSITY_MULTIPLIER, S.f(0.0)) + S.op(SET_PED_DENSITY_MULTIPLIER, S.f(0.0)))
        time.sleep(20.0)
        counts = {100: [], 300: []}
        for pct in (100, 300, 100, 300):
            setting("enhance.draw_distance", pct)
            g.wait_frames(250)
            time.sleep(1.0)
            for _ in range(2):
                time.sleep(1.1)
                counts[pct].append(perf(g).get("draws", 0))
        g.scm(S.op(SET_CAR_DENSITY_MULTIPLIER, S.f(1.0)) + S.op(SET_PED_DENSITY_MULTIPLIER, S.f(1.0)))
        d100, d300 = (sum(counts[p]) / len(counts[p]) for p in (100, 300))
        data["draws_empty_street"] = counts
        # the mechanism is checked; the draw counts are reported (walkers left in the street and a busy machine move them
        # more than the setting: runs gave 643 -> 700, 1198 -> 1616, 850 -> 786)
        ok("lod", abs(allowed - 3.0) < 1e-3 and abs(lod300 - lod100 * allowed) < 0.02 * lod300 + 1e-3,
           "LODDistMultiplier %.3f -> %.3f (scale %.2f); reported: draws in the emptied street %.0f -> %.0f, %.1f%% of "
           "the picture changed" % (lod100, lod300, allowed, d100, d300, frac * 100))
        data["draw"] = {"lod100": lod100, "lod300": lod300, "changed": frac, "perf100": perf100, "perf300": perf300,
                        "state": st}

        # packets: a drive at 300% draw distance
        g.autodrive(speed=40.0)
        time.sleep(2.0)
        vanished0 = int(state(g)["vanished"])  # also resets the peak (and the teleport's cut is behind)
        time.sleep(30.0)
        st = state(g)
        data["drive"] = {"state": st, "perf": perf(g)}
        ok("packets", int(st["peak"]) < 0xE6670 - 0x12BFF,
           "peak %d KB of %d KB (the game drops sectors below %d KB free), scale %s" % (
               int(st["peak"]) >> 10, 0xE6670 >> 10, 0x12BFF >> 10, st["lod"]))

        # traffic: random cars (0x48A96C) while the drive goes on, at 100% then 300%
        def random_cars(seconds=40.0):
            n, t0 = [], time.time()
            while time.time() - t0 < seconds:
                n.append(g.u32(0x48A96C))
                time.sleep(0.5)
            return sum(n) / len(n)

        cars100 = random_cars()
        setting("enhance.traffic", 300)
        cars300 = random_cars()
        data["traffic"] = {"random_cars_100": cars100, "random_cars_300": cars300}
        # reported, not checked: the city changes along the drive, so 40 s windows differ by more than the setting's
        # effect (runs gave 7.3 -> 8.8, 8.8 -> 10.2, 7.7 -> 6.4; game-internals.md 12d: the spawn spots limit cars)
        print("[world_check] traffic   random cars while driving %.1f -> %.1f (reported only)" % (cars100, cars300),
              flush=True)

        # vanish: no car in view (within the distance cars are drawn at) disappeared during the drive at 300% draw
        # distance and traffic (the module's detector; the game removed them at 160 m on screen and, with a full pool,
        # the closest car)
        st = state(g)
        ok("vanish", int(st["vanished"]) == vanished0,
           "cars vanished in view during %d s of driving: %d; kept in view %s times; fewest free vehicles %s" % (
               110, int(st["vanished"]) - vanished0, st["kept"], st["minvehfree"]))

        # back
        setting("enhance.traffic", 100)
        setting("enhance.draw_distance", 100)
        g.wait_frames(5)
        held, bad = originals_hold()
        ok("back", held, bad or "all original")
    finally:
        try:
            g.quit()
        except Exception:
            pass
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump({"checks": results, "data": data}, f, indent=1)
    failed = [k for k, v in results.items() if not v["ok"]]
    print("[world_check] %s" % ("all ok" if not failed else "FAILED: " + ", ".join(failed)), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

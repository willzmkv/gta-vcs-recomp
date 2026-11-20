#!/usr/bin/env python3
"""Melee against the street's own peds: Vic punches ambient pedestrians and the game has to register the hits.

    python tools/melee_check.py [OUT] [--save DIR] [--targets 12] [--punches 3] [--spot havana] [--variant NAME=ENV=VAL,...]

Reported by the player (2026-11-15): many of the street's pedestrians are intangible to fists, bats and knives (the
blows pass through them) while bullets and cars hurt them. One hidden run per variant (default `as-is`) from the card
copy (default the rig's user copy), the raised population on (VCS_POPULATION_FPS=0, 60 fps, as the player plays),
noon, the player invincible, at a reference spot (tools/spots.py). After the pool fills, up to --targets random peds on
foot (created by the population, alive, within 60 m) are taken one by one: Vic is put 2.5 m from the ped, given 0.7 s
for the game's near-ped scan (one frame in 16) to list it, then put 1.0 m from it, facing it, and Circle is pressed
(--punches times, re-placed before each). A target is hit when its health dropped or its state changed
to a hit / fall / death; for each punch the log keeps what FireMelee reads (game-internals.md 12h): whether the ped was
in Vic's near-ped list (+0x6F8, count +0x77A), its entity flags' bit 9 (+0x48) and its state.
A punch counts only with Vic within 1.2 m when it is thrown (a ped can walk off between placing and the blow); each
target's pool slot is kept - the PS2's pool has 70, the port's more, and the slots from 70 up were the intangible ones
(CPed::IsPedPointerValid's compiled size, vcs_distance.cpp). Pass = at least 60 % of the placed targets hit (a ped walking
or turning steps out of some blows, below slot 70 too), at least 90 % of the blows at peds from slot 70 up with the ped
in Vic's near list (0 % before the fix; a ped the population is taking out of the world is not counted), and the game
alive. `--variant original=VCS_PED_VALID=0` shows the bug. Results: OUT/result.json; exit code 0 = pass. Outputs: OUT (default build/melee_check).
"""
import argparse
import json
import math
import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import spots  # noqa: E402
import toolchain as tc  # noqa: E402

PED_POOL = 0x487A94          # CPool<CPed> *: {entries, flags, size}; entries 0xD20 bytes
PED_SIZE = 0xD20
STATE_DRIVING, STATE_DIE, STATE_DEAD = 0x37, 0x39, 0x3A
HIT_RATE = 0.6     # of the placed targets (a walking or turning ped steps out of 2-4 in 10 blows, below slot 70 too)
LISTED_RATE = 0.9  # blows at peds from slot 70 up with the ped in the near list (0 before the fix)
PS2_PED_SLOTS = 70  # the PS2's ped pool; the port's is larger (vcs_distance.cpp)
PLACED = 1.2        # a punch counts only with Vic this close: a fist reaches ~1.2 m (measured: none hit from 1.25 m)
# the player's way (memory: tests without the raised population show an empty city)
PLAYER_ENV = {"VCS_POPULATION_FPS": "0", "VCS_FPS": "60", "VCS_PERF_OVERLAY": "0"}


def ped_info(g, ped):
    raw = g.peek(ped, 0x900)
    x, y, z = struct.unpack_from("<3f", raw, 0x30)
    fx, fy = struct.unpack_from("<2f", raw, 0x10)
    return {
        "ped": ped,
        "pos": (x, y, z),
        "fwd": (fx, fy),
        "flags": struct.unpack_from("<Q", raw, 0x48)[0],
        "entries": struct.unpack_from("<I", raw, 0xE4)[0],
        "vehicle": struct.unpack_from("<I", raw, 0x480)[0],
        "health": struct.unpack_from("<f", raw, 0x4E4)[0],
        "near": [v for v in struct.unpack_from("<10I", raw, 0x6F8)],
        "near_count": struct.unpack_from("<H", raw, 0x77A)[0],
        "state": struct.unpack_from("<I", raw, 0x8C4)[0],
        "created_by": struct.unpack_from("<I", raw, 0x8F4)[0],
    }


def peds(g):
    pool = g.u32(PED_POOL)
    entries, flags, size = struct.unpack("<3I", g.peek(pool, 12))
    used = g.peek(flags, size)
    return [entries + i * PED_SIZE for i in range(size) if not used[i] & 0x80]


def in_cell_list(g, ped, pos):
    """Whether the ped is in the ped list (+0x24) of the 80 m world cell it stands in - what ScanForEntities walks
    (cell (x / 80 + 30, y / 80 + 25) of the 50 x 50 at *0x486464, 0x38 each; list nodes {item, ?, next})."""
    ix, iy = int(pos[0] / 80 + 30), int(pos[1] / 80 + 25)
    node = g.u32(g.u32(0x486464) + (iy * 50 + ix) * 0x38 + 0x24)
    for _ in range(4000):
        if not node:
            return False
        item, _, node = struct.unpack("<3I", g.peek(node, 12))
        if item == ped:
            return True
    return False


def slot(g, ped):
    pool = g.u32(PED_POOL)
    return (ped - g.u32(pool)) // PED_SIZE


def position(g, ped):
    return struct.unpack("<3f", g.peek(ped + 0x30, 12))


def random_on_foot(info):
    return (info["created_by"] == 1 and info["vehicle"] == 0 and info["health"] > 0.0 and
            info["state"] not in (STATE_DRIVING, 0x38, STATE_DIE, STATE_DEAD))


def face(g, target, gap):
    """Vic `gap` m from the target on his side, facing it (GTA's heading: 0 north, 90 west)."""
    _, (px, py, _), _ = g.player()
    tx, ty, tz = target
    dx, dy = px - tx, py - ty
    d = math.hypot(dx, dy)
    ux, uy = (dx / d, dy / d) if d > 0.05 else (0.0, -1.0)
    x, y = tx + ux * gap, ty + uy * gap
    heading = math.degrees(math.atan2(ux, -uy)) % 360.0  # facing -u: atan2(-(-ux), -uy)
    S = rig.Scm
    g.scm(S.op(S.SET_CHAR_COORDINATES, S.PLAYER, S.f(x), S.f(y), S.f(tz - 1.0)) +
          S.op(S.SET_CHAR_HEADING, S.PLAYER, S.f(heading)))


def punch(g, hold=0.12, after=0.75):
    g.cmd("gamepad b 1")
    time.sleep(hold)
    g.cmd("gamepad b 0")
    time.sleep(after)


def run(out, name, env, save, spot, targets, punches, log):
    e = dict(PLAYER_ENV)
    e.update(env)
    g = rig.Game.start(save=save, name="melee_" + name, seconds=900, env=e)
    result = {"variant": name, "env": e, "targets": [], "alive": True}
    try:
        g.wait_ingame(timeout=180)
        spots.wait_after_load()
        g.set_time(12)
        g.weather(0)
        g.invincible(True)
        spots.put(g, spot, camera=False)
        g.cmd("gamepad on")
        print("[melee] %s: waiting for the population" % name, flush=True)
        time.sleep(75)
        result["distancestate"] = g.cmd("distancestate")
        tried = set()
        empty = 0
        while len(result["targets"]) < targets and g.alive():
            player, (px, py, pz), _ = g.player()
            cands = []
            for p in peds(g):
                if p == player or p in tried:
                    continue
                x, y, z = position(g, p)
                d = math.hypot(x - px, y - py)
                if d < 60.0 and abs(z - pz) < 4.0 and random_on_foot(ped_info(g, p)):
                    cands.append((d, p))
            if not cands:
                # none on foot near: back to the spot for the population to bring more, a few times
                empty += 1
                if empty > 12:
                    log("[melee] %s: no more peds on foot near the spot" % name)
                    break
                spots.put(g, spot, camera=False)
                time.sleep(15)
                continue
            empty = 0
            ped = min(cands)[1]
            tried.add(ped)
            before = ped_info(g, ped)
            t = {"ped": "%08x" % ped, "slot": slot(g, ped), "health_before": before["health"],
                 "state_before": before["state"], "punches": [], "hit": False}
            for _ in range(punches):
                info = ped_info(g, ped)
                if not random_on_foot(info):
                    break
                # near first and long enough for CPed::ScanForEntities' next pass (one frame in 16) to list the
                # ped: a teleport from farther than its 30 m leaves the list empty until then
                face(g, info["pos"], 2.5)
                time.sleep(0.7)
                face(g, position(g, ped), 1.0)
                me = ped_info(g, g.player()[0])
                info = ped_info(g, ped)
                near = me["near"][:min(me["near_count"], 10)]
                d = math.hypot(info["pos"][0] - me["pos"][0], info["pos"][1] - me["pos"][1])
                if d > PLACED:
                    t["punches"].append({"dist": round(d, 2), "placed": False})
                    continue
                punch(g)
                after = ped_info(g, ped)
                p = {"dist": round(d, 2), "placed": True, "in_near": ped in near, "near_count": me["near_count"],
                     "bit9": bool(info["flags"] >> 9 & 1), "flags": "%016x" % info["flags"],
                     "state": info["state"], "health": info["health"], "health_after": after["health"],
                     "state_after": after["state"]}
                if not p["in_near"]:
                    # why ScanForEntities left it out: not in the world (+0xE4), leaving it (bit 32), not in its cell
                    p.update(in_world=info["entries"] != 0, remove=bool(info["flags"] >> 32 & 1),
                             in_cell=in_cell_list(g, ped, info["pos"]), player_state=me["state"])
                t["punches"].append(p)
                if after["health"] < info["health"] - 0.01 or after["state"] in (STATE_DIE, STATE_DEAD):
                    t["hit"] = True
                    break
            t["placed"] = any(p["placed"] for p in t["punches"])
            last = ped_info(g, ped)
            t["health_after"], t["state_after"] = last["health"], last["state"]
            result["targets"].append(t)
            log("[melee] %s ped %s slot %3d: %s  %s" % (
                name, t["ped"], t["slot"], ("HIT " if t["hit"] else "MISS") if t["placed"] else "----",
                " | ".join(("d=%.2f near=%d/%d bit9=%d st=%x hp %.0f->%.0f st->%x" % (
                    p["dist"], p["in_near"], p["near_count"], p["bit9"], p["state"], p["health"],
                    p["health_after"], p["state_after"]) + (" [world=%d remove=%d cell=%d vic_st=%x]" % (
                        p["in_world"], p["remove"], p["in_cell"], p["player_state"]) if not p["in_near"] else "")) if p["placed"] else "d=%.2f not placed" % p["dist"]
                           for p in t["punches"])))
        g.cmd("gamepad off")
    except Exception as ex:  # noqa: BLE001
        result["error"] = str(ex)
        log("[melee] %s: %s" % (name, ex))
    finally:
        result["alive"] = g.alive()
        try:
            g.quit()
        except Exception:  # noqa: BLE001
            pass
    placed = [t for t in result["targets"] if t["placed"]]
    hits = sum(t["hit"] for t in placed)
    high = [t for t in placed if t["slot"] >= PS2_PED_SLOTS]
    high_hits = sum(t["hit"] for t in high)
    # the bug itself: a ped in a slot from 70 up missing from Vic's near peds when the punch is thrown
    # a ped the population is taking out of the world (+0xE4 empty, its slot soon another ped's - the test's teleports
    # cut the camera) is nobody's target, on the PS2 too: not counted
    blows = [p for t in high for p in t["punches"] if p["placed"] and (p["in_near"] or p["in_world"])]
    listed = sum(p["in_near"] for p in blows)
    result.update(hits=hits, placed=len(placed), tried=len(result["targets"]), high=len(high), high_hits=high_hits,
                  high_blows=len(blows), high_listed=listed)
    result["pass"] = (result["alive"] and "error" not in result and len(placed) > 0 and hits >= HIT_RATE * len(placed)
                      and listed >= LISTED_RATE * len(blows))
    log("[melee] %s: %d / %d placed targets hit; slots >= %d: %d / %d hit, listed at %d / %d blows (%d not placed) -> %s" % (
        name, hits, len(placed), PS2_PED_SLOTS, high_hits, len(high), listed, len(blows),
        len(result["targets"]) - len(placed), "PASS" if result["pass"] else "FAIL"))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(tc.ROOT, "build", "melee_check"))
    ap.add_argument("--save", default=None, help="card copy to start from (default the rig's user copy)")
    ap.add_argument("--spot", default="havana", choices=sorted(spots.SPOTS))
    ap.add_argument("--targets", type=int, default=12)
    ap.add_argument("--punches", type=int, default=3)
    ap.add_argument("--variant", action="append", default=[], help="NAME=ENV=VAL,ENV=VAL (default: as-is)")
    a = ap.parse_args()
    save = a.save
    if not save:
        user = os.path.join(rig.SAVES, "user")
        save = user if os.path.isdir(os.path.join(user, "mc0")) else rig.default_save()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    variants = [("as-is", {})]
    if a.variant:
        variants = []
        for v in a.variant:
            name, _, rest = v.partition("=")
            env = dict(kv.split("=", 1) for kv in rest.split(",") if kv)
            variants.append((name, env))
    lines = []

    def log(s):
        print(s, flush=True)
        lines.append(s)

    results = [run(out, n, env, save, a.spot, a.targets, a.punches, log) for n, env in variants]
    (out / "result.json").write_text(json.dumps(results, indent=1))
    (out / "log.txt").write_text("\n".join(lines) + "\n")
    return 0 if all(r["pass"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())

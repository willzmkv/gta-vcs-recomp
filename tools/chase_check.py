#!/usr/bin/env python3
"""Police chasing a moving car: does the law keep up? (the player's report, 2026-11-17: with Population distance up, the
police came only once the car stood still - their cars spawn with the traffic, at its distance; game/vcs_distance.cpp
chased()).

    python tools/chase_check.py [OUT] [--speed 30] [--seconds 90] [--stars 3] [ENV=VAL ...]

The player's car along tools/routes/havana_downtown.txt at a fixed speed (the rig's `cruise`), wanted at --stars from
the start; every second the police cars (model 207, POLICEM) within 100 m and 250 m and the nearest one's distance.
Settings: the player's vcs.ini (their Population distance, Vehicle density), the population governor off. PASS: a police
car within 100 m in at least a third of the seconds after the first 20, and no more new cars appearing in plain sight
(within 160 m, ahead in a 45-degree cone) than one per 20 s - the first fix spawned all traffic at the game's distance
while wanted, so every new car popped in close in front (the player's report, 2026-11-18). --stars 0 measures the
pop-ins without a chase, for comparing.
"""
import json
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

POLICE = {207}


def main():
    args = [a for a in sys.argv[1:] if "=" not in a or a.startswith("--")]
    env = dict(a.split("=", 1) for a in sys.argv[1:] if "=" in a and not a.startswith("--"))
    opts = {"--speed": 30.0, "--seconds": 90.0, "--stars": 3}
    out = None
    i = 0
    while i < len(args):
        if args[i] in opts:
            opts[args[i]] = type(opts[args[i]])(args[i + 1])
            i += 2
        else:
            out = args[i]
            i += 1
    out = os.path.abspath(out or os.path.join(rig.ROOT, "build", "chase_check", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    route = os.path.join(rig.ROOT, "tools", "routes", "havana_downtown.txt")
    pts = [tuple(float(v) for v in l.split()) for l in open(route) if l.strip() and not l.startswith("#")]
    e = {"VCS_POPULATION_FPS": "0", "VCS_FPS": "30"}
    e.update(env)
    save = os.path.join(rig.SAVES, "shakedown") if os.path.isdir(os.path.join(rig.SAVES, "shakedown")) else rig.default_save()
    g = rig.Game.start(save=save, name="chase_check", port=47680, data_dir=os.path.join(out, "data"), env=e)
    S = rig.Scm
    samples = []
    seen = set()  # (pool slot, its id byte): the cars already counted
    popins = []
    try:
        g.wait_ingame()
        g.set_time(12)
        g.weather(0)
        g.invincible()
        x, y, z = pts[0]
        g.teleport(x, y, z)
        time.sleep(1.0)
        car = S.LOCAL0
        g.scm(S.op(S.REQUEST_MODEL, S.i(211)) + S.op(S.LOAD_ALL_MODELS_NOW) +
              S.op(S.CREATE_CAR, S.i(211), S.f(x), S.f(y), S.f(z + 1.0), car) +
              S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, car) + S.op(S.SET_CAR_PROOFS, car, *([S.i(1)] * 5)) +
              S.op(S.MARK_MODEL_AS_NO_LONGER_NEEDED, S.i(211)))
        time.sleep(2.0)
        g.cmd("cruise start %s %.1f loop" % (route, opts["--speed"]))
        g.scm(S.op(0x0099, S.PLAYER, S.i(opts["--stars"]))) # ALTER_WANTED_LEVEL_NO_DROP
        t0 = time.time()
        while time.time() - t0 < opts["--seconds"]:
            pool = g.u32(0x487A98)
            entries, flags, size = struct.unpack("<3I", g.peek(pool, 12))
            used = g.peek(flags, size)
            ped = g.u32(0x4E4910)
            veh = g.u32(ped + 0x480)
            px, py = struct.unpack("<2f", g.peek(veh + 0x30, 8)) if veh else struct.unpack("<2f", g.peek(ped + 0x30, 8))
            fx, fy = struct.unpack("<2f", g.peek((veh or ped) + 0x10, 8))  # the matrix's forward row
            first = not seen
            now = set()
            near100 = near250 = 0
            nearest = None
            for k in range(size):
                if used[k] & 0x80:
                    continue
                v = entries + k * 0x8C0
                if v == veh:
                    continue
                head = g.peek(v + 0x30, 0x28)  # position (+0x30) .. model (+0x56): one read a car
                vx, vy = struct.unpack_from("<2f", head, 0)
                model = struct.unpack_from("<h", head, 0x26)[0]
                d = math.hypot(vx - px, vy - py)
                key = (k, used[k] & 0x7F)
                now.add(key)
                if key not in seen and not first and d < 160.0 and d > 1.0:
                    cos = ((vx - px) * fx + (vy - py) * fy) / (d * max(math.hypot(fx, fy), 1e-6))
                    if cos > math.cos(math.radians(45.0)):
                        popins.append({"t": round(time.time() - t0, 1), "model": model, "d": round(d, 1)})
                if model not in POLICE:
                    continue
                near100 += d < 100
                near250 += d < 250
                nearest = d if nearest is None else min(nearest, d)
            seen = now
            stars = g.u32(ped + 0x920)
            samples.append({"t": round(time.time() - t0, 1), "near100": near100, "near250": near250,
                            "nearest": round(nearest, 1) if nearest is not None else None, "stars": stars})
            time.sleep(max(0.0, 1.0 - (time.time() - t0) % 1.0))
        g.cmd("cruise stop")
    finally:
        g.quit()
    with open(os.path.join(out, "samples.jsonl"), "w") as f:
        for s in samples:
            f.write(json.dumps(s) + "\n")
    with open(os.path.join(out, "popins.jsonl"), "w") as f:
        for s in popins:
            f.write(json.dumps(s) + "\n")
    late = [s for s in samples if s["t"] >= 20]
    close = sum(1 for s in late if s["near100"])
    nearest = [s["nearest"] for s in late if s["nearest"] is not None]
    lasted = samples[-1]["t"] if samples else 0.0
    popOk = len(popins) <= max(1.0, lasted / 20.0)
    ok = late and (opts["--stars"] == 0 or close >= len(late) / 3) and popOk
    print("[chase_check] %s: police within 100 m in %d of %d s (after the first 20), within 250 m in %d; nearest median %s m;"
          " new cars in plain sight %d in %.0f s (%s)" % (
        "PASS" if ok else "FAIL", close, len(late), sum(1 for s in late if s["near250"]),
        sorted(nearest)[len(nearest) // 2] if nearest else "-", len(popins), lasted, out))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

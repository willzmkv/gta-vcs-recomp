#!/usr/bin/env python3
"""Hose the Hoes (mission 59, LOU_B1) check: what the mission counts per frame, at a simulated frame rate (GitHub
issue 144: the water cannon's fires never went out, the girls' bar ran down too fast).

    python tools/hose_check.py [OUT] [--step 144] [--port 47634] [--env NAME=VALUE ...]

From the Fort Baxter card, frame-rate independent timing with VCS_TIMESTEP=<step> (exactly 1/step s of game time a
frame, unpaced: any frame rate on this machine), the player invincible: start the mission and skip its cutscene, then
- the girls' bar ($G1513) while no fire is out: 1.2 a second at any step (the script takes 0.04 a frame - a PS2 frame -
  with fewer than two fires out; vcs_script_timing.cpp),
- the player warped into the mission's fire truck and taken near the brothel: from the moment the fires count
  ($G1506 = 1, TIMERA set to 0) the script adds TIMERA to $G14F1 every frame and sets the two parked burning cars'
  health to 1 at 1,500,000 and 3,000,000 ($G1504 / $G1505 = 1): 10.0 and 14.1 s of game time on the PS2 (30 fps),
  4.6 and 6.5 s at 144 fps before 2026-11-14.
The water cannon itself (how far it reaches, how long a fire takes): `framerate_audit.py --only hose`.
Results: OUT/result.json (default OUT: build/hose_check/<date-time>). Exit code 0 = within 5% of the PS2's numbers.
VCS_SCRIPT_TIMESTEP=0 (--env) brings the old behaviour back.
"""
import argparse
import datetime
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = rig.ROOT
SCRIPT_SPACE = 0x1DE0800   # *(0x48741C): global n is at SCRIPT_SPACE + 4 n (game-internals.md 11b)
CLOCK_MS = 0x4CD104        # CTimer::m_snTimeInMilliseconds
STATE = 0x14E5             # 0 start, 1 set-up, 2 get the fire truck, 3 the fires, 5 / 6 passed, 7 failed
GIRLS = 0x1513             # the girls' bar (float, 100 at the start)
TRUCK = 0x1565             # the mission's fire truck
FIRES_COUNT = 0x1506       # 1 from when the player brings the truck within 120 m of the brothel
CAR_FUSES = (0x1504, 0x1505)
NEAR_BROTHEL = (-962.0, 150.0)  # in the 120 m (the brothel's fires are around -962, 188)
PS2 = {"girls_per_s": 1.2, "fuse1_s": 10.0, "fuse2_s": 14.14}


def check(out, step=144, port=47634, env=None):
    os.makedirs(out, exist_ok=True)
    S = rig.Scm
    e = {"VCS_TIMING": "1", "VCS_TIMESTEP": str(step), "VCS_FPS": "0", "PS2X_INPUT": "injected",
         "VCS_SCRIPT_TIMESTEP_LOG": "1"}
    e.update(env or {})
    g = rig.Game.start(save=rig.default_save(), name="hose_check_%d" % step, port=port,
                       data_dir=os.path.join(ROOT, "build", "hose_check", "data_%d" % port), env=e)
    gi = lambda n: struct.unpack("<i", g.peek(SCRIPT_SPACE + 4 * n, 4))[0]
    gf = lambda n: g.peekf(SCRIPT_SPACE + 4 * n, 1)[0]
    gv = lambda n: bytes([0xCD + (n >> 8), n & 0xFF])  # a global as a script argument
    clock = lambda: g.u32(CLOCK_MS) / 1000.0
    result = {"step": step, "env": env or {}}
    try:
        g.wait_ingame()
        g.invincible()
        g.prime()
        g.mission(59)
        t0 = time.time()
        while gi(STATE) < 2 and time.time() - t0 < 180:
            g.cmd("press Shift 150")  # through the opening cutscene
            time.sleep(2.0)
        result["state_after_start"] = gi(STATE)
        if gi(STATE) != 2:
            raise RuntimeError("the mission did not reach the fire truck stage (state %d)" % gi(STATE))
        time.sleep(2.0)
        c0, b0 = clock(), gf(GIRLS)
        while clock() - c0 < 6.0:
            time.sleep(0.05)
        c1, b1 = clock(), gf(GIRLS)
        result["girls_per_s"] = (b0 - b1) / (c1 - c0)

        truck = gi(TRUCK)
        g.scm(S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, gv(TRUCK)))
        time.sleep(1.0)
        g.scm(S.op(S.LOAD_SCENE, S.f(NEAR_BROTHEL[0]), S.f(NEAR_BROTHEL[1]), S.f(10.0)) +
              S.op(S.SET_CAR_COORDINATES, gv(TRUCK), S.f(NEAR_BROTHEL[0]), S.f(NEAR_BROTHEL[1]), S.f(-100.0)))
        t0 = time.time()
        while not gi(FIRES_COUNT) and time.time() - t0 < 60:
            time.sleep(0.01)
        start = clock()
        result["truck"] = truck
        result["state_at_fires"] = gi(STATE)
        if not gi(FIRES_COUNT):
            raise RuntimeError("the fires never started counting (state %d)" % gi(STATE))
        for n, var in enumerate(CAR_FUSES, 1):
            while not gi(var) and clock() - start < 40.0:
                time.sleep(0.01)
            result["fuse%d_s" % n] = clock() - start if gi(var) else None
        result["state_at_end"] = gi(STATE)
    finally:
        g.quit()
    ok = True
    for k, want in PS2.items():
        v = result.get(k)
        result[k + "_ok"] = good = isinstance(v, float) and abs(v - want) <= 0.05 * want
        ok &= good
    result["pass"] = ok
    with open(os.path.join(out, "result.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)
    for k in PS2:
        print("[hose] %-12s %8s   (PS2 %.2f)%s" % (k, "%.2f" % result[k] if isinstance(result.get(k), float) else "-", PS2[k],
                                                 "" if result[k + "_ok"] else "  differs"))
    print("[hose] %s; %s" % ("pass" if ok else "FAIL", out))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--step", type=int, default=144)
    ap.add_argument("--port", type=int, default=47634)
    ap.add_argument("--env", nargs="*", default=[])
    a = ap.parse_args()
    out = a.out or os.path.join(ROOT, "build", "hose_check", datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    env = dict(kv.split("=", 1) for kv in a.env)
    sys.exit(0 if check(out, a.step, a.port, env) else 1)


if __name__ == "__main__":
    main()

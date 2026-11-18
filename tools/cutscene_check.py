#!/usr/bin/env python3
"""A cutscene's first frames look as the rest of it (docs/plans/2026-11-17-cutscene-loading.md).
    python tools/cutscene_check.py [OUT] [ENV=VAL ...]
The intro's first shot from an empty card: Vic arriving at Fort Baxter, the camera looking up at palms against the sky,
fading in from black. Every presented frame of its first seconds (the rig's `burst`), the brightness of a band of sky
(rows 1/6 .. 1/4). The sky must come up with the fade: no jump between two frames (the fade's own steps are ~10), and
bright at the end. Before the fix, the sky stayed dark grey under the fade and jumped to cyan when the fade ended: the
renderer's HUD classifier took the sky's full-screen fill for the fade quad (same corners) and moved it into the HUD
layer (game-internals.md 6f, src/game/vcs_hud.cpp g_otherKeys). VCS_HUD_NATIVE=0 is the reference without the layer.
"""
import glob
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

MAX_STEP = 45.0   # largest brightness change between two presented frames
BRIGHT = 150.0    # the sky band at the end of the burst


def main():
    args = [a for a in sys.argv[1:] if "=" not in a]
    env = {"VCS_PERF_OVERLAY": "0"}
    env.update(dict(a.split("=", 1) for a in sys.argv[1:] if "=" in a))
    out = os.path.abspath(args[0] if args else os.path.join(rig.ROOT, "build", "cutscene_check", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    burst = os.path.join(out, "burst").replace("\\", "/")
    os.makedirs(burst, exist_ok=True)
    g = rig.Game.start(save=None, name="cutscene_check", seconds=300, data_dir=os.path.join(out, "data"), env=env)
    try:
        t0 = time.time()
        while int(g.cmd("frames")) < 25:  # the loading screens are over at ~frame 30; the palms fade in from ~50
            if time.time() - t0 > 120:
                raise RuntimeError("the game did not start")
            time.sleep(0.05)
        g.cmd("burst %s 260" % burst)
        last = -1
        while True:
            time.sleep(2)
            c = len(glob.glob(burst + "/*.png"))
            if c == last:
                break
            last = c
    finally:
        g.quit()
    levels = []
    for p in sorted(glob.glob(os.path.join(burst, "burst_*.png"))):
        w, h, rows = tc.read_png(p)
        band = b"".join(rows[y] for y in range(h // 6, h // 4))
        levels.append(round(sum(band) / max(1, len(band)), 1))
    # from the first frame the fade shows anything (the loading screens before it are bright too: start at the black)
    first_black = next((i for i, v in enumerate(levels) if v < 3.0), None)
    shot = levels[first_black:] if first_black is not None else []
    steps = [abs(b - a) for a, b in zip(shot, shot[1:])]
    worst = max(steps) if steps else 0.0
    end = max(shot[-30:]) if shot else 0.0
    ok = bool(shot) and worst <= MAX_STEP and end >= BRIGHT
    with open(os.path.join(out, "levels.json"), "w") as f:
        json.dump({"levels": levels, "first_black": first_black, "worst_step": worst, "end": end, "pass": ok}, f)
    print("[cutscene_check] %s: the sky band from black to %.0f, its largest step between frames %.0f (at most %.0f) (%s)" % (
        "PASS" if ok else "FAIL", end, worst, MAX_STEP, out))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

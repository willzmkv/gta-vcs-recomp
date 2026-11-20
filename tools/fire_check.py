#!/usr/bin/env python3
"""Fire in the game: a burning car, the flame-thrower firing, a molotov's fire on the ground - photographed.

    python tools/fire_check.py [OUT] [--save DIR] [--variant NAME=ENV=VAL,ENV=VAL ...]

Reported by the player (2026-11-03): no flames at all - the flame-thrower's, a molotov's, a burning car's. One hidden run
per variant (default: `as-is`, and `ps2-memory` with VCS_STREAMING_MB=0) from the card copy (default the rig's user copy)
at the police spot (tools/spots.py), noon, the player invincible: a car 7 m ahead set to health 200 (it burns below 250) and
photographed every 0.6 s (`<variant>_car_N.png`); the flame-thrower (type 33) fired
(`<variant>_flame_N.png`); a molotov (type 17) thrown (`<variant>_molotov_N.png`), every 0.4 s - all with the game's
camera behind him (the free camera takes the controls: no shots). Pass = the game alive and fire on screen: fire-coloured
pixels in the best flame-thrower frame >= 80 and molotov frame >= 150 (a burning car's counted, not judged). Results:
OUT/result.json; exit code 0 = pass. Outputs: OUT (game pictures: build/).
"""
import argparse
import json
import math
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import spots  # noqa: E402
import toolchain as tc  # noqa: E402

SET_CAR_HEALTH = 0x014C
# Fire-coloured pixels (red > 200, blue < 110, red - blue > 120, green > 60; every second pixel of every second row) in
# the best frame of each: with the flames missing 0-12, with them drawn 124-461 (flame-thrower) and 351-8461 (molotov)
FLAME_MIN, MOLOTOV_MIN = 80, 150


def fire_pixels(path):
    w, h, rows = tc.read_png(str(path))
    n = 0
    for y in range(0, h, 2):
        r = rows[y]
        for x in range(0, w * 3, 6):
            if r[x] > 200 and r[x + 2] < 110 and r[x] - r[x + 2] > 120 and r[x + 1] > 60:
                n += 1
    return n


def look_at(g, eye, target):
    yaw = math.degrees(math.atan2(target[1] - eye[1], target[0] - eye[0]))
    pitch = math.degrees(math.atan2(target[2] - eye[2], math.hypot(target[0] - eye[0], target[1] - eye[1])))
    g.cmd('campose %.3f %.3f %.3f %.2f %.2f' % (eye[0], eye[1], eye[2], yaw, pitch))


def facing(g):
    ped, (px, py, pz), _ = g.player()
    fx, fy = g.peekf(ped + 0x10, 2)
    n = math.hypot(fx, fy) or 1.0
    return (px, py, pz), (fx / n, fy / n)


def run(out, name, env, save):
    g = rig.Game.start(save=save, name='fire_' + name, seconds=240, env=env)
    # the player's control scheme is theirs (vcs.ini): this run takes the PS2's - mouse fires, no trigger schemes.
    # Set through the control channel, a hidden run's settings last for the run only (2026-11-17: with the Modern
    # scheme chosen the checks saw no fire at all)
    pinned = {'pad.trigger_shoot': 0, 'pad.trigger_drive': 0, 'pad.free_aim': 0}
    shots = []
    try:
        g.wait_ingame(timeout=180)
        for key, value in pinned.items():
            g.cmd('setting %s %d' % (key, value))
        g.set_time(12)
        g.weather(0)
        g.invincible(True)
        # the police spot (tools/spots.py): the game's camera behind Vic on a street. gamefiles_check's street left the
        # camera looking straight down on the sand with the car out of the picture (2026-11-17), and the sand's colour
        # passed for fire at the old exposure of 120
        spots.wait_after_load()
        spots.put(g, 'police', camera=False)
        time.sleep(3)
        S = rig.Scm
        (px, py, pz), (fx, fy) = facing(g)
        cx, cy = px + fx * 7.0, py + fy * 7.0
        ints, floats = g.scm_outputs(
            S.op(S.REQUEST_MODEL, S.i(211)) + S.op(S.LOAD_ALL_MODELS_NOW) +
            S.op(S.CREATE_CAR, S.i(211), S.f(cx), S.f(cy), S.f(pz + 1.0), S.LOCAL0) +
            S.op(S.SET_CAR_COORDINATES, S.LOCAL0, S.f(cx), S.f(cy), S.f(-100.0)) +
            S.op(0x004D, S.LOCAL0, b"\x0e", b"\x0f", b"\x10") +  # GET_CAR_COORDINATES -> locals 1..3
            S.op(S.MARK_MODEL_AS_NO_LONGER_NEEDED, S.i(211)), 4)
        car = ints[0]
        time.sleep(1.0)
        # the game's own camera behind him (the free camera takes the player's controls: no shots with it)
        g.scm(S.op(SET_CAR_HEALTH, S.i(car), S.i(200)))  # it burns below 250, then blows up
        for k in range(8):  # the fire grows from the engine, then the car blows up
            time.sleep(0.6)
            shots.append(out / ('%s_car_%d.png' % (name, k)))
            g.cmd('screenshot ' + str(shots[-1]))
        time.sleep(3)
        for weapon, tag, hold in ((33, 'flame', 0.8), (17, 'molotov', 0.25)):
            g.cmd('trainer weapon %d' % weapon)
            time.sleep(2)
            g.cmd('key mouseleft down')
            for k in range(6):
                time.sleep(hold if k == 0 else 0.4)
                if tag == 'molotov' and k == 0:
                    g.cmd('key mouseleft up')
                shots.append(out / ('%s_%s_%d.png' % (name, tag, k)))
                g.cmd('screenshot ' + str(shots[-1]))
            g.cmd('key mouseleft up')
            time.sleep(2)
        alive = g.alive()
    finally:
        g.quit()
    return shots, alive


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?')
    ap.add_argument('--save', default=os.path.join(tc.ROOT, 'build', 'rig', 'saves', 'user'))
    ap.add_argument('--variant', action='append', default=[], help='NAME=ENV=VAL,ENV=VAL (repeatable)')
    a = ap.parse_args()
    out = Path(a.out or os.path.join(tc.ROOT, 'build', 'fire_check', time.strftime('%Y%m%d-%H%M%S'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    variants = {'as-is': {}, 'ps2-memory': {'VCS_STREAMING_MB': '0'}}
    if a.variant:
        variants = {}
        for v in a.variant:
            name, _, rest = v.partition('=')
            variants[name] = dict(kv.split('=', 1) for kv in rest.split(',') if kv)
    results, ok = {}, True
    for name, env in variants.items():
        shots, alive = run(out, name, env, a.save)
        counts = {s.stem: fire_pixels(s) for s in shots if s.is_file()}
        best = {kind: max([n for k, n in counts.items() if '_%s_' % kind in k] or [0]) for kind in ('car', 'flame', 'molotov')}
        passed = alive and best['flame'] >= FLAME_MIN and best['molotov'] >= MOLOTOV_MIN
        ok &= passed
        results[name] = {'alive': alive, 'fire_pixels': counts, 'best': best, 'pass': passed}
        print('%-12s %s  fire pixels (best frame): car %d, flame-thrower %d, molotov %d  %s' % (
            name, 'alive' if alive else 'DEAD', best['car'], best['flame'], best['molotov'], 'ok' if passed else 'FAIL'), flush=True)
    (out / 'result.json').write_text(json.dumps(results, indent=1), encoding='utf-8')
    print('PASS' if ok else 'FAIL', '-', out)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

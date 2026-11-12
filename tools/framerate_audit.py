#!/usr/bin/env python3
"""Frame-rate dependence audit: the same scripted scenarios at forced time steps, compared per simulated second.

    python tools/framerate_audit.py [OUT] [--steps 15,30,60,100] [--only fall,run,...]

Each step N is its own game run in frame-rate independent timing with VCS_TIMESTEP=N: CTimer is given exactly 1/N s
per frame (in whole milliseconds, carried) and the game runs unpaced, so behaviour can be compared per simulated
second at the equivalent of any frame rate on this machine. Simulated time is the game's own clock
(CTimer::m_snTimeInMilliseconds, 0x4CD104). Input is injected over the control channel (PS2X_INPUT=injected: the
keyboard / pad are ignored). From a copy of the Fort Baxter card; noon, sunny, invincible.

Scenarios (each from a fresh teleport, so earlier ones don't leak into later ones):
  fall    the player dropped from 30 m above the ground: time to fall 15 m, where he lands
  run     W held for 4 s: speed over the last 3 s
  sprint  W + Cross held for 4 s: speed over the last 3 s
  jump    Square on foot: rise and airtime
  car     in a car (model 211), Cross held for 4 s: speed at 1 / 2 / 3 / 4 s (the car's move speed), distance
  coast   the car at full throttle for 1 s, then let go: its deceleration over the next second (rolling resistance)
  runover the car at full throttle at a dead ped 14 m ahead: how high it rises, how fast it is thrown up
  bikebody a PCJ-600 rolling slowly past a dead ped at its rear wheel: how far it pitches (forward vector z), whether
          it ends upright (up z)
  bikecoast the PCJ-600 at full throttle for 0.4 s, then let go: its deceleration
  bikelean the PCJ-600 in a gentle left turn (0.4 s of throttle, then left for 0.5 s): its lean per g of sideways
          acceleration
  brake   the car on the airport's runway at full throttle for 3 s, then the brake for 2.5 s: deceleration, nose dip,
          how far its heading swings
  bikebrake the PCJ-600 there at full throttle for 6 s (36 m/s), then the brake: deceleration, nose dip, the heading's
          swing (fishtailing: 11 degrees at 30 fps, 58 at 144 and spun round onto its side at 240 before the brake
          wobble was put on the step), lean, roll at the end (up z), whether the rider came off
  rotor   a Maverick standing in an alley narrower than its rotor: the health its blades cost per second (per frame
          before 2026-11-10: 382 a second at 30 fps, 1590 at 144 - on fire within a second)
  carwater the car dropped into the sea at 15 m/s: its speed 1 s after it reaches the water (the water's drag)
  bikewater the PCJ-600 dropped into the sea: how deep it settles
  ferris  the player in a ferris wheel carriage for 10 s: how long he stays where he stood in it, how far he drifts
  carcam  the car camera after a left turn: how long its lag behind the car takes to halve once the car runs straight
  footcam the on-foot camera while the player runs to its right: how fast it turns after him
  clock   game clock ms per frame vs the step; frames per simulated second
  heat    PHI_A2's heat bar: its floor and the heat at +1 a frame, per simulated second (starts the mission: last)

    python tools/framerate_audit.py --sites

checks instead that every per-frame mission meter src/game/vcs_script_timing.cpp puts on the time step is found where
its table says (a game per mission, VCS_SCRIPT_TIMESTEP_CHECK=1).

Prints, per scenario and metric, each step's value and its difference from the 30 fps result (the original frame
rate), flagging differences over --tolerance (default 5%). Writes OUT/audit.json (OUT: build/audit/<date-time>).
"""
import argparse
import datetime
import json
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = rig.ROOT
CLOCK_MS = 0x4CD104      # CTimer::m_snTimeInMilliseconds
PED_VEHICLE = 0x450      # ped -> its vehicle (found by scanning the ped for the car's address, game-internals.md 12)
MOVE_SPEED = 0x140       # CPhysical move speed (x, y, z), units per 1/50 s (z found mid-fall at +0x148)
FRAME_COUNTER = 0x4CD124  # CTimer::m_FrameCounter
# open ground for the on-foot and car runs (spots from the world tour, checked by screenshot): x, y, heading - a long
# straight pavement beside the open tarmac near the airport (the Fort Baxter door faces a fence; -675,-75 is sea)
FOOT_SPOT = (-1425.0, -575.0, 90.0)
CAR_SPOT = (-1425.0, -640.0, 0.0)  # a new car faces north; open for 4 s of full throttle here (scratch probe)
# the airport: a PCJ-600 held at full throttle north from here ran 274 m in 9 s, up to 40 m/s, without hitting
# anything (a probe of candidate lines; x -1700 has an obstacle at y -1369, CAR_SPOT a 7 m drop ~70 m north)
RUNWAY_SPOT = (-1600.0, -1450.0, 0.0)


class Session:
    def __init__(self, step, name, port):
        env = {"VCS_ASPECT": "43", "VCS_TIMING": "1", "VCS_TIMESTEP": str(step), "VCS_FPS": "0", "PS2X_INPUT": "injected"}
        self.step = step
        self.in_car = False
        self.g = rig.Game.start(save=rig.default_save(), name=name, port=port,
                                data_dir=os.path.join(ROOT, "build", "audit", "data_%d" % step), env=env)
        g = self.g
        g.wait_ingame()
        g.set_time(12)
        g.weather(0)
        g.invincible()
        g.prime()

    def clock(self):
        return self.g.u32(CLOCK_MS) / 1000.0

    def pos(self):
        """The player's position - the vehicle's while in one (the ped's own matrix isn't its place then)."""
        ped, p, _ = self.g.player()
        vehicle = self.g.u32(ped + PED_VEHICLE) if self.in_car else 0
        if vehicle:
            return tuple(self.g.peekf(vehicle + 0x30, 3))
        return p

    def sample(self, seconds, every=0.0):
        """[(sim time, (x, y, z))] for `seconds` of simulated time from now."""
        out = []
        t0 = self.clock()
        while True:
            t = self.clock()
            out.append((t - t0, self.pos()))
            if t - t0 >= seconds:
                return out
            if every:
                time.sleep(every)

    def wait_sim(self, seconds):
        t0 = self.clock()
        while self.clock() - t0 < seconds:
            time.sleep(0.01)

    def key(self, name, down):
        self.g.cmd("key %s %s" % (name, "down" if down else "up"))

    def place(self, x, y, z=-100.0, heading=0.0):
        self.g.teleport(x, y, z, heading=heading)
        self.wait_sim(1.5)

    def quit(self):
        self.g.quit()


def horizontal_speed(samples, t_from, t_to):
    """Average horizontal speed (m/s) between two simulated times (nearest samples)."""
    a = min(samples, key=lambda s: abs(s[0] - t_from))
    b = min(samples, key=lambda s: abs(s[0] - t_to))
    if b[0] <= a[0]:
        return 0.0
    return math.hypot(b[1][0] - a[1][0], b[1][1] - a[1][1]) / (b[0] - a[0])


def scenario_fall(s):
    x, y, h = FOOT_SPOT
    s.place(x, y, heading=h)
    ground = s.pos()[2]
    s.g.teleport(x, y, ground + 30.0, heading=h)
    samples = s.sample(5.0)
    z0 = samples[0][1][2]
    t15 = None
    for (ta, pa), (tb, pb) in zip(samples, samples[1:]):
        if pa[2] > z0 - 15.0 >= pb[2]:
            t15 = ta + (tb - ta) * (pa[2] - (z0 - 15.0)) / max(pa[2] - pb[2], 1e-6)
            break
    return {"fall_15m_s": t15, "landed_z_above_ground": samples[-1][1][2] - ground}


def scenario_foot(s, keys):
    x, y, h = FOOT_SPOT
    s.place(x, y, heading=h)
    for k in keys:
        s.key(k, True)
    try:
        if os.environ.get("AUDIT_SHOTS"):
            s.wait_sim(1.5)
            for i in range(3):
                s.g.cmd("screenshot " + os.path.join(os.environ["AUDIT_SHOTS"], "foot_%d_%d.png" % (s.step, i)))
        samples = s.sample(4.0)
    finally:
        for k in keys:
            s.key(k, False)
    return {"speed_m_s": horizontal_speed(samples, 1.0, 4.0)}


def scenario_jump(s):
    x, y, h = FOOT_SPOT
    s.place(x, y, heading=h)
    z0 = s.pos()[2]
    s.key("Space", True)
    samples = s.sample(2.5)
    s.key("Space", False)
    top = max(p[2] for _, p in samples)
    airborne = [t for t, p in samples if p[2] > z0 + 0.05]
    return {"rise_m": top - z0, "airtime_s": (airborne[-1] - airborne[0]) if len(airborne) > 1 else 0.0}


def scenario_car(s):
    x, y, h = CAR_SPOT
    s.place(x, y, heading=h)
    S = rig.Scm
    _, (px, py, pz), _ = s.g.player()
    s.g.scm(S.op(S.REQUEST_MODEL, S.i(211)) + S.op(S.LOAD_ALL_MODELS_NOW) +
            S.op(S.CREATE_CAR, S.i(211), S.f(px), S.f(py), S.f(pz + 1.0), S.LOCAL0) +
            S.op(S.SET_CAR_PROOFS, S.LOCAL0, S.i(1), S.i(1), S.i(1), S.i(1), S.i(1)) +
            S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, S.LOCAL0) + S.op(S.MARK_MODEL_AS_NO_LONGER_NEEDED, S.i(211)))
    s.wait_sim(1.0)
    vehicle = s.g.u32(s.g.player()[0] + PED_VEHICLE)
    if not vehicle:
        raise RuntimeError("not in the car")
    s.in_car = True
    s.wait_sim(2.0)
    if os.environ.get("AUDIT_SHOTS"):
        s.g.cmd("screenshot " + os.path.join(os.environ["AUDIT_SHOTS"], "car_%d_a.png" % s.step))
    s.key("Shift", True)
    speeds = []  # (sim time, m/s from the car's own move speed - positions read apart from the clock are too noisy)
    try:
        t0 = s.clock()
        while True:
            t = s.clock() - t0
            v = s.g.peekf(vehicle + MOVE_SPEED, 3)
            speeds.append((t, math.hypot(v[0], v[1]) * 50.0))
            if t >= 4.0:
                break
    finally:
        if os.environ.get("AUDIT_SHOTS"):
            s.g.cmd("screenshot " + os.path.join(os.environ["AUDIT_SHOTS"], "car_%d_b.png" % s.step))
        s.key("Shift", False)
    s.in_car = False

    def at(t):
        return min(speeds, key=lambda x: abs(x[0] - t))[1]

    # distance by integrating the speed
    distance = sum((b[0] - a[0]) * (a[1] + b[1]) / 2 for a, b in zip(speeds, speeds[1:]))
    return {"speed_1s": at(1.0), "speed_2s": at(2.0), "speed_3s": at(3.0), "speed_4s": at(4.0), "distance_4s": distance}


DELETE_CHAR = 0x003F  # handlers next to CREATE_CHAR / CREATE_CAR, as in VC's order; checked: the pool slot frees
DELETE_CAR = 0x0049
BIKE = 212  # PCJ-600


PED_STATE = 0x8C4  # 1 on foot (game-internals.md 12)


def leave_vehicle(s):
    """Triangle (F) until the player is on foot (a few seconds at most). Not just out of the vehicle (+0x450 clears
    while he still climbs out): a warp into a new car then left it frozen."""
    def on_foot():
        ped = s.g.player()[0]
        return not s.g.u32(ped + PED_VEHICLE) and s.g.u32(ped + PED_STATE) == 1
    vehicle = s.g.u32(s.g.player()[0] + PED_VEHICLE)
    for _ in range(60):  # stopped first: out of a moving car he falls (state 0x3A), and a warp from there froze the car
        if not vehicle or math.hypot(*s.g.peekf(vehicle + MOVE_SPEED, 3)) * 50.0 < 0.3:
            break
        s.wait_sim(0.1)
    for _ in range(3):
        if on_foot():
            return
        if s.g.u32(s.g.player()[0] + PED_VEHICLE):
            s.g.cmd("press F 300")
        for _ in range(40):
            s.wait_sim(0.1)
            if on_foot():
                return


def vehicle_run(s, name, model, act, body_at=None, extra="", spot=CAR_SPOT):
    """A vehicle scenario from `spot` (x, y, heading; CAR_SPOT), the player in a new `model`; body_at: a dead ped (fwd,
    right) metres from the vehicle. act(s) drives it while the rig's `trace` records it every frame. Returns the trace
    rows (sim time s, then the vehicle's matrix (16 floats), turn speed +0x70, move speed +0x140, bike lean +0x680 /
    +0x684, then the `extra` trace spans' floats - "0x<addr> <count> ...", VEH+0x<offset> for a field of the vehicle).
    The vehicle and the ped are deleted afterwards, so the next scenario starts on clear ground."""
    g, S = s.g, rig.Scm
    x, y, h = spot
    leave_vehicle(s)  # (scenario_car leaves him in its car: a teleport would bring the car along)
    s.place(x, y, heading=h)
    # beside him, at the height he stands: with z = -100 (find the ground) the vehicle was sometimes put 1.6 m under the
    # road and stayed frozen there
    z = s.pos()[2] + 0.5
    ints, _ = g.scm_outputs(S.op(S.REQUEST_MODEL, S.i(model)) + S.op(S.REQUEST_MODEL, S.i(55)) + S.op(S.LOAD_ALL_MODELS_NOW) +
                            S.op(S.CREATE_CAR, S.i(model), S.f(x + 4.0), S.f(y), S.f(z), S.LOCAL0) +
                            S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, S.LOCAL0), 1)
    car = ints[0]
    pool = g.u32(0x487A98)
    vehicle = g.u32(pool) + (car >> 8) * 0x8C0  # CPool: slot = handle >> 8 (game-internals.md 12)
    s.wait_sim(2.0)
    for _ in range(3):  # a warp right after getting out of the last vehicle can be lost: until he sits in this one
        if g.u32(g.player()[0] + PED_VEHICLE) == vehicle:
            break
        g.scm(S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, S.i(car)))
        s.wait_sim(1.5)
    else:
        raise RuntimeError("not in the %s" % name)
    ped = None
    if body_at:
        vx, vy, vz = g.peekf(vehicle + 0x30, 3)
        fx, fy, _ = g.peekf(vehicle + 0x10, 3)
        rx, ry, _ = g.peekf(vehicle + 0x00, 3)
        ints, _ = g.scm_outputs(S.op(S.CREATE_CHAR, S.i(4), S.i(55), S.f(vx + fx * body_at[0] + rx * body_at[1]),
                                     S.f(vy + fy * body_at[0] + ry * body_at[1]), S.f(vz), S.LOCAL0) +
                                S.op(S.SET_CHAR_HEALTH, S.LOCAL0, S.i(0)), 1)
        ped = ints[0]
        s.wait_sim(2.5)  # dies and lies down
    path = os.path.join(ROOT, "build", "audit", "trace_%d_%s.txt" % (s.step, name))
    # "VEH+0x<off>" in `extra`: a field of this vehicle
    extra = re.sub(r"VEH\+0x([0-9a-fA-F]+)", lambda m: "0x%x" % (vehicle + int(m.group(1), 16)), extra)
    g.cmd("trace %s 0x%x 16 0x%x 3 0x%x 3 0x%x 2 %s" % (path, vehicle, vehicle + 0x70, vehicle + 0x140, vehicle + 0x680,
                                                        extra))
    try:
        act(s)
    finally:
        g.cmd("trace off")
        for k in ("Shift", "A", "D", "W", "S"):
            s.key(k, False)
    leave_vehicle(s)  # then both gone
    g.scm(S.op(DELETE_CAR, S.i(car)) + (S.op(DELETE_CHAR, S.i(ped)) if ped is not None else b""))
    rows = [[float(v) for v in line.split()] for line in open(path)]
    t0 = rows[0][1]
    return [[(r[1] - t0) / 1000.0] + r[3:] for r in rows]


def held(key, seconds, then=0.0):
    def act(s):
        s.key(key, True)
        s.wait_sim(seconds)
        s.key(key, False)
        s.wait_sim(then)
    return act


# trace row columns (after the sim time): matrix right 0-2, forward 4-6, up 8-10, position 12-14; turn speed 16-18;
# move speed 19-21 (units per 1/50 s: x 50 = m/s); lean 23 (radians)
def speed(r):
    return math.hypot(r[1 + 19], r[1 + 20]) * 50.0


def deceleration(rows):
    """From the fastest moment: m/s^2 over the next second (or until nearly stopped)."""
    i = max(range(len(rows)), key=lambda k: speed(rows[k]))
    j = next((k for k in range(i, len(rows)) if rows[k][0] >= rows[i][0] + 1.0 or speed(rows[k]) < 0.5), len(rows) - 1)
    return (speed(rows[i]) - speed(rows[j])) / max(rows[j][0] - rows[i][0], 1e-6)


def scenario_coast(s):
    rows = vehicle_run(s, "coast", 211, held("Shift", 1.0, then=2.0))
    return {"peak_m_s": max(speed(r) for r in rows), "decel_m_s2": deceleration(rows)}


def scenario_runover(s):
    rows = [r for r in vehicle_run(s, "runover", 211, held("Shift", 3.4), body_at=(14.0, 0.0)) if r[0] <= 3.4]
    # rise: above the lowest point of the 0.6 s before (the road isn't level)
    rise = max(r[1 + 14] - min(q[1 + 14] for q in rows if r[0] - 0.6 <= q[0] <= r[0]) for r in rows)
    return {"rise_m": rise, "throw_m_s": max(r[1 + 21] for r in rows) * 50.0, "pitch_rate": max(abs(r[1 + 16]) for r in rows)}


def scenario_bikebody(s):
    rows = vehicle_run(s, "bikebody", BIKE, held("Shift", 0.25, then=4.0), body_at=(-0.8, 0.6))
    return {"nose_fwd_z": max(abs(r[1 + 6]) for r in rows), "end_up_z": rows[-1][1 + 10]}


def scenario_bikecoast(s):
    rows = vehicle_run(s, "bikecoast", BIKE, held("Shift", 0.4, then=2.0))
    return {"decel_m_s2": deceleration(rows)}


def brake_run(s, name, model, accelerate):
    """Full throttle for `accelerate` s, then the brake (S: Square while driving) for 2.5 s. The player's vehicle
    pointer is traced too (as a float: 0 once he is off), to see the rider thrown over the bars."""
    ped = s.g.player()[0]
    def act(s):
        s.key("W", True)
        s.wait_sim(accelerate)
        s.key("W", False)
        s.key("S", True)
        s.wait_sim(2.5)
        s.key("S", False)
        s.wait_sim(0.5)
    rows = vehicle_run(s, name, model, act, extra="0x%x 1" % (ped + PED_VEHICLE), spot=RUNWAY_SPOT)
    peak = max(range(len(rows)), key=lambda k: speed(rows[k]))
    t0 = rows[peak][0]
    def at(t):
        return min(rows, key=lambda r: abs(r[0] - t))
    braking = [r for r in rows if t0 <= r[0] <= t0 + 2.5]
    def heading(r):
        return math.degrees(math.atan2(-r[1 + 4], r[1 + 5]))
    h0 = heading(rows[peak])
    # the speed lost in the first 0.5 s of braking (before a slow bike's rear wheel lifts or it stops)
    return {"peak_m_s": speed(rows[peak]), "decel_m_s2": (speed(rows[peak]) - speed(at(t0 + 0.5))) / 0.5,
            "nose_fwd_z": min(r[1 + 6] for r in braking),
            # turn speed about the vehicle's right axis (nose down / up), radians per 1/50 s
            "pitch_rate": max(abs(sum(r[1 + 16 + i] * r[1 + i] for i in range(3))) for r in braking),
            "rider_off": float(any(r[1 + 24] == 0.0 for r in braking)),
            # how far the heading swings from where it pointed (a bike fishtailing), the lean drawn, the bike's roll
            "heading_deg": max(abs((heading(r) - h0 + 180.0) % 360.0 - 180.0) for r in braking),
            "lean_deg": max(abs(math.degrees(r[1 + 23])) for r in braking),
            "end_up_z": braking[-1][1 + 10]}


def scenario_brake(s):
    return brake_run(s, "brake", 211, 3.0)


def scenario_bikebrake(s):
    return brake_run(s, "bikebrake", BIKE, 6.0)


def scenario_bikelean(s):
    """The lean (+0x684) for the sideways acceleration (speed x yaw rate, in g): its median over the frames of the turn
    with more than 0.25 g. The peak lean followed how long the key was held (a frame at 30 fps is 13% of a short turn);
    the ratio doesn't. 0.6 at any frame rate; 0.49 at 144 fps with CBike's max(step, 0.5) unpatched."""
    def act(s):
        s.key("Shift", True)
        s.wait_sim(0.4)
        s.key("A", True)
        s.wait_sim(0.5)
        s.key("A", False)
        s.key("Shift", False)
        s.wait_sim(1.0)
    rows = vehicle_run(s, "bikelean", BIKE, act)
    ratios = []
    for r in rows:
        g_side = math.hypot(r[1 + 19], r[1 + 20]) * r[1 + 18] / 0.008  # units per (1/50 s)^2 over GRAVITY 0.008
        if abs(g_side) > 0.25:
            ratios.append(math.sin(abs(r[1 + 23])) / abs(g_side))
    ratios.sort()
    return {"lean_per_g": ratios[len(ratios) // 2] if ratios else 0.0,
            "lean_deg": max(abs(math.degrees(r[1 + 23])) for r in rows)}


SEA_SPOT = (-675.0, -75.0)  # open sea; the player swims there, at the water's surface


def water_run(s, name, model):
    """A vehicle (no driver) dropped from 8 m into the sea with 15 m/s forwards; 8 s of trace rows (as vehicle_run) and
    the water's height. The player swims beside it and goes back to land afterwards."""
    g, S = s.g, rig.Scm
    leave_vehicle(s)
    g.teleport(SEA_SPOT[0], SEA_SPOT[1], -100.0)
    s.wait_sim(2.0)
    px, py, water = s.pos()
    ints, _ = g.scm_outputs(S.op(S.REQUEST_MODEL, S.i(model)) + S.op(S.LOAD_ALL_MODELS_NOW) +
                            S.op(S.CREATE_CAR, S.i(model), S.f(px + 10.0), S.f(py - 20.0), S.f(water + 8.0), S.LOCAL0), 1)
    vehicle = g.u32(g.u32(0x487A98)) + (ints[0] >> 8) * 0x8C0
    path = os.path.join(ROOT, "build", "audit", "trace_%d_%s.txt" % (s.step, name))
    g.cmd("trace %s 0x%x 16 0x%x 3 0x%x 3" % (path, vehicle, vehicle + 0x70, vehicle + 0x140))
    g.cmd("pokef 0x%x 0 0.3 0" % (vehicle + MOVE_SPEED))  # 0.3 units per 1/50 s = 15 m/s north
    s.wait_sim(8.0)
    g.cmd("trace off")
    g.scm(S.op(DELETE_CAR, S.i(ints[0])))
    x, y, h = FOOT_SPOT
    s.place(x, y, heading=h)
    rows = [[float(v) for v in line.split()] for line in open(path)]
    t0 = rows[0][1]
    return [[(r[1] - t0) / 1000.0] + r[3:] for r in rows], water


def scenario_carwater(s):
    """Above 100 fps CAutomobile's in-water code (FUN_00138b68) took the share under water as step / 0.5 too small
    (max(step, 0.5)): less drag, 1.6 m/s left after 1 s at 144 fps against 0.6."""
    rows, water = water_run(s, "carwater", 211)
    entry = next(r[0] for r in rows if r[1 + 14] < water + 0.5)
    r = min(rows, key=lambda r: abs(r[0] - (entry + 1.0)))
    return {"speed_1s_in_m_s": speed(r)}


def scenario_bikewater(s):
    """The bike's (FUN_00336cb8): it sank to the bottom at 144 fps (z 1.2) where it settles at 3.2."""
    rows, water = water_run(s, "bikewater", BIKE)
    zs = [r[1 + 14] for r in rows if 3.0 <= r[0] <= 8.0]
    return {"depth_m": water - sum(zs) / len(zs)}


ROTOR_ALLEY = (306.0, -648.0)  # an alley downtown, 8 m between its walls: a helicopter's main rotor is in both
MAVERICK = 213
VEHICLE_HEALTH = 0x27C  # float, 1000 new (found 2026-11-10: the float that falls while the blades are in the walls)


def scenario_rotor(s):
    """CVehicle's blade test (FUN_003EE308) sets a damage impulse on every frame a blade touches something, and the
    vehicle loses the same health for it each frame (12.75 for the Maverick against a wall) - 4.8 times as fast at 144
    fps (GitHub issue 102). vcs_physics_timing.cpp lets the blades count once per 1/30 s."""
    g, S = s.g, rig.Scm
    leave_vehicle(s)
    g.teleport(ROTOR_ALLEY[0], ROTOR_ALLEY[1], -100.0)
    s.wait_sim(2.0)
    x, y, z = s.pos()
    ints, _ = g.scm_outputs(S.op(S.REQUEST_MODEL, S.i(MAVERICK)) + S.op(S.LOAD_ALL_MODELS_NOW) +
                            S.op(S.CREATE_CAR, S.i(MAVERICK), S.f(x), S.f(y), S.f(-100.0), S.LOCAL0) +
                            S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, S.LOCAL0), 1)
    vehicle = g.u32(g.u32(0x487A98)) + (ints[0] >> 8) * 0x8C0
    rows, t0 = [], s.clock()
    while s.clock() - t0 < 4.0:
        rows.append((s.clock() - t0, g.peekf(vehicle + VEHICLE_HEALTH, 1)[0]))
        time.sleep(0.005)
    # between 900 and 300: the rotor has spun up, and the helicopter is not burning yet (below 250 it loses more)
    a = next((r for r in rows if r[1] < 900.0), None)
    b = next((r for r in rows if r[1] < 300.0), rows[-1])
    S2 = rig.Scm
    g.scm(S2.op(0x0216, S2.PLAYER, S2.f(x), S2.f(y + 12.0), S2.f(z)) + S2.op(DELETE_CAR, S2.i(ints[0])))  # WARP_CHAR_FROM_CAR_TO_COORD
    s.wait_sim(1.0)
    fx, fy, h = FOOT_SPOT
    s.place(fx, fy, heading=h)
    if not a or b[0] <= a[0]:
        return {"health_per_s": 0.0, "health_after_4s": rows[-1][1]}
    return {"health_per_s": (a[1] - b[1]) / (b[0] - a[0]), "seconds_900_to_300": b[0] - a[0], "health_after_4s": rows[-1][1]}


OBJECT_POOL = 0x487AA4  # CPool of objects: 475 entries of 0x220 bytes (game-internals.md 12)
FERRIS_CARRIAGE = 7069  # the model of the ferris wheel's 12 carriages, which turn in the plane x = 342.19
FERRIS_SPOT = (342.0, -250.0)  # on the ground beside the wheel


def pool_objects(g, model):
    """The addresses of the objects of one model in the object pool."""
    import struct
    pool = g.u32(OBJECT_POOL)
    ent, flags, n = g.u32(pool), g.u32(pool + 4), g.u32(pool + 8)
    used = g.peek(flags, n)
    raw = b"".join(g.peek(ent + o, min(4096, n * 0x220 - o)) for o in range(0, n * 0x220, 4096))
    return [ent + i * 0x220 for i in range(n)
            if not used[i] & 0x80 and struct.unpack_from("<h", raw, i * 0x220 + 0x56)[0] == model]


def scenario_ferris(s):
    """The player put in the lowest ferris wheel carriage, then 10 s of the ride: how long he stays where he stood in it
    (within 1 m) and how far he drifts. The wheel (FUN_00175620) gave its carriages a move speed of (new position - old)
    / max(step, 1.0): above 50 fps too small, and at 144 fps he slid out of the carriage within 2 s (issue 10)."""
    g, S = s.g, rig.Scm
    leave_vehicle(s)
    x, y = FERRIS_SPOT
    s.place(x, y)
    carriages = pool_objects(g, FERRIS_CARRIAGE)
    if not carriages:
        raise RuntimeError("no ferris wheel carriages")
    carriage = min(carriages, key=lambda a: g.peekf(a + 0x38, 1)[0])
    cx, cy, cz = g.peekf(carriage + 0x30, 3)
    g.scm(S.op(S.SET_CHAR_COORDINATES, S.PLAYER, S.f(cx), S.f(cy), S.f(cz + 0.5)))
    s.wait_sim(0.5)  # lands on its floor
    ped = g.player()[0]
    path = os.path.join(ROOT, "build", "audit", "trace_%d_ferris.txt" % s.step)
    g.cmd("trace %s 0x%x 3 0x%x 3" % (path, ped + 0x30, carriage + 0x30))
    try:
        s.wait_sim(10.0)
    finally:
        g.cmd("trace off")
    rows = [[float(v) for v in line.split()] for line in open(path)]
    t0 = rows[0][1]
    offsets = [((r[1] - t0) / 1000.0, [r[3 + k] - r[6 + k] for k in range(3)]) for r in rows]
    start = offsets[0][1]
    drift = [(t, math.dist(o, start)) for t, o in offsets]
    riding = next((t for t, d in drift if d > 1.0), drift[-1][0])
    leave = S.op(S.SET_CHAR_COORDINATES, S.PLAYER, S.f(x), S.f(y), S.f(-100.0))  # off the wheel for what follows
    g.scm(leave)
    return {"riding_s": riding, "drift": "%.2f m" % max(d for _, d in drift)}


CAM_BETA = 0x6F44D0 + 0x70 + 0x7C  # TheCamera's active CCam: beta, the heading from the target to the camera (12b)


def wrapped(a):
    """An angle in -pi .. pi."""
    return (a + math.pi) % (2.0 * math.pi) - math.pi


def camera_catch_up(samples, released):
    """How fast a follow camera gets back behind its target after a turn. samples: (sim time, forward x, forward y,
    camera beta); released: when the turn key was let go. From the moment the target holds its heading (within 0.5
    degrees over the next 0.2 s), the camera's lag behind it (beta against the backwards heading) and the time it takes
    to halve. None when the target's heading changes by 3 degrees before that (it hit something)."""
    rows = [(t, math.atan2(-fy, -fx), wrapped(beta - math.atan2(-fy, -fx))) for t, fx, fy, beta in samples]

    def at(t):
        return min(rows, key=lambda r: abs(r[0] - t))
    start = next((r for r in rows if r[0] >= released and abs(wrapped(at(r[0] + 0.2)[1] - r[1])) < math.radians(0.5)),
                 None)
    if start is None:
        return None, None
    half = next((r for r in rows if r[0] > start[0] and abs(r[2]) <= abs(start[2]) / 2.0), None)
    if half is None or any(abs(wrapped(r[1] - start[1])) > math.radians(3.0) for r in rows if start[0] <= r[0] <= half[0]):
        return None, math.degrees(abs(start[2]))
    return half[0] - start[0], math.degrees(abs(start[2]))


def scenario_carcam(s):
    """The car camera (FUN_0029faf0) catching up behind the car after a turn: 1.2 s of full throttle, a left turn for
    0.5 s, then coasting. How long the camera's lag behind the car takes to halve once the car runs straight (and the
    lag then). The camera's automatic turn was a speed of (heading change) / max(step, 1.0): above 50 fps step times too
    slow, ~0.35 s at 1/30 against ~1 s at 1/144."""
    def act(s):
        s.key("Shift", True)
        s.wait_sim(1.2)
        s.key("A", True)
        s.wait_sim(0.5)
        s.key("A", False)
        s.key("Shift", False)
        s.wait_sim(3.0)
    rows = vehicle_run(s, "carcam", 211, act, extra="0x%x 1" % CAM_BETA)
    half, lag = camera_catch_up([(r[0], r[1 + 4], r[1 + 5], r[1 + 24]) for r in rows], 1.7)
    return {"half_s": half, "lag": "%.0f deg" % lag if lag is not None else "-"}


def scenario_footcam(s):
    """The on-foot camera (FUN_0029dab0) swinging after the player while he runs to its right (D, 2.5 s): its turn rate
    over the last 2 s, degrees a second. (The controls follow the camera, so after a turn the player lines up with the
    camera rather than the camera with him; running sideways is where its own turn shows.) Same max(step, 1.0) as the
    car camera's."""
    x, y, h = FOOT_SPOT
    leave_vehicle(s)
    s.place(x, y, heading=h)
    s.wait_sim(1.5)  # the camera settles behind him
    path = os.path.join(ROOT, "build", "audit", "trace_%d_footcam.txt" % s.step)
    s.g.cmd("trace %s 0x%x 1" % (path, CAM_BETA))
    s.key("D", True)
    try:
        s.wait_sim(2.5)
    finally:
        s.key("D", False)
        s.g.cmd("trace off")
    rows = [[float(v) for v in line.split()] for line in open(path)]
    t0 = rows[0][1]
    rows = [((r[1] - t0) / 1000.0, r[3]) for r in rows if r[1] - t0 >= 500]
    turn = sum(wrapped(b[1] - a[1]) for a, b in zip(rows, rows[1:]))
    return {"turn_deg_s": abs(math.degrees(turn)) / max(rows[-1][0] - rows[0][0], 1e-6)}


def scenario_heat(s):
    """PHI_A2 "Boomshine Blowout": the heat's floor (+0.015 a frame in the script) and the heat at a rate of +1 a frame,
    per simulated second (0.45 / 30 at the PS2's 30 fps). The mission is walked to its forklift state: into Phil's
    truck, then to the warehouse's back (the state-2 marker); in state 4 the heat routine runs every frame and the
    rate $G15AC is only recomputed once the player is on the forklift, so it can be set (game-internals.md 11f).
    Leaves the mission running: keep it the last scenario."""
    g, S = s.g, rig.Scm
    space = g.u32(0x48741C)

    def gv(n):
        return space + 4 * n

    state = gv(0x159E)
    g.mission(47)
    for i in range(240):  # the opening cutscene (skipped), then state 0's "get in the truck" loop at mission +0x3BEE..
        base = g.u32(0x4CD3D8)
        if any(0x3BEE <= sc[2] - base <= 0x3C40 for sc in g.scripts()):
            break
        if i % 6 == 3:
            g.cmd("press Shift 150")
        time.sleep(0.5)
    else:
        raise RuntimeError("PHI_A2 never reached its truck loop")
    g.scm(S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, bytes([0xCD + 0x14, 0xEE])))  # $G14EE: Phil's truck
    for i in range(120):  # state 1 (drive Phil to the warehouse) skipped: to state 2 once the mission is in it
        if g.u32(state) == 2:
            break
        if i == 20 and g.u32(state) == 1:
            g.cmd("poke 0x%x 02000000" % state)
        time.sleep(0.25)
    g.scm(S.op(S.SET_CHAR_COORDINATES, S.PLAYER, S.f(-1235.45), S.f(-278.9), S.f(11.6)))  # the warehouse's back
    for i in range(120):
        if g.u32(state) == 4:
            break
        time.sleep(0.25)
    else:
        raise RuntimeError("PHI_A2 never reached the forklift state (state %d)" % g.u32(state))
    s.wait_sim(1.0)

    def sample():
        return s.clock(), g.peekf(gv(0x15AB), 1)[0], g.peekf(gv(0x15AA), 1)[0]

    a = sample()
    s.wait_sim(5.0)
    b = sample()
    g.cmd("pokef 0x%x 1.0" % gv(0x15AC))
    s.wait_sim(0.2)
    c = sample()
    s.wait_sim(1.5)  # +45 at 30 a second: the heat stays under 100 (where the boomshine blows)
    d = sample()
    return {"floor_per_s": (b[1] - a[1]) / (b[0] - a[0]), "heat_per_s": (d[2] - c[2]) / (d[0] - c[0])}


def scenario_clock(s):
    c0, f0 = s.g.u32(CLOCK_MS), s.g.u32(FRAME_COUNTER)
    s.wait_sim(5.0)
    c1, f1 = s.g.u32(CLOCK_MS), s.g.u32(FRAME_COUNTER)
    frames = max(f1 - f0, 1)
    return {"ms_per_frame": (c1 - c0) / frames, "expected_ms": 1000.0 / s.step, "frames_per_sim_s": frames / ((c1 - c0) / 1000.0)}


SCENARIOS = {
    "clock": scenario_clock,
    "fall": scenario_fall,
    "run": lambda s: scenario_foot(s, ["W"]),
    "sprint": lambda s: scenario_foot(s, ["W", "Shift"]),
    "jump": scenario_jump,
    "car": scenario_car,
    "coast": scenario_coast,
    "runover": scenario_runover,
    "bikebody": scenario_bikebody,
    "bikecoast": scenario_bikecoast,
    "bikelean": scenario_bikelean,
    "brake": scenario_brake,
    "bikebrake": scenario_bikebrake,
    "rotor": scenario_rotor,
    "carwater": scenario_carwater,
    "bikewater": scenario_bikewater,
    "ferris": scenario_ferris,
    "carcam": scenario_carcam,
    "footcam": scenario_footcam,
    "heat": scenario_heat,  # starts a mission: last
}


def check_sites(port):
    """--sites: every per-frame meter instruction src/game/vcs_script_timing.cpp scales (its kSites) is where the table
    says, as the game decodes it: a game per listed mission with VCS_SCRIPT_TIMESTEP_CHECK=1, the mission launched, the
    log's "check: mission N +0x.. matches" lines counted against the table."""
    src = open(os.path.join(ROOT, "src", "game", "vcs_script_timing.cpp"), encoding="utf-8").read()
    table = {}
    for m, off in re.findall(r"^\s*\{(\d+), 0x([0-9A-Fa-f]+), 0x[0-9A-Fa-f]{4}, ", src, re.M):
        table.setdefault(int(m), set()).add(int(off, 16))
    bad = 0
    for mission in sorted(table):
        g = rig.Game.start(save=rig.default_save(), name="sites_%d" % mission,
                           port=port, data_dir=os.path.join(ROOT, "build", "audit", "sites_data"),
                           env={"VCS_SCRIPT_TIMESTEP_CHECK": "1"})
        try:
            g.wait_ingame()
            g.prime()
            g.mission(mission)
            found = set()
            for _ in range(40):
                time.sleep(0.5)
                log = open(g.log_path, encoding="utf-8", errors="replace").read()
                found = {int(o, 16) for o in re.findall(r"check: mission %d \+0x([0-9A-F]+) matches" % mission, log)}
                if found >= table[mission]:
                    break
        finally:
            g.quit()
        missing = table[mission] - found
        bad += len(missing)
        print("[sites] mission %d: %d of %d found%s" % (mission, len(found & table[mission]), len(table[mission]),
              ", missing " + " ".join("+0x%X" % o for o in sorted(missing)) if missing else ""), flush=True)
    print("[sites] %s" % ("all found" if not bad else "%d missing" % bad))
    return bad == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--steps", default="15,30,60,100")
    ap.add_argument("--only", default="", help="comma-separated scenarios (default: all)")
    ap.add_argument("--tolerance", type=float, default=0.05)
    ap.add_argument("--sites", action="store_true", help="only check the per-frame meter sites (check_sites)")
    a = ap.parse_args()
    if a.sites:
        sys.exit(0 if check_sites(47670) else 1)
    out = a.out or os.path.join(ROOT, "build", "audit", datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    os.makedirs(out, exist_ok=True)
    steps = [int(x) for x in a.steps.split(",") if x]
    names = [n for n in (a.only.split(",") if a.only else SCENARIOS) if n]
    results = {}
    for step in steps:
        print("[audit] step 1/%d s" % step, flush=True)
        s = Session(step, "audit_%d" % step, 47670)
        try:
            results[step] = {}
            for name in names:
                try:
                    results[step][name] = SCENARIOS[name](s)
                except Exception as e:  # one scenario failing doesn't end the run
                    results[step][name] = {"error": str(e)}
                print("  %-7s %s" % (name, results[step][name]), flush=True)
        finally:
            s.quit()
    with open(os.path.join(out, "audit.json"), "w", encoding="utf-8") as f:
        json.dump({"steps": steps, "results": results}, f, indent=1)

    ref = results.get(30) or results[steps[0]]
    print("\n%-8s %-22s" % ("scenario", "metric") + "".join("%12s" % ("1/%d" % st) for st in steps))
    flagged = 0
    for name in names:
        metrics = [k for k, v in ref.get(name, {}).items() if isinstance(v, (int, float))]
        for m in metrics:
            row = "%-8s %-22s" % (name, m)
            for st in steps:
                v = results[st].get(name, {}).get(m)
                r = ref[name][m]
                if not isinstance(v, (int, float)):
                    row += "%12s" % "-"
                    continue
                off = (v - r) / abs(r) if r else 0.0
                bad = abs(off) > a.tolerance and m not in ("expected_ms", "frames_per_sim_s")
                flagged += bad
                row += "%12s" % ("%.3g%s" % (v, " !" if bad else ""))
            print(row)
    print("\n%d values differ from 1/30 by more than %.0f%% (marked !); results in %s" % (flagged, a.tolerance * 100, out))


if __name__ == "__main__":
    main()

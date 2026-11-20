#!/usr/bin/env python3
"""Trainer check (docs/TRAINER.md): every trainer feature through the control channel, each verified in the game's
memory, with screenshots.

    python tools/trainer_check.py [OUT] [--vehicles all|N] [--freecam] [--port 47672]

Loads the Fort Baxter rig save (a copy, hidden), then: the name lists; god mode (health refilled, an explosion at
the player's feet survived); wanted level set / cleared / never wanted; money; weapons, loadouts, unlimited ammo
and no reload; spawning vehicles (every one with --vehicles all, default a sample of 12) and getting in; repair,
flip, boost, paint and vehicle invincibility; teleports to every landmark on foot and one in a vehicle; saved
position; time, freeze time, weather, game speed; the free camera (flying, the mouse, far away, Hide HUD, the player
teleported below it, the camera given back); the mission lists and a side job launched; that no trainer script is left in the game's script list while
the game is paused (none can end up in a save); and the menu, driven with keys. Results: OUT/summary.txt; screenshots OUT/*.png.
Default OUT: build/trainer/<date-time>.
"""
import argparse
import math
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = rig.Scm
PED_HEALTH, PED_VEHICLE, PED_STATE, PED_WEAPONS = 0x4E4, 0x480, 0x8C4, 0x578
CAR_HEALTH = 0x278
CLOCK = 0x4CD148
CAMERA = 0x6F44D0  # TheCamera: +0x30 its position (game-internals.md 12b)
CAMERA_TARGET = CAMERA + 0xE0  # what the active camera follows (game-internals.md 12)
# open streets far apart (the landmarks, docs/TRAINER.md), for the vehicle batches
SPOTS = [(75.0, 175.0), (-1446.2, -820.2), (571.0, -640.8), (-834.4, 1403.7), (-978.2, -570.1), (351.4, 339.0),
         (-958.2, 85.6), (83.8, -1284.7)]


def L(n):
    return bytes([0x0D + n])


class Check:
    def __init__(self, out, port):
        self.out = out
        self.port = port
        self.results = []
        self.g = None

    def ok(self, name, cond, detail=""):
        self.results.append((name, bool(cond), detail))
        print("%-4s %s %s" % ("ok" if cond else "FAIL", name, detail), flush=True)
        return cond

    def t(self, line):
        return self.g.cmd("trainer " + line, timeout=90)

    def shot(self, name):
        self.g.cmd("screenshot " + os.path.join(self.out, name + ".png").replace("\\", "/"))

    def outputs(self, code, n):
        """Run `code` (writing locals 0..n-1) and read them back as ints and floats."""
        return self.g.scm_outputs(code, n)

    def ped(self):
        return self.g.u32(0x4E4910)

    def wanted(self):
        return self.outputs(S.op(0x0110, S.PLAYER, L(0)), 1)[0][0]

    def money(self):
        return self.outputs(S.op(0x0096, S.PLAYER, L(0)), 1)[0][0]

    def car(self):
        return self.g.u32(self.ped() + PED_VEHICLE)

    def driving(self):
        return self.car() != 0 and self.g.u32(self.ped() + PED_STATE) == 0x37

    def slot(self, weapon_type):
        data = self.g.peek(self.ped() + PED_WEAPONS, 10 * 0x1C)
        for s in range(10):
            t, _, clip, total = struct.unpack_from("<4I", data, s * 0x1C)
            if t == weapon_type:
                return s, clip, total
        return None

    def pose(self):
        """The free camera (trainer freecam): active, x, y, z, yaw, pitch, fov, speed, hud (CHud::Draw calls)."""
        return {k: float(v) for k, v in (kv.split("=") for kv in self.t("freecam").split() if "=" in kv)}

    def free_camera(self):
        """The free camera: flies at its speed in real time while the player stands, turns with the mouse, moves the
        game's own camera (TheCamera +0x30), puts the player below it, gives the camera back; Hide HUD."""
        g = self.g
        g.teleport(75.0, 175.0, -100.0)
        time.sleep(2.0)
        self.t("camspeed 20")
        self.t("toggle freecam 1")
        time.sleep(1.0)
        a = self.pose()
        p0 = g.player()[1]
        g.cmd("key W down")
        time.sleep(2.5)
        g.cmd("key W up")
        time.sleep(0.3)
        b = self.pose()
        moved = math.dist((a["x"], a["y"], a["z"]), (b["x"], b["y"], b["z"]))
        still = math.dist(p0, g.player()[1])
        self.ok("free camera flies", a["active"] == 1 and 40 < moved < 60 and still < 0.5,
                "%.1f m in 2.5 s at 20 m/s; the player moved %.2f m" % (moved, still))
        cam = g.peekf(CAMERA + 0x30, 3)
        self.ok("free camera is the game's camera", math.dist(cam, (b["x"], b["y"], b["z"])) < 0.1, "%s" % cam)
        g.cmd("mouse 400 0")  # 400 counts: a radian at 100 % sensitivity, to the right
        time.sleep(0.5)
        c = self.pose()
        turn = math.remainder(c["yaw"] - b["yaw"], 2 * math.pi)
        self.ok("free camera turns with the mouse", abs(turn + 1.0) < 0.01, "%.3f rad" % turn)
        self.shot("freecam")
        # up and far away: the world streams in around the camera
        self.t("camspeed 80")
        g.cmd("key E down")
        time.sleep(1.25)
        g.cmd("key E up")
        g.cmd("key shift down")
        g.cmd("key W down")
        time.sleep(2.0)
        g.cmd("key W up")
        g.cmd("key shift up")
        g.cmd("mouse 0 250")  # then look down at the city
        time.sleep(5.0)
        far = self.pose()
        self.shot("freecam_far")
        self.ok("free camera far away", math.dist((far["x"], far["y"]), p0[:2]) > 400 and far["z"] > 50,
                "(%.0f, %.0f, %.0f)" % (far["x"], far["y"], far["z"]))
        # Hide HUD: CHud::Draw skipped, then back
        self.t("toggle hidehud 1")
        h0 = self.pose()["hud"]
        time.sleep(1.0)
        h1 = self.pose()["hud"]
        self.shot("freecam_nohud")
        self.t("toggle hidehud 0")
        time.sleep(1.0)
        h2 = self.pose()["hud"]
        self.ok("hide HUD", h1 == h0 and h2 > h1, "%d -> %d hidden, %d shown" % (h0, h1, h2))
        # the player to the ground below it; the camera back behind him
        self.t("camtp")
        time.sleep(2.0)
        pos = g.player()[1]
        self.ok("teleport below the camera", math.dist(pos[:2], (far["x"], far["y"])) < 3 and "freecam=0" in self.t("state"),
                "player %s" % (pos,))
        cam = g.peekf(CAMERA + 0x30, 3)
        self.ok("camera back on the player", g.u32(CAMERA_TARGET) == self.ped() and math.dist(cam, pos) < 15,
                "camera %s" % cam)
        self.t("camspeed 20")

    def cutscene_under_free_camera(self):
        """Soldier's opening cutscene played out under the free camera (turned on once it runs: the launch turns it off).
        A cutscene ends only when its camera has played its path: the free camera once replaced the camera's update and
        the mission sat on "Loading..." for good (2026-10-30). Normally on foot in the mission ~190 s after the launch."""
        g = self.g
        story = [e for e in self.t("list story").split(";") if ":" in e]
        soldier = int(story[0].split(":")[0])
        self.t("mission %d" % soldier)
        time.sleep(45.0)  # into the cutscene proper (camera mode 17)
        self.t("toggle freecam 1")
        start, mode = time.time(), 0
        while time.time() - start < 240:
            time.sleep(5.0)
            mode = g.u32(CAMERA + 0x70) & 0xFFFF
            if mode == 4 and g.u32(CAMERA + 0x888) == 1:  # the follow camera on foot: the mission has begun
                break
        self.shot("freecam_after_cutscene")
        self.t("toggle freecam 0")
        self.ok("cutscene ends under the free camera", mode == 4, "camera mode %d, %.0f s after turning it on" % (mode, time.time() - start))

    def run(self, vehicles, only_freecam=False):
        g = self.g = rig.Game.start(save=rig.default_save(), name="trainer_check",
                                    port=self.port, data_dir=os.path.join(ROOT, "build", "trainer", "data"),
                                    env={"VCS_FPS": "30", "VCS_ASPECT": "43"})
        try:
            g.wait_ingame()
            g.prime()
            if only_freecam:
                self.free_camera()
                self.cutscene_under_free_camera()
                return self.summary()
            g.teleport(75.0, 175.0, -100.0)
            time.sleep(2)
            state = self.t("state")
            self.ok("ready", "ready=1" in state, state)

            lists = {}
            for what in ("vehicles", "weapons", "loadouts", "landmarks", "weathers"):
                items = [e.split(":", 1) for e in self.t("list " + what).split(";") if ":" in e]
                lists[what] = [(int(i), n) for i, n in items]
            self.ok("vehicle names", len(lists["vehicles"]) >= 100, "%d, e.g. %s" % (len(lists["vehicles"]), lists["vehicles"][:4]))
            self.ok("landmark names", len(lists["landmarks"]) == 13, str([n for _, n in lists["landmarks"]]))
            self.ok("weapon names", len(lists["weapons"]) >= 30, str([n for _, n in lists["weapons"]][:8]))

            # god mode
            self.t("toggle god 1")
            g.scm(S.op(0x014B, S.PLAYER, S.i(20)))
            time.sleep(1.0)
            self.ok("god mode refills health", g.peekf(self.ped() + PED_HEALTH, 1)[0] >= 100.0)
            x, y, z = g.player()[1]
            g.scm(S.op(0x013C, S.f(x + 1.0), S.f(y), S.f(z), S.i(0)))  # a grenade's explosion at his feet
            time.sleep(2.0)
            self.shot("god_explosion")
            st = g.u32(self.ped() + PED_STATE)
            self.ok("god mode survives an explosion", st not in (0x39, 0x3A) and g.peekf(self.ped() + PED_HEALTH, 1)[0] >= 100.0,
                    "state %x" % st)

            # wanted level
            self.t("setwanted 3")
            time.sleep(0.5)
            self.ok("wanted level set", self.wanted() == 3, str(self.wanted()))
            self.t("setwanted 0")
            self.ok("wanted level cleared", self.wanted() == 0)
            self.t("toggle wanted 1")
            g.scm(S.op(0x0099, S.PLAYER, S.i(4)))
            time.sleep(1.0)
            self.ok("never wanted", self.wanted() == 0, str(self.wanted()))
            self.t("toggle wanted 0")

            # money
            m0 = self.money()
            self.t("money 25000")
            self.ok("money", self.money() == m0 + 25000, "%d -> %d" % (m0, self.money()))

            # weapons
            self.t("noweapons")
            self.t("weapon 28")
            s = self.slot(28)
            self.ok("give weapon", s is not None, str(s))
            time.sleep(0.5)
            self.shot("weapon_rifle")
            self.t("loadout 1")
            given = [t for t in (9, 12, 20, 22, 29, 30, 32) if self.slot(t)]
            self.ok("loadout", len(given) >= 6, str(given))  # slots shared within a loadout keep the last one
            self.t("toggle ammo 1")
            time.sleep(1.0)
            s = self.slot(29)
            self.ok("unlimited ammo", s and s[2] >= 9000, str(s))
            self.t("toggle reload 1")
            g.scm(S.op(0x010B, S.PLAYER, S.i(29)))
            time.sleep(1.0)
            full = self.slot(29)[1]
            g.scm(S.op(0x03FA, S.i(3)))  # SET_PLAYER_CURRENT_WEAPON_AMMO_IN_CLIP: as if 27 rounds were fired
            time.sleep(1.0)
            self.ok("no reload refills the clip", self.slot(29)[1] == full and full > 3, "clip %d (full %d)" % (self.slot(29)[1], full))
            self.t("toggle reload 0")
            self.t("toggle ammo 0")

            # vehicles
            self.t("toggle warp 1")
            ids = [i for i, _ in lists["vehicles"]]
            if vehicles != "all":
                step = max(1, len(ids) // int(vehicles))
                ids = ids[::step][:int(vehicles)]
            names = dict(lists["vehicles"])
            bad = []
            for n, model in enumerate(ids):
                if n % 8 == 0:  # a new spot for every batch: the cars left behind go away, as they would in play
                    spot = SPOTS[(n // 8) % len(SPOTS)]
                    g.teleport(spot[0], spot[1], -100.0)
                    time.sleep(1.0)
                print("  spawn %d %s" % (model, names[model]), flush=True)
                try:
                    self.t("spawn %d" % model)
                except RuntimeError as e:
                    bad.append((model, names[model], str(e)))
                    continue
                time.sleep(0.6)
                car = self.car()
                got = car and g.peek(car + 0x56, 2)
                got = struct.unpack("<H", got)[0] if car else -1
                if not self.driving() or got != model:
                    bad.append((model, names[model], "driving=%s model=%d" % (self.driving(), got)))
                elif g.u32(CAMERA_TARGET) != car:  # from car to car the camera stayed on the old one (2026-10-28)
                    bad.append((model, names[model], "camera follows %x, not %x" % (g.u32(CAMERA_TARGET), car)))
                if n < 3 or vehicles == "all" and n % 15 == 0:
                    self.shot("spawn_%d" % model)
            self.ok("spawn vehicles", not bad, "%d spawned; problems: %s" % (len(ids), bad))

            # vehicle actions, in a Sentinel
            g.teleport(75.0, 175.0, -100.0)
            time.sleep(1.0)
            self.t("spawn 211")
            time.sleep(0.5)
            car = self.car()
            g.scm(S.op(0x024B, S.PLAYER, L(0)) + S.op(0x014C, L(0), S.i(300)))
            self.t("repair")
            self.ok("repair", g.peekf(car + CAR_HEALTH, 1)[0] >= 1000.0, str(g.peekf(car + CAR_HEALTH, 1)))
            g.scm(S.op(0x024B, S.PLAYER, L(0)) + S.op(0x03B9, L(0), S.f(180.0), S.f(0.0), S.f(0.0)))
            time.sleep(1.5)
            self.shot("upside_down")
            self.t("flip")
            time.sleep(1.5)
            up = g.peekf(car + 0x28, 1)[0]
            self.ok("flip", up > 0.9, "up.z %.3f" % up)
            self.shot("flipped")
            self.t("paint")
            # boost on open ground (framerate_audit.CAR_SPOT, facing north): in the garden above the car met the hedge,
            # or a car the spawns left there, within the 0.2 s and read 0.2 / 6.6 / 26.6 in three runs (2026-10-28)
            g.scm(S.op(S.LOAD_SCENE, S.f(-1425.0), S.f(-640.0), S.f(15.0)) + S.op(0x024B, S.PLAYER, L(0)) +
                  S.op(S.SET_CAR_COORDINATES, L(0), S.f(-1425.0), S.f(-640.0), S.f(-100.0)) +
                  S.op(0x00D1, L(0), S.f(0.0)))  # SET_CAR_HEADING
            time.sleep(1.5)
            self.t("boost")
            time.sleep(0.2)
            speed = self.outputs(S.op(0x024B, S.PLAYER, L(0)) + S.op(0x01CA, L(0), L(1)), 2)[1][1]
            self.ok("boost", speed > 20.0, "speed %.1f" % speed)
            time.sleep(2.0)
            self.t("toggle vehicle 1")
            g.scm(S.op(0x024B, S.PLAYER, L(0)) + S.op(0x014C, L(0), S.i(200)))
            time.sleep(1.0)
            self.ok("vehicle invincibility", g.peekf(car + CAR_HEALTH, 1)[0] >= 1000.0, str(g.peekf(car + CAR_HEALTH, 1)))
            self.t("toggle vehicle 0")

            # teleports: in the vehicle, then on foot to every landmark
            target = dict(lists["landmarks"])
            first = lists["landmarks"][0][0]
            before = g.player()[1]
            self.t("teleport %d" % first)
            time.sleep(2.0)
            after = g.player()[1]
            self.ok("teleport in a vehicle", self.driving() and abs(after[0] - before[0]) + abs(after[1] - before[1]) > 100,
                    "%s -> %s" % (before, after))
            g.scm(S.op(0x0216, S.PLAYER, S.f(g.player()[1][0] + 3.0), S.f(g.player()[1][1]), S.f(-100.0)))
            time.sleep(1.0)
            far = []
            for idx, name in lists["landmarks"]:
                self.t("teleport %d" % idx)
                time.sleep(2.5)
                pos = g.player()[1]
                self.shot("landmark_%02d_%s" % (idx, re.sub(r"\W+", "_", name)))
                far.append((name, round(pos[0]), round(pos[1]), round(pos[2])))
            self.ok("teleport to landmarks", all(-2200 < p[1] < 1600 for p in far), str(far))
            self.t("savepos")
            saved = g.player()[1]
            g.teleport(75.0, 175.0, -100.0)
            time.sleep(1.0)
            self.t("returnpos")
            time.sleep(1.0)
            back = g.player()[1]
            self.ok("saved position", abs(back[0] - saved[0]) < 2 and abs(back[1] - saved[1]) < 2, "%s -> %s" % (saved, back))

            # world
            self.t("time 22")
            self.ok("time", g.peek(CLOCK, 1)[0] == 22, str(g.peek(CLOCK, 2)))
            self.t("toggle freeze 1")
            time.sleep(4.0)
            clock = g.peek(CLOCK, 2)
            self.ok("freeze time", clock[0] == 22 and clock[1] == 0, str(list(clock)))
            self.t("toggle freeze 0")
            self.t("weather 2")
            time.sleep(2.0)
            self.shot("weather_rain")
            self.t("weather -1")
            self.t("speed 50")
            self.t("speed 100")
            self.ok("world commands", True)

            self.free_camera()

            # nothing of the trainer's left in the script list while paused
            self.t("toggle god 1")
            self.t("toggle wanted 1")
            g.cmd("press Escape 150")
            time.sleep(2.0)
            left = [s for s in g.scripts() if s[1] == "trainer"]
            self.shot("paused")
            self.ok("no trainer script while paused", not left, str(left))
            g.cmd("press Escape 150")
            time.sleep(1.5)
            self.t("heal")
            self.ok("actions after the pause", True)
            # the menu, driven by keys: opening it and flipping a row; the game behind it keeps running and gets
            # none of the keys (Escape, the trainer's back, is the game's Start: it would open the pause map)
            self.t("toggle god 0")
            g.cmd("press f5 60")
            time.sleep(0.5)
            self.shot("menu_main")
            for k in ("return", "return"):  # Player, then God mode
                g.cmd("press %s 60" % k)
                time.sleep(0.3)
            self.shot("menu_player")
            t0 = g.u32(0x4CD104)
            time.sleep(1.0)
            running = g.u32(0x4CD104) != t0
            self.ok("menu toggles a row", "god=1" in self.t("state"))
            self.ok("menu keeps its keys from the game", running)
            for k in ("backspace", "backspace"):
                g.cmd("press %s 60" % k)
                time.sleep(0.3)
            notices = self.t("notices")
            self.ok("notices", "Health and armour" in notices, notices[-200:])

            # missions (last: a running mission stays): the lists, a side job from on foot (the launcher puts him in
            # its vehicle first), and a second launch refused while it runs
            story = [e for e in self.t("list story").split(";") if ":" in e]
            jobs = dict((int(i), n) for i, n in (e.strip().split(":", 1) for e in self.t("list jobs").split(";") if ":" in e))
            self.ok("mission lists", len(story) >= 45 and len(jobs) == 5 and "Soldier" in story[0],
                    "%d story, first %s; jobs %s" % (len(story), story[:1], sorted(jobs.values())))
            g.teleport(75.0, 175.0, -100.0)
            time.sleep(2.0)
            taxi = next(i for i, n in jobs.items() if n == "Taxi")
            self.t("toggle freecam 1")  # a launch turns the free camera off (the mission's start is to be watched)
            said = self.t("mission %d" % taxi)
            time.sleep(3.0)
            model = struct.unpack("<H", g.peek(self.car() + 0x56, 2))[0] if self.driving() else -1
            self.shot("mission_taxi")
            state = self.t("state")
            self.ok("side job in its vehicle", "mission=1" in state and model == 226, "%s; model %d" % (said, model))
            self.ok("mission launch ends the free camera", "freecam=0" in state and g.u32(CAMERA_TARGET) == self.car(), said)
            said = self.t("mission %d" % int(story[1].split(":")[0]))
            self.ok("no second mission", "A mission is running" in said, said)
        finally:
            g.quit()
        return self.summary()

    def summary(self):
        fails = [r for r in self.results if not r[1]]
        with open(os.path.join(self.out, "summary.txt"), "w", encoding="utf-8") as f:
            for name, good, detail in self.results:
                f.write("%-4s %s %s\n" % ("ok" if good else "FAIL", name, detail))
            f.write("%d checks, %d failed\n" % (len(self.results), len(fails)))
        print("%d checks, %d failed -> %s" % (len(self.results), len(fails), self.out))
        return not fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--vehicles", default="12", help="all, or how many to sample")
    ap.add_argument("--port", type=int, default=47672)
    ap.add_argument("--freecam", action="store_true", help="only the free camera's checks")
    a = ap.parse_args()
    # absolute: the game writes the screenshots, from its own working directory
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "trainer", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    sys.exit(0 if Check(out, a.port).run(a.vehicles, a.freecam) else 1)


if __name__ == "__main__":
    main()

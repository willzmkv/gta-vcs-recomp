#!/usr/bin/env python3
"""Automation rig client (docs/NATIVE_PORT_ROADMAP.md milestone 1; src/debug/rig.cpp is the game side).

    python tools/rig.py start [--save DIR] [--seconds N] [--name NAME]   launch with the control channel
    python tools/rig.py peek ADDR [BYTES] | peekf ADDR [COUNT] | poke ADDR HEX | pokef ADDR F...
    python tools/rig.py findvec X Y Z TOL [LO HI] | findu32 VALUE [LO HI] | pause on|off | frames
    python tools/rig.py trace PATH ADDR COUNT [ADDR COUNT ...] | trace off   floats every frame to PATH (+ frame, ms, step)
    python tools/rig.py <any control command ...>                        sent as it is

`start` runs the game from a memory card folder (default: an empty card in build/rig/data). With --save DIR the
card is copied from DIR (a folder holding mc0/) so the game auto-loads its newest save - the quickest way into
gameplay. The user's own card is never used directly.

As a library: `import rig; g = rig.Game.start(save=...); g.cmd('peekf 0x6f4500 3'); g.quit()`.
"""
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamectl  # noqa: E402
import toolchain as tc  # noqa: E402

ROOT = tc.ROOT if hasattr(tc, "ROOT") else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


SAVES = os.path.join(ROOT, "build", "rig", "saves")


def default_save():
    """The card copy the test tools start from: build/rig/saves/fortbaxter (a save in the Fort Baxter barracks, the one
    their numbers were taken with), else build/rig/saves/user (a copy of the player's own card, made with copy_card -
    never the live card itself; the Linux laptop has only that one). Each holds mc0/. Neither there: the fortbaxter
    path, so the copy fails naming it."""
    for name in ("fortbaxter", "user"):
        path = os.path.join(SAVES, name)
        if os.path.isdir(os.path.join(path, "mc0")):
            if name != "fortbaxter":
                print("[rig] no fortbaxter card copy: starting from %s" % os.path.relpath(path, ROOT), flush=True)
            return path
    return os.path.join(SAVES, "fortbaxter")


def copy_card(src, dst):
    """Copy a card folder keeping file times (the card's directory order falls back to creation times)."""
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    if os.name == "nt":
        # robocopy keeps creation times; exit codes < 8 mean success
        r = subprocess.run(["robocopy", src, dst, "/E", "/COPY:DAT", "/DCOPY:T", "/NFL", "/NDL", "/NJH", "/NJS"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode >= 8:
            raise RuntimeError("robocopy failed (%d) copying %s" % (r.returncode, src))
    else:
        # Linux keeps no settable creation time; the card's own order file (.ps2x-order) keeps its directory order
        shutil.copytree(src, dst, copy_function=shutil.copy2)


class Scm:
    """Bytecode for the game's script VM (docs/game-internals.md section 11). Opcodes are the VCS numbers
    (Sanny Builder Library vcs.json, checked against the handler table); arguments carry a type byte."""
    PLAYER = b"\xcf\xa5"  # global 0x2A5: the player (VCS uses one handle for the player and the player's char)
    LOCAL0 = b"\x0d"      # local variable 0 (outputs)

    # opcodes
    TERMINATE = 0x0023
    CREATE_CHAR = 0x003E  # ped type, model, x, y, z (-100: the ground), handle out (handler 0x4120d8)
    SET_CHAR_COORDINATES = 0x0044
    CREATE_CAR = 0x0048
    SET_CAR_COORDINATES = 0x004E
    CAR_WANDER_RANDOMLY = 0x004B
    SET_CAR_CRUISE_SPEED = 0x0050
    SET_CAR_DRIVING_STYLE = 0x0051
    CREATE_CHAR_INSIDE_CAR = 0x00A4
    SET_CAR_PROOFS = 0x01B2
    WARP_CHAR_INTO_CAR_AS_PASSENGER = 0x0298
    SET_TIME_OF_DAY = 0x005B
    CLEAR_WANTED_LEVEL = 0x009B
    DO_FADE = 0x00C9
    SET_CHAR_HEADING = 0x00CF
    GIVE_WEAPON_TO_CHAR = 0x0106
    SET_PLAYER_CONTROL = 0x0107
    FORCE_WEATHER_NOW = 0x0109
    RELEASE_WEATHER = 0x010A
    SET_CHAR_HEALTH = 0x014B
    REQUEST_MODEL = 0x0160
    MARK_CHAR_AS_NO_LONGER_NEEDED = 0x0112
    MARK_CAR_AS_NO_LONGER_NEEDED = 0x0113
    MARK_MODEL_AS_NO_LONGER_NEEDED = 0x0162
    SET_CHAR_PROOFS = 0x01B1
    WARP_CHAR_INTO_CAR = 0x021C
    LOAD_ALL_MODELS_NOW = 0x0228
    LOAD_SCENE = 0x0253
    LAUNCH_MISSION = 0x0289
    SET_AREA_VISIBLE = 0x02EA  # CGame::currArea (0x489F7C): 0 = outdoors, interiors have their own numbers
    REQUEST_COLLISION = 0x0303

    @staticmethod
    def op(code, *args):
        return code.to_bytes(2, "little") + b"".join(args)

    @staticmethod
    def i(v):
        v = int(v)
        if v == 0:
            return b"\x01"
        if -128 <= v < 128:
            return b"\x07" + (v & 0xFF).to_bytes(1, "little")
        if -32768 <= v < 32768:
            return b"\x08" + (v & 0xFFFF).to_bytes(2, "little")
        return b"\x06" + (v & 0xFFFFFFFF).to_bytes(4, "little")

    @staticmethod
    def f(v):
        import struct
        return b"\x09" + struct.pack("<f", float(v))

    @classmethod
    def end(cls):
        return cls.op(cls.TERMINATE)


class Game:
    def __init__(self, proc, log_path, port=gamectl.DEFAULT_PORT):
        self.proc = proc
        self.log_path = log_path
        self.port = port

    @classmethod
    def start(cls, save=None, name="rig", seconds=3600, env=None, data_dir=None, port=None):
        """port: a control port of its own, so several games can run side by side (each with its own card)."""
        port = port or gamectl.DEFAULT_PORT
        data = data_dir or os.path.join(ROOT, "build", "rig", "data" if port == gamectl.DEFAULT_PORT else "data%d" % port)
        if save:
            os.makedirs(data, exist_ok=True)
            copy_card(os.path.join(save, "mc0"), os.path.join(data, "mc0"))
        else:
            if os.path.isdir(data):
                shutil.rmtree(data)
            os.makedirs(data)
        e = dict(os.environ)
        e["VCS_DATA_DIR"] = os.path.abspath(data)
        e["PS2X_CONTROL_PORT"] = str(port)
        e.update(env or {})
        # run.py logs to build/run/<name>.log; no inherited pipes, so a caller's shell does not wait for the game. Hidden
        # (test runs; VCS_TEST_VISIBLE=1 shows the window)
        proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "run.py"), str(int(seconds)), name, "--control",
                                 "--hidden"],
                                cwd=ROOT, env=e, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, start_new_session=not tc.IS_WINDOWS)
        gamectl.wait(port=port, timeout=90)
        return cls(proc, os.path.join(ROOT, "build", "run", name + ".log"), port)

    def alive(self):
        return self.proc is None or self.proc.poll() is None

    def wait_ingame(self, timeout=120.0, settle=4.0):
        """Wait until the player exists and game frames advance (a loaded save is playable), then `settle` s more."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if not self.alive():
                raise RuntimeError("the game exited while loading")
            try:
                f0 = int(self.cmd("frames"))
                self.player()
                time.sleep(1.0)
                if int(self.cmd("frames")) > f0 + 10 and not self.player()[2]:
                    time.sleep(settle)
                    return
            except Exception:
                time.sleep(1.0)
        raise RuntimeError("not in game after %.0f s" % timeout)

    def new_game(self, timeout=180.0, settle=4.0):
        """From an empty card (start(save=None)): the game boots into a new game - the intro cutscene, then the first
        mission at Fort Baxter. Cross (Shift) every few seconds skips the title flyover and the cutscene until the
        player is outdoors (area 0) and frames advance; then `settle` s more. A mission is running then (unlike
        wait_ingame's save): fine for measuring and profile training, which need no save (tools/bench.py)."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            if not self.alive():
                raise RuntimeError("the game exited while booting")
            try:
                # The title flyover is outdoors too, with advancing frames but no player. Requiring the player
                # prevents benchmarks and visual checks from returning while the title camera is still running.
                if self.area() == 0 and self.player()[0]:
                    f0 = int(self.cmd("frames"))
                    time.sleep(1.0)
                    if int(self.cmd("frames")) > f0 + 10:
                        time.sleep(settle)
                        return
            except Exception:
                pass
            if time.time() - t0 > 8.0:
                gamectl.send("press Shift", port=self.port)
            time.sleep(4.0)
        raise RuntimeError("not in game after %.0f s" % timeout)

    # Outside the Fort Baxter barracks door (where the game itself puts the player when leaving that interior).
    BARRACKS_DOOR = (-1648.87, -261.0)

    def prime(self, timeout=10.0):
        """Make later teleports land where asked. The Fort Baxter save loads in the barracks safehouse (interior area
        2). After the player has been teleported out of it, the safehouse script (`safehou`, IP 0x20E86) moves him back
        to the barracks door once, on the next teleport (game-internals.md 12). Trigger that here: two teleports away,
        then wait until the script has put him at the door. Returns True when it happened."""
        self.teleport(75.0, 175.0, -100.0)
        time.sleep(3.0)
        self.teleport(-600.0, 600.0, -100.0)
        t0 = time.time()
        while time.time() - t0 < timeout:
            x, y, _ = self.player()[1]
            if abs(x - self.BARRACKS_DOOR[0]) < 15 and abs(y - self.BARRACKS_DOOR[1]) < 15:
                time.sleep(1.0)
                return True
            time.sleep(0.25)
        return False

    def cmd(self, line, timeout=70.0):
        reply = gamectl.send(line, port=self.port, timeout=timeout)
        if not reply.startswith("ok"):
            raise RuntimeError("%s -> %s" % (line, reply))
        return reply[3:] if len(reply) > 3 else ""

    def peekf(self, addr, n=4):
        return [float(v) for v in self.cmd("peekf 0x%x %d" % (addr, n)).split()]

    def peek(self, addr, n):
        return bytes.fromhex(self.cmd("peek 0x%x %d" % (addr, n)))

    def u32(self, addr):
        return int.from_bytes(self.peek(addr, 4), "little")

    # ---- script commands (run through the game's own script VM) ----
    def scm(self, code):
        """Run bytecode as a new script; returns when it terminated (Scm.end() is appended)."""
        return self.cmd("scm " + (code + Scm.end()).hex(), timeout=90)

    def scm_outputs(self, code, n):
        """Run bytecode that writes locals 0..n-1 (outputs) and read them back: (ints, floats)."""
        import struct
        slot = int(self.scm(code).split()[0], 16)
        raw = self.peek(slot + 0x54, n * 4)
        return struct.unpack("<%di" % n, raw), struct.unpack("<%df" % n, raw)

    def player(self):
        """(ped address, (x, y, z), mission running)"""
        w = self.cmd("player").split()
        return int(w[0], 16), (float(w[1]), float(w[2]), float(w[3])), w[4] == "1"

    def scripts(self):
        out = []
        for item in self.cmd("scripts").split():
            sid, name, ip, mission = item.split(":")
            out.append((int(sid), name, int(ip, 16), mission == "1"))
        return out

    def area(self):
        """CGame::currArea: 0 outdoors, otherwise the interior the game is showing."""
        return self.u32(0x489F7C)

    def teleport(self, x, y, z, heading=None, area=0):
        """To (x, y, z) - z <= -100 finds the ground - in interior `area` (0 = outdoors). The area has to be set: a
        save loads inside an interior (the barracks are area 2), and outdoors drawn with an interior area is black."""
        S = Scm
        code = S.op(S.SET_AREA_VISIBLE, S.i(area)) + S.op(S.LOAD_SCENE, S.f(x), S.f(y), S.f(z)) + S.op(S.SET_CHAR_COORDINATES, S.PLAYER, S.f(x), S.f(y), S.f(z))
        if heading is not None:
            code += S.op(S.SET_CHAR_HEADING, S.PLAYER, S.f(heading))
        return self.scm(code)

    def set_time(self, hours, minutes=0):
        return self.scm(Scm.op(Scm.SET_TIME_OF_DAY, Scm.i(hours), Scm.i(minutes)))

    def weather(self, kind=None):
        """Force a weather type (0 sunny ...), or None to release it."""
        if kind is None:
            return self.scm(Scm.op(Scm.RELEASE_WEATHER))
        return self.scm(Scm.op(Scm.FORCE_WEATHER_NOW, Scm.i(kind)))

    def invincible(self, on=True):
        b = Scm.i(1 if on else 0)
        return self.scm(Scm.op(Scm.SET_CHAR_PROOFS, Scm.PLAYER, b, b, b, b, b) + Scm.op(Scm.CLEAR_WANTED_LEVEL, Scm.PLAYER))

    def health(self, value=100):
        return self.scm(Scm.op(Scm.SET_CHAR_HEALTH, Scm.PLAYER, Scm.i(value)))

    def spawn_car(self, model=211, ahead=4.0):
        """Create a car (vehicle model id, e.g. 211; MAIN.SCM uses 171..276) next to the player and put him in it."""
        S = Scm
        _, (x, y, z), _ = self.player()
        code = (S.op(S.REQUEST_MODEL, S.i(model)) + S.op(S.LOAD_ALL_MODELS_NOW) +
                S.op(S.CREATE_CAR, S.i(model), S.f(x + ahead), S.f(y), S.f(z + 1.0), S.LOCAL0) +
                S.op(S.SET_CAR_COORDINATES, S.LOCAL0, S.f(x + ahead), S.f(y), S.f(-100.0)) +
                S.op(S.WARP_CHAR_INTO_CAR, S.PLAYER, S.LOCAL0) + S.op(S.MARK_MODEL_AS_NO_LONGER_NEEDED, S.i(model)))
        return self.scm(code)

    def autodrive(self, model=211, speed=30.0, style=2, driver=(4, 55), release=False):
        """A car with an AI driver (ped type / model) wandering the streets, the player in the passenger seat:
        realistic driving at speed without steering it ourselves. release: hand car and driver back to the game
        (MARK_..._AS_NO_LONGER_NEEDED) so they become ordinary traffic and are removed once left behind - for long
        runs that create many cars."""
        S = Scm
        _, (x, y, z), _ = self.player()
        car, drv = S.LOCAL0, b"\x0e"  # locals 0 and 1
        code = (S.op(S.REQUEST_MODEL, S.i(model)) + S.op(S.REQUEST_MODEL, S.i(driver[1])) + S.op(S.LOAD_ALL_MODELS_NOW) +
                S.op(S.CREATE_CAR, S.i(model), S.f(x + 4.0), S.f(y), S.f(-100.0), car) +
                S.op(S.CREATE_CHAR_INSIDE_CAR, car, S.i(driver[0]), S.i(driver[1]), drv) +
                S.op(S.WARP_CHAR_INTO_CAR_AS_PASSENGER, S.PLAYER, car, S.i(0)) +
                S.op(S.SET_CAR_PROOFS, car, S.i(1), S.i(1), S.i(1), S.i(1), S.i(1)) +
                S.op(S.CAR_WANDER_RANDOMLY, car) + S.op(S.SET_CAR_CRUISE_SPEED, car, S.f(speed)) +
                S.op(S.SET_CAR_DRIVING_STYLE, car, S.i(style)))
        if release:
            code += S.op(S.MARK_CAR_AS_NO_LONGER_NEEDED, car) + S.op(S.MARK_CHAR_AS_NO_LONGER_NEEDED, drv)
        return self.scm(code)

    def mission(self, index):
        """LOAD_AND_LAUNCH_MISSION_INTERNAL: the game's own mission start (only when no mission is running)."""
        if self.player()[2]:
            raise RuntimeError("a mission is already running")
        return self.scm(Scm.op(Scm.LAUNCH_MISSION, Scm.i(index)))

    def wait_frames(self, n, timeout=60.0):
        start = int(self.cmd("frames"))
        t0 = time.time()
        while int(self.cmd("frames")) < start + n:
            if time.time() - t0 > timeout:
                raise RuntimeError("game frames stalled")
            time.sleep(0.05)

    def quit(self):
        try:
            gamectl.send("quit", port=self.port, timeout=10)
        except Exception:
            pass
        try:
            self.proc.wait(timeout=30)
        except Exception:
            # the game (a child of tools/run.py) too: killing only run.py left a hung game holding the control port
            if tc.IS_WINDOWS:
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.proc.pid)], capture_output=True)
            else:
                # run.py and the game share the session started for them (start): the whole group goes
                import signal
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except OSError:
                    self.proc.kill()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "start":
        import argparse
        ap = argparse.ArgumentParser()
        ap.add_argument("start")
        ap.add_argument("--save")
        ap.add_argument("--seconds", type=int, default=3600)
        ap.add_argument("--name", default="rig")
        a = ap.parse_args()
        g = Game.start(save=a.save, name=a.name, seconds=a.seconds)
        print("started; control channel up (log: %s)" % g.log_path)
        return 0
    print(gamectl.send(" ".join(sys.argv[1:])))
    return 0


if __name__ == "__main__":
    sys.exit(main())

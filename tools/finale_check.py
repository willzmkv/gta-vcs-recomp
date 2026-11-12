#!/usr/bin/env python3
"""Last Stand's end (mission 100, DIA_C5; game-internals.md 11g, docs/TRAINER.md): the final cutscene, then the credits.

    python tools/finale_check.py [OUT] [--story] [--full] [--port 47673]

Loads the Fort Baxter rig save (a copy, hidden) and starts Last Stand from the trainer - out of story order, as the
trainer allows. Skips the mission to its roof (state 11: the Mendez / Martinez scene), wins the fight (both shot
through the script VM), and then checks: the final cutscene plays, the mission ends, and the credits start - run by the
trainer (a script of its own, `credits`), because the story's own end-of-game check wants every chapter done. Without
that the screen stayed faded out with the game's "Loading..." over it for good (the user's report, 2026-10-29).

--story sets the story's chapter flags first: the game's own credits run, and the trainer must not start a second
copy. --full waits for the credits to end (several minutes): the player back below the Mendez building, the credits'
script gone and the picture faded in again.
Results: OUT/summary.txt, OUT/*.png. Default OUT: build/finale/<date-time>.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT_SPACE = 0x1DE0800           # *(0x48741C): globals are ScriptSpace + 4 * n
ACTIVE_SCRIPTS = 0x4CD3C4
MISSION_RUNNING = 0x4CD3E7
MISSION_BASE = 0x3CAE3             # *(0x4CD3D8): where missions are loaded (an IP)
DIA_C5_FILE = 0x1C291E             # Last Stand's start in MAIN.SCM (IP); its offsets below are from the disassembly
ROOF_STATE, FIGHT_STATE = 11, 12   # local 0: the mission's state; 11 the roof scene, 12 the fight with both
DISPATCH = 0x1C6202                # IS 0L > 0 -> the state dispatch
CUTSCENE = (0x22F5E, 0x22F97)      # main script: wait for the cutscene to load, play it, clear it
CREDITS = (0x24DC3, 0x25105)       # main script: the credits subroutine and the GOSUBs it uses
ON_MISSION = 0x2AC                 # $G2AC: 1 on a mission and during the credits
STORY_FLAGS = ((0x82, 2), (0x296, 0), (0x16, 1), (0x19, 1), (0x1A, 1), (0x1B, 1), (0x1D, 1))
ROOF = (-925.14, 1329.07, 84.59)
BELOW = (-948.0, 1336.7)           # where the credits put the player at their end


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

    def shot(self, name):
        self.g.cmd("screenshot " + os.path.join(self.out, name + ".png").replace("\\", "/"))

    def luma(self, name):
        """Screenshot `name` and its mean brightness (0..255, every 8th pixel)."""
        self.shot(name)
        w, h, rows = tc.read_png(os.path.join(self.out, name + ".png"))
        vals = [sum(r[x * 3:x * 3 + 3]) / 3 for r in rows[::8] for x in range(0, w, 8)]
        return sum(vals) / len(vals)

    def scripts(self):
        """(slot, name, ip, local 0) of every active script."""
        out, s = [], self.g.u32(ACTIVE_SCRIPTS)
        while s and len(out) < 256:
            raw = self.g.peek(s, 0x218)
            out.append((s, raw[0x20F:0x217].split(b"\0")[0].decode("latin-1"), struct.unpack_from("<I", raw, 0x10)[0],
                        struct.unpack_from("<i", raw, 0x54)[0]))
            s = struct.unpack_from("<I", raw, 0)[0]
        return out

    def mission(self):
        return next((x for x in self.scripts() if x[1] == "dia_c5"), None)

    def global_(self, n):
        return struct.unpack("<i", self.g.peek(SCRIPT_SPACE + 4 * n, 4))[0]

    def wait(self, what, cond, seconds, every=1.0):
        t0 = time.time()
        while time.time() - t0 < seconds:
            v = cond()
            if v:
                return v
            time.sleep(every)
        print("     (gave up waiting for %s after %.0f s)" % (what, seconds), flush=True)
        return None

    def poke(self, addr, fmt, v):
        self.g.cmd("poke 0x%x %s" % (addr, struct.pack(fmt, v).hex()))

    def run(self, story, full):
        g = self.g = rig.Game.start(save=rig.default_save(), name="finale_check",
                                    port=self.port, data_dir=os.path.join(ROOT, "build", "finale", "data%d" % self.port))
        try:
            g.wait_ingame()
            g.invincible()
            index = next((int(e.split(":")[0]) for e in g.cmd("trainer list story", timeout=90).split(";")
                          if e.endswith(":Last Stand")), None)
            if not self.ok("Last Stand in the trainer's list", index is not None, "index %s" % index):
                return
            said = g.cmd("trainer mission %d" % index, timeout=90)
            self.ok("launched from the trainer", self.wait("the mission", self.mission, 30), said)
            time.sleep(25)  # its opening conversation
            g.teleport(*ROOF)
            time.sleep(2)
            # skip to the roof: the mission's script back at its state dispatch with state 11, inside its one GOSUB
            # (+0x204 the GOSUB / CALL depth, +0x1FC the local frame); between frames
            g.cmd("pause on")
            time.sleep(0.3)
            slot = self.mission()[0]
            self.poke(slot + 0x10, "<I", MISSION_BASE + DISPATCH - DIA_C5_FILE)
            self.poke(slot + 0x204, "<H", 1)
            self.poke(slot + 0x1FC, "<I", 0)
            self.poke(slot + 0x54, "<i", ROOF_STATE)
            self.poke(slot + 0x200, "<I", 0)
            if story:
                for n, v in STORY_FLAGS:
                    self.poke(SCRIPT_SPACE + 4 * n, "<i", v)
            g.cmd("pause off")
            fight = self.wait("the fight", lambda: (self.mission() or (0, 0, 0, 0))[3] == FIGHT_STATE, 120)
            self.shot("1_roof_scene_done")
            if not self.ok("roof scene played", fight):
                return
            g.invincible()
            g.health(200)
            g.scm(rig.Scm.op(0x01F1, b"\xe3\xca") + rig.Scm.op(0x01F1, b"\xe3\xcb"))  # EXPLODE_CHAR_HEAD $G16CA, $G16CB
            cut = self.wait("the final cutscene", lambda: any(CUTSCENE[0] <= x[2] <= CUTSCENE[1] for x in self.scripts()), 60)
            self.ok("final cutscene", cut)
            time.sleep(20)
            self.shot("2_cutscene")
            ended = self.wait("the mission's end", lambda: not g.peek(MISSION_RUNNING, 1)[0], 240)
            self.ok("mission ended", ended)
            in_credits = lambda: [x for x in self.scripts() if CREDITS[0] <= x[2] < CREDITS[1]]
            credits = self.wait("the credits", in_credits, 30)
            time.sleep(8)
            self.shot("3_credits")
            who = sorted({x[1] for x in credits or []})
            want = "storysc" if story else "credits"  # the trainer's script is named so
            self.ok("credits roll", credits and who == [want], "run by %s, $G2AC %d" % (who, self.global_(ON_MISSION)))
            if story:
                time.sleep(10)
                self.ok("one credits only", len(in_credits()) == 1, str(in_credits()))
            if full:
                back = self.wait("the credits' end", lambda: not in_credits() and self.global_(ON_MISSION) == 0, 1200, 5)
                _, (x, y, _), _ = g.player()
                self.ok("credits ended, player below the building", back and abs(x - BELOW[0]) < 5 and abs(y - BELOW[1]) < 5,
                        "at (%.1f, %.1f)" % (x, y))
                left = [(n, hex(ip)) for _, n, ip, _ in self.scripts() if n in ("credits", "trainer")]
                self.ok("credits script terminated", not left, str(left))
                seen = []
                for k in range(10):  # the credits' DO_FADE 1000 1: the picture back
                    time.sleep(3)
                    seen.append(self.luma("4_after_credits_%d" % k))
                    if seen[-1] > 20:
                        break
                self.ok("screen faded in", seen[-1] > 20, "brightness %s" % " ".join("%.0f" % v for v in seen))
        finally:
            g.quit()
            fails = [r for r in self.results if not r[1]]
            with open(os.path.join(self.out, "summary.txt"), "w", encoding="utf-8") as f:
                for name, good, detail in self.results:
                    f.write("%-4s %s %s\n" % ("ok" if good else "FAIL", name, detail))
                f.write("%d checks, %d failed\n" % (len(self.results), len(fails)))
            print("%d checks, %d failed -> %s" % (len(self.results), len(fails), self.out))
        return self.results and not [r for r in self.results if not r[1]]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--story", action="store_true", help="the story's chapter flags set: the game's own credits")
    ap.add_argument("--full", action="store_true", help="wait for the credits to end")
    ap.add_argument("--port", type=int, default=47673)
    a = ap.parse_args()
    # absolute: the game writes the screenshots, from its own working directory
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "finale", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    sys.exit(0 if Check(out, a.port).run(a.story, a.full) else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Button prompts by device (Enhancements > Button prompts; src/game/vcs_prompts.cpp, game-internals.md 6n).

    python tools/prompts_check.py [OUT] [--port N]

One hidden session from a card copy (rig.default_save). Help boxes with controls in them (the outfit shop: a stick, Cross,
Triangle; the Rhino: L2 / R2, the right stick, Circle; the handbrake: Cross and Square in a car) are put up with
PRINT_HELP_FOREVER, and screenshots taken in each set. Checks:
  expand     the `~k~` text of a few actions turned into control tokens (the `prompts expand` command): R3, the left
             stick or the D-pad, L2 + R2 and the D-pad's up in a vehicle, R1 in the vehicle's context
  sets       each set (PlayStation, Xbox, Keyboard & mouse) is the one shown, and the help box's glyphs are drawn from
             the glyph atlas (glyphs found in the font's batches, glyph draws in the renderer)
  automatic  Automatic: a gamepad button makes it the gamepad's glyphs, a key press the keyboard's - with the help box
             still up
  menu       the pause menu's help-bar buttons (back / select / move) are drawn as glyphs
  original   Original: no glyph found or drawn, the help text in the PS2's words
Screenshots go to OUT (default build/prompts_check); OUT/results.json holds the checks. Exit code 1 when one fails.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamectl  # noqa: E402
import rig  # noqa: E402

ROOT = rig.ROOT
S = rig.Scm
PRINT_HELP_FOREVER = 0x0323
CLEAR_HELP = 0x0268


def state(g):
    """`prompts` -> dict"""
    reply = g.cmd("prompts")
    return dict(kv.split("=", 1) for kv in reply.split() if "=" in kv)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "build", "prompts_check"))
    ap.add_argument("--port", type=int, default=None)
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    results = {}

    def ok(name, cond, detail=""):
        results[name] = {"ok": bool(cond), "detail": str(detail)}
        print("[prompts_check] %-10s %s  %s" % (name, "ok" if cond else "FAILED", detail), flush=True)

    g = rig.Game.start(save=rig.default_save(), name="prompts_check", port=args.port)
    try:
        g.wait_ingame()
        g.cmd("setting enhance.prompts 3")

        def shot(name):
            path = os.path.join(out, name + ".png")
            g.cmd("screenshot " + path)
            return path

        def help_text(key):
            g.scm(S.op(PRINT_HELP_FOREVER, b"\x0a" + key.encode() + b"\x00") + S.end())

        def frames(n=20):
            g.wait_frames(n)

        # expand
        expected = {
            "TGSUB": "{R3@foot}",
            "AMMOV": "{LS@foot} or {DPAD@foot}",
            "VELB": "{L2@vehicle} + {R2@vehicle}",
            "FLUP": "{DU@vehicle}",
            "VEHB": "{R1@vehicle}",
        }
        got = {a: g.cmd("prompts expand " + a) for a in expected}
        ok("expand", all(got[a] == e for a, e in expected.items()), got)

        # sets: Xbox (from the start), PlayStation, Keyboard & mouse
        sets_ok, detail = True, {}
        for value, name in ((3, "xbox"), (2, "ps"), (4, "keyboard")):
            g.cmd("setting enhance.prompts %d" % value)
            frames(10)
            help_text("CLOTHA")
            frames(30)
            st = state(g)
            shot("help_%s_shop" % name)
            good = st.get("shown") == name and int(st.get("glyphs", 0)) >= 3
            drawn0 = int(st.get("drawn", 0))
            help_text("H_IV_06")
            frames(30)
            shot("help_%s_rhino" % name)
            st2 = state(g)
            good = good and int(st2.get("drawn", 0)) > drawn0
            detail[name] = {"shown": st.get("shown"), "glyphs": st.get("glyphs"), "drawn": st2.get("drawn")}
            sets_ok = sets_ok and good
        ok("sets", sets_ok, detail)

        # automatic: the gamepad, then the keyboard, the help box up the whole time
        g.cmd("setting enhance.prompts 1")
        help_text("CLOTHA")
        frames(10)
        g.cmd("gamepad on")
        g.cmd("gamepad lb 1")
        frames(4)
        g.cmd("gamepad lb 0")
        frames(20)
        pad = state(g)
        shot("auto_gamepad")
        g.cmd("gamepad off")
        gamectl.send("press Q", port=g.port)
        frames(20)
        keys = state(g)
        shot("auto_keyboard")
        ok("automatic", pad.get("device") == "gamepad" and pad.get("shown") in ("xbox", "ps") and
           keys.get("device") == "keyboard" and keys.get("shown") == "keyboard",
           {"after the pad": pad.get("shown"), "after a key": keys.get("shown")})

        # menu: the help bars' buttons, keyboard then Xbox
        g.scm(S.op(CLEAR_HELP) + S.end())
        gamectl.send("press Escape", port=g.port)
        time.sleep(2.0)
        st = state(g)
        shot("menu_keyboard")
        g.cmd("setting enhance.prompts 3")
        frames(20)
        st2 = state(g)
        shot("menu_xbox")
        ok("menu", int(st.get("buttons", 0)) >= 2 and int(st2.get("buttons", 0)) >= 2,
           {"keyboard": st.get("buttons"), "xbox": st2.get("buttons")})
        gamectl.send("press Escape", port=g.port)
        time.sleep(1.5)

        # original
        g.cmd("setting enhance.prompts 0")
        frames(10)
        help_text("CLOTHA")
        frames(30)
        before = int(state(g).get("drawn", 0))
        frames(20)
        st = state(g)
        shot("help_original")
        ok("original", st.get("shown") == "original" and int(st.get("glyphs", 0)) == 0 and int(st.get("drawn", 0)) == before,
           st)
    finally:
        g.quit()
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump(results, f, indent=2)
    failed = [k for k, v in results.items() if not v["ok"]]
    print("[prompts_check] %s (%s)" % ("all ok" if not failed else "FAILED: " + ", ".join(failed), out))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

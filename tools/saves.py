#!/usr/bin/env python3
"""Memory card check through the game's own menus (milestone 2, docs/game-internals.md 10e).

    python tools/saves.py [OUT] [--port N]

One session from a copy of build/rig/saves/fortbaxter (slot 1 = "1 Soldier"):
  recover (Windows) a save planted as the card layer once stored "1 Kill Phil: Part 2" - an empty "1 Kill Phil" with
          the save in an NTFS stream - while slots 1 and 2 hold saves: at boot it is the file "3 Kill Phil%3A Part 2",
          the first free slot (game-internals.md 10f)
  save    "Kill Phil: Part 2" registered as the last mission passed (the save's name; a colon), then
          ACTIVATE_SAVE_MENU (script opcode 0x0260) -> Save Game -> slot 2 -> Yes (over "2 In the beginning"):
          "2 Kill Phil%3A Part 2" appears, whole
  load    pause menu -> Game -> Load Game -> slot 1 -> Yes, with trainer god mode on: slot 1 is read in full, the
          player is back in free roam
  delete  pause menu -> Game -> Delete Game -> slot 2 -> Yes: the slot file and its card-order entry are gone
  backup  the card was zipped into <data>/backups/auto-*.zip before the delete: the deleted slot is in it
Screenshots after every key press go to OUT (default build/saves); OUT/results.json holds the checks. Exit code 1
when one fails. Menu keys (CONTROLS.md): Shift = Cross, Return = Start, arrows = D-pad.
"""
import argparse
import glob
import json
import os
import shutil
import sys
import time
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rig  # noqa: E402

ROOT = rig.ROOT
FOLDER = ("mc0", "BASLUS-21590GTA40001")


def normalise_seed(folder):
    """The steps below expect the card the check was written with: slot 1 "1 Soldier" plus the stray "1 In the
    beginning". The rig's Fort Baxter card is each machine's own (build/ is not in git; one holds "1 Cholo Vitorioso"
    alone): its slot-1 save is renamed "1 Soldier" and copied as the stray when that is missing. A save's title on
    the card is its file name after the slot number, so the copies load as the original."""
    order_path = os.path.join(folder, ".ps2x-order")
    order = open(order_path).read().split("\n")
    slot1 = sorted(f for f in os.listdir(folder) if f.startswith("1 "))
    if not slot1:
        return
    main_save = "1 Soldier" if "1 Soldier" in slot1 else next(f for f in slot1 if f != "1 In the beginning")
    if main_save != "1 Soldier":
        os.rename(os.path.join(folder, main_save), os.path.join(folder, "1 Soldier"))
        order = ["1 Soldier" if o == main_save else o for o in order]
    if not os.path.exists(os.path.join(folder, "1 In the beginning")):
        shutil.copyfile(os.path.join(folder, "1 Soldier"), os.path.join(folder, "1 In the beginning"))
        order = [o for o in order if o] + ["1 In the beginning", ""]
    open(order_path, "w", newline="\n").write("\n".join(order))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?", default=os.path.join(ROOT, "build", "saves"))
    ap.add_argument("--port", type=int, default=47640)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    data = os.path.join(out, "data")
    folder = os.path.join(data, *FOLDER)
    seed = os.path.join(out, "seed")
    rig.copy_card(os.path.join(ROOT, "build", "rig", "saves", "fortbaxter", "mc0"), os.path.join(seed, "mc0"))
    normalise_seed(os.path.join(seed, *FOLDER))
    if os.path.isdir(os.path.join(data, "backups")):
        shutil.rmtree(os.path.join(data, "backups"))
    if os.name == "nt":
        # what the card layer did before names were encoded: fopen("1 Kill Phil: Part 2") on NTFS. Slot 1 holds a save
        # (as when the player saved again after the loss) and the card's stray "1 In the beginning" becomes slot 2: the
        # recovered save must go to slot 3, the first free one
        seed_folder = os.path.join(seed, *FOLDER)
        with open(os.path.join(seed_folder, "1 Soldier"), "rb") as f, \
                open(os.path.join(seed_folder, "1 Kill Phil: Part 2"), "wb") as lost:
            lost.write(f.read())
        os.rename(os.path.join(seed_folder, "1 In the beginning"), os.path.join(seed_folder, "2 In the beginning"))
        order = os.path.join(seed_folder, ".ps2x-order")
        text = open(order).read().replace("1 In the beginning", "2 In the beginning")
        open(order, "w", newline="\n").write(text)
    g = rig.Game.start(save=seed, name="saves", data_dir=data, port=a.port)
    shots = [0]

    def keys(tag, *seq):
        """KEY[@seconds to wait after], a screenshot after each"""
        for k in seq:
            key, _, wait = k.partition("@")
            g.cmd("press %s 150" % key)
            time.sleep(float(wait or 1.5))
            shots[0] += 1
            g.cmd("screenshot " + os.path.join(out, "%02d_%s_%s.png" % (shots[0], tag, key)))

    def slot_files():
        return sorted(f for f in os.listdir(folder) if f[:1].isdigit())

    def log_text():
        return open(g.log_path, errors="replace").read()

    checks = {}
    try:
        g.wait_ingame()
        _, saved_pos, _ = g.player()  # where the card's save put the player
        if os.name == "nt":
            got = os.path.join(folder, "3 Kill Phil%3A Part 2")
            checks["recover"] = {"ok": os.path.isfile(got) and os.path.getsize(got) == 98304 and
                                 not os.path.exists(os.path.join(folder, "1 Kill Phil")), "files": slot_files()}
        before = slot_files()
        # save: named after the last mission passed - REGISTER_MISSION_PASSED (0x01EB) "REN_C05", "Kill Phil: Part 2".
        # The menu opens on "Save Game"; the slot list starts on Cancel, two down is slot 2
        g.cmd("scm " + (rig.Scm.op(0x01EB) + b"\x0aREN_C05\x00" + rig.Scm.end()).hex())
        g.cmd("scm " + (rig.Scm.op(0x0260) + rig.Scm.end()).hex())
        time.sleep(3)
        keys("save", "Shift@3", "Down", "Down", "Shift@3", "Up", "Shift@12", "Shift@4")
        new = [f for f in slot_files() if f not in before]
        # (named in the game's language: "2 Matar Phil%3A Parte 2" in Portuguese - slot 2, the colon encoded)
        checks["save"] = {"ok": len(new) == 1 and new[0].startswith("2 ") and "%3A" in new[0] and
                          os.path.getsize(os.path.join(folder, new[0])) == 98304, "new_files": new}
        # load slot 1 (Map -> Brief -> Game tab), from the other end of the map: the load must bring the player back.
        # Trainer god mode on first: its tick script's slot is one the load fills with the save's scripts, which the
        # trainer once took for its own and cancelled - the load then hung on "Loading..." (game-internals.md 11d)
        g.teleport(575.0, 175.0, -100.0)
        g.cmd("trainer toggle god 1")
        time.sleep(4)
        mark = len(log_text())
        keys("load", "Return@3", "Right", "Right", "Shift@2", "Shift@3", "Down", "Shift@3", "Up", "Shift@15")
        # the runtime's [MC] lines reach the log through a buffered stdout (maybe only at exit): the player being
        # back where the save put them is the check (a hung load leaves him at INITPLA's start); the full read is
        # reported when the log already has it
        tail = log_text()[mark:]
        read_full = "'/BASLUS-21590GTA40001/1 Soldier' flags=0x1" in tail and "result=98300" in tail
        _, pos, mission = g.player()
        back = abs(pos[0] - saved_pos[0]) < 30 and abs(pos[1] - saved_pos[1]) < 30
        checks["load"] = {"ok": back and not mission, "back_at_save": back, "slot1_read_logged": read_full,
                          "trainer_slot_taken_by_load": "[rig] script slot" in tail, "mission_running": mission,
                          "pos": pos, "saved_pos": saved_pos}
        # delete slot 2 (the Game tab again: Load, New, Delete)
        keys("delete", "Return@3", "Right", "Right", "Shift@2", "Down", "Down", "Shift@3", "Down", "Down", "Shift@3",
             "Up", "Shift@8", "Shift@3", "F@2", "F@2", "F@2")
        order = open(os.path.join(folder, ".ps2x-order")).read().split("\n")
        left = [f for f in slot_files() if f.startswith("2 ")]
        checks["delete"] = {"ok": not left and not any(o.startswith("2 ") for o in order), "left": left}
        backups = sorted(glob.glob(os.path.join(data, "backups", "auto-*.zip")))
        held = []
        if backups:
            with zipfile.ZipFile(backups[-1]) as z:
                held = [n.rsplit("/", 1)[-1] for n in z.namelist() if n.rsplit("/", 1)[-1][:1].isdigit()]
        checks["backup"] = {"ok": bool(new) and new[0] in held, "zips": [os.path.basename(b) for b in backups],
                            "slots_in_newest": held}
    finally:
        g.quit()
    json.dump(checks, open(os.path.join(out, "results.json"), "w"), indent=1)
    for k, v in checks.items():
        print("%-7s %s %s" % (k, "ok" if v["ok"] else "FAIL", {x: y for x, y in v.items() if x != "ok"}))
    return 0 if checks and all(v["ok"] for v in checks.values()) and len(checks) == (5 if os.name == "nt" else 4) else 1


if __name__ == "__main__":
    sys.exit(main())

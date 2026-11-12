#!/usr/bin/env python3
"""Regression suite (milestone 1 of NATIVE_PORT_ROADMAP.md): one command that runs the rig's checks and compares
them with a stored baseline.

    python tools/regress.py [OUT] [--quick] [--set-baseline]

Steps (each a tool of its own, run in turn; the game window shows):
  scenes    tools/scenes.py capture + compare with the baseline's scenes (picture diffs, perf)
  tour      tools/tour.py on a fixed subset of the map grid (classes per point)
  missions  tools/missions.py on a few story missions (crash / hang / dark / frame time; 94 = GON_C3, the
            poolside ground the GPU clipping drew black)
  drive     tools/drive.py on two spots (frame time, black / dip frames)
  evening   the hospital at 12:00 / 20:00 / 21:00 - no black pictures (the evening black frames)
  saves     tools/saves.py: save to slot 2, Load Game slot 1, Delete Game slot 2 through the game's menus
  shakedown tools/shakedown.py: mission 50's shop entry at 144 fps (the shopkeeper deleted by the 32-bit branch bug,
            the mission stuck on "Loading...")
  haze      tools/haze_check.py: mission 94's cutscene with original timing at 60 fps (a thug run over behind the
            fade, his audio line waited for for ever) and the haze across a 16:9 picture
  widecull  at 16:9, 16:10, 21:9 and 32:9 the entity culling (CCamera's side planes) reaches the screen edges, not
            the 4:3 ones
  widepref  the game's own (anamorphic) widescreen preference stays off; mission 50's cutscene has its black bars by
            default and none with Display > Cutscene black bars off
  scopemask the sniper scope and the camera at 16:9: their black mask reaches the screen edges (no world beside it)
  menushape every presented frame into and out of the pause menu at 32:9 on a 16:9 screen: a 4:3 shape shows the menu,
            a wide one the game (the shifted frame at the switch)
            (buildings, peds and cars vanishing at the sides in widescreen)
  meters    per-frame mission meters on the time step: every scaled instruction found in its mission (not --quick),
            PHI_A2's heat bar as fast per game second at 1/144 as at 1/30
  controls  tools/controls_check.py: every default key binding, rebinding, walk, wheel, presets, Reset all settings,
            the game's vibration reaching the host
  fire      tools/fire_check.py: a burning car, the flame-thrower and a molotov - fire on screen (the GS fire sprites
            lost to the D3D11 shader depth, game-internals.md 6u)

Writes OUT/report.md and OUT/metrics.json (default OUT: build/regress/<date-time>). --set-baseline stores this
run as build/regress/baseline (metrics.json + the scenes capture); later runs report what changed against it:
tour classes, mission results, frame times (+10% or more), new black / dip frames, failing scenes.
--quick skips missions and halves the rest.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
BASELINE = os.path.join(ROOT, "build", "regress", "baseline")


def run(args, log):
    print("  $", " ".join(args))
    with open(log, "w") as f:
        r = subprocess.run([sys.executable] + args, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
    return r.returncode, open(log, errors="replace").read()


def step_scenes(out, quick):
    cap = os.path.join(out, "scenes")
    run([os.path.join(TOOLS, "scenes.py"), "capture", cap], os.path.join(out, "scenes_capture.log"))
    m = {"captured": sorted(f[:-4] for f in os.listdir(cap) if f.endswith(".png") and f != "sheet.png")} if os.path.isdir(cap) else {}
    ref = os.path.join(BASELINE, "scenes")
    if os.path.isdir(ref):
        rc, text = run([os.path.join(TOOLS, "scenes.py"), "compare", ref, cap], os.path.join(out, "scenes_compare.log"))
        m["compare_failed"] = [l.split()[0] for l in text.splitlines() if l.rstrip().endswith("FAIL") or "FAIL missing" in l]
    return m


def step_tour(out, quick):
    d = os.path.join(out, "tour")
    args = [os.path.join(TOOLS, "tour.py"), d, "--every", "9" if not quick else "18", "--limit", "27" if not quick else "12"]
    run(args, os.path.join(out, "tour.log"))
    recs = [json.loads(l) for l in open(os.path.join(d, "results.jsonl"))] if os.path.isfile(os.path.join(d, "results.jsonl")) else []
    classes = {}
    for r in recs:
        classes[r["class"]] = classes.get(r["class"], 0) + 1
    fts = sorted(r.get("perf", {}).get("frame_ms", 0) for r in recs if r.get("perf"))
    return {"points": len(recs), "classes": classes, "per_point": {"%d,%d" % tuple(r["target"]): r["class"] for r in recs},
            "frame_ms_median": fts[len(fts) // 2] if fts else None}


def step_missions(out, quick):
    if quick:
        return {}
    d = os.path.join(out, "missions")
    run([os.path.join(TOOLS, "missions.py"), d, "--only", "45,46,50,55,94", "--seconds", "30"], os.path.join(out, "missions.log"))
    path = os.path.join(d, "results.jsonl")
    recs = [json.loads(l) for l in open(path)] if os.path.isfile(path) else []
    return {str(r["mission"]): {k: r.get(k) for k in ("name", "result", "mission_running", "worst_frame_ms", "dark_shots")} for r in recs}


def step_drive(out, quick):
    d = os.path.join(out, "drive")
    _, text = run([os.path.join(TOOLS, "drive.py"), d, "--spots", "0,2", "--seconds", "45" if not quick else "25"],
                  os.path.join(out, "drive.log"))
    lines = [l for l in text.splitlines() if l.startswith("spot ")]
    fms = [float(re.search(r"frame_ms=([\d.]+)", l).group(1)) for l in lines]
    black = max([int(re.search(r"black=(\d+)", l).group(1)) for l in lines] or [0])
    dips = max([int(re.search(r"dips=(\d+)", l).group(1)) for l in lines] or [0])
    events = [l.strip() for l in text.splitlines() if "[frames]" in l]
    return {"samples": len(lines), "frame_ms_max": max(fms) if fms else None,
            "frame_ms_mean": round(sum(fms) / len(fms), 1) if fms else None, "black_frames": black, "dip_frames": dips,
            "events": events[-20:]}


def step_evening(out, quick):
    """The evening black frames (game-internals.md 6g): at the hospital, 20:00 and 21:00 must show a picture. Fixed
    2026-10-23 (render targets tracked taller than the scissor were dropped by a texture upload into the Z area)."""
    sys.path.insert(0, TOOLS)
    import rig  # noqa: E402
    import tour  # noqa: E402
    d = os.path.join(out, "evening")
    os.makedirs(d, exist_ok=True)
    g = rig.Game.start(save=rig.default_save(), name="regress_evening")
    lumas = {}
    try:
        g.wait_ingame()
        g.invincible(True)
        g.prime()
        g.teleport(-884.0, -470.0, -100.0)
        time.sleep(3)
        for h in (12, 20, 21):
            g.set_time(h, 0)
            time.sleep(2.5)  # the scene load's fade is over
            vals = []
            for k in range(3):
                path = os.path.join(d, "t%02d_%d.png" % (h, k))
                g.cmd("screenshot " + path)
                vals.append(round(tour.picture_stats(path)[0], 1))
                time.sleep(0.4)
            lumas["%02d:00" % h] = vals
    finally:
        g.quit()
    black = [t for t, v in lumas.items() if min(v) < 10]
    return {"luma": lumas, "black_times": black}


def step_saves(out, quick):
    d = os.path.join(out, "saves")
    run([os.path.join(TOOLS, "saves.py"), d], os.path.join(out, "saves.log"))
    path = os.path.join(d, "results.json")
    return {k: v["ok"] for k, v in json.load(open(path)).items()} if os.path.isfile(path) else {"error": "no results"}


def step_shakedown(out, quick):
    sys.path.insert(0, TOOLS)
    import shakedown  # noqa: E402
    return shakedown.check(os.path.join(out, "shakedown"))


def step_haze(out, quick):
    sys.path.insert(0, TOOLS)
    import haze_check  # noqa: E402
    return haze_check.check(os.path.join(out, "haze"))


def step_widecull(out, quick):
    """Widescreen culling (game-internals.md 5): CCamera::IsSphereVisible's side planes (TheCamera 0x6F44D0 + 0xB60,
    right (cos h, -sin h)) must end where the widened view window does (tan(FOV / 2) x r at 0x742AC0), not at the
    game's 4:3 edge. Fixed 2026-10-27 (src/game/vcs_display.cpp, prepareSphereTest)."""
    sys.path.insert(0, TOOLS)
    import rig  # noqa: E402
    res = {}
    for aspect in ("169", "329") if quick else ("169", "1610", "219", "329"):
        g = rig.Game.start(save=rig.default_save(), name="regress_widecull_" + aspect,
                           env={"VCS_ASPECT": aspect})
        try:
            g.wait_ingame()
            g.cmd("pause on")
            rx, ry = g.peekf(0x6F44D0 + 0xB60, 2)
            lx, ly = g.peekf(0x6F44D0 + 0xB70, 2)
            window = g.peekf(0x742AC0, 1)[0]
            g.cmd("pause off")
        finally:
            g.quit()
        right, left = -ry / rx, ly / lx
        res[aspect] = {"view_window": round(window, 4), "right_edge": round(right, 4), "left_edge": round(left, 4),
                       "pass": abs(right - window) < 1e-3 * window and abs(left - window) < 1e-3 * window}
    return res


def step_widepref(out, quick):
    """The game's own widescreen preference (*(*0x48F3D0 + 0x30), the PS2's anamorphic mode) stays off, and the cutscene
    letterbox follows Display > Cutscene black bars (game-internals.md 5a, src/game/vcs_display.cpp, 2026-11-06: the
    game's option removed the bars). Mission 50's opening cutscene, the top and bottom twelfth of the picture black with
    the bars, not without them: 16:9 with the setting On and Off, and 4:3 with it Off - the bars stay there (the setting
    is locked On at 4:3, 2026-11-06). The hidden window is 4:3, so a 16:9 picture is letterboxed inside it."""
    sys.path.insert(0, TOOLS)
    import rig  # noqa: E402
    import toolchain as tc  # noqa: E402
    d = os.path.abspath(os.path.join(out, "widepref"))
    os.makedirs(d, exist_ok=True)
    res = {}
    for aspect, bars, want in (("169", "1", True), ("169", "0", False), ("43", "0", True)):
        name = "%s_bars%s" % (aspect, bars)
        g = rig.Game.start(save=rig.default_save(), name="regress_widepref_" + name,
                           env={"VCS_ASPECT": aspect, "VCS_CUTSCENE_BARS": bars})
        try:
            g.wait_ingame()
            prefs = g.u32(0x48F3D0)
            g.cmd("poke 0x%x 01000000" % (prefs + 0x30))  # as a save with the option on would (the setter is hooked too)
            g.wait_frames(5)
            pref = g.u32(prefs + 0x30)
            g.invincible(True)
            g.prime()
            g.mission(50)
            time.sleep(12)
            shot = os.path.join(d, "cutscene_%s.png" % name)
            g.cmd("screenshot " + shot.replace(os.sep, "/"))
            cutscene = g.u32(g.u32(0x48F820) + 0x34)
        finally:
            g.quit()
        w, h, rows = tc.read_png(shot)
        pic = h if aspect == "43" else min(h, w * 9 // 16)  # the picture's rows inside the hidden 4:3 window
        y0 = (h - pic) // 2
        band = pic // 12
        mean = lambda ys: sum(sum(rows[y][x] for x in range(0, w * 3, 12)) for y in ys) / (len(ys) * w / 4)  # noqa: E731
        top, bottom = mean(range(y0 + 2, y0 + band - 2)), mean(range(y0 + pic - band + 2, y0 + pic - 2))
        res[name] = {"pref": pref, "cutscene": cutscene, "top": round(top, 1), "bottom": round(bottom, 1),
                     "pass": pref == 0 and cutscene != 0 and ((top < 3 and bottom < 3) if want else max(top, bottom) > 8)}
    res["pass"] = all(r["pass"] for r in res.values())
    return res


def step_scopemask(out, quick):
    """The sniper scope and the camera at 16:9 (game-internals.md 6f "Scope mask", src/game/vcs_hud.cpp, 2026-11-06):
    the mask's side bars reach the screen edges - before, they and the scope were squeezed into the middle 4:3 and the
    3D world showed in the strips beside it (max 253 there). The hidden window is 4:3, so the 16:9 picture is
    letterboxed in it: the strips are x 0..1/8 and 7/8..1 of the screenshot inside the picture's rows."""
    sys.path.insert(0, TOOLS)
    import rig  # noqa: E402
    import toolchain as tc  # noqa: E402
    d = os.path.abspath(os.path.join(out, "scopemask"))
    os.makedirs(d, exist_ok=True)
    res = {}
    g = rig.Game.start(save=rig.default_save(), name="regress_scopemask",
                       env={"VCS_ASPECT": "169", "VCS_HUD": "0"})
    try:
        g.wait_ingame()
        g.invincible(True)
        g.set_time(12)
        for weapon in (30, 39):  # the sniper rifle (camera mode 7), the camera (47)
            g.scm(rig.Scm.op(rig.Scm.GIVE_WEAPON_TO_CHAR, rig.Scm.PLAYER, rig.Scm.i(weapon), rig.Scm.i(50)))
            time.sleep(1.0)
            g.cmd("key mouseright down")
            time.sleep(1.5)
            shot = os.path.join(d, "weapon%d.png" % weapon)
            g.cmd("screenshot " + shot.replace(os.sep, "/"))
            g.cmd("key mouseright up")
            time.sleep(1.0)
            w, h, rows = tc.read_png(shot)
            top, bottom = (h - w * 9 // 16) // 2 + 8, (h + w * 9 // 16) // 2 - 8
            strip = lambda x0, x1: max(max(rows[y][x * 3:x * 3 + 3]) for y in range(top, bottom, 3) for x in range(x0, x1, 2))  # noqa: E731
            left, right = strip(2, w // 8 - 5), strip(w - w // 8 + 5, w - 2)
            res[str(weapon)] = {"left_max": left, "right_max": right, "pass": left <= 8 and right <= 8}
    finally:
        g.quit()
    res["pass"] = all(r["pass"] for r in res.values())
    return res


def step_meters(out, quick):
    """Mission meters that count per frame on the time step (game-internals.md 11f, src/game/vcs_script_timing.cpp,
    fixed 2026-10-27): every listed instruction found in its mission (not with --quick: a game per mission), and
    PHI_A2's heat bar per game second at 1/144 as at 1/30 (within 10%; it filled 4.8x as fast). 10%, not 5%: the 1/30
    reference itself - the game's own per-frame rate, nothing scaled - read 30.6, 29.1 and 28.2 a second in three runs
    of its 1.5 s window, while 1/144 read ~30 each time."""
    sys.path.insert(0, TOOLS)
    import framerate_audit as fa  # noqa: E402
    res = {"sites": None if quick else fa.check_sites(47670)}
    for step in (30, 144):
        s = fa.Session(step, "regress_meters_%d" % step, 47670)
        try:
            res[str(step)] = fa.scenario_heat(s)
        finally:
            s.quit()
    a, b = res["30"], res["144"]
    res["pass"] = res["sites"] is not False and all(abs(b[k] - a[k]) <= 0.10 * abs(a[k]) for k in ("floor_per_s", "heat_per_s"))
    return res


def step_controls(out, quick):
    """The player's controls and settings (docs/CONTROLS.md, src/game/vcs_input.cpp, 2026-10-28): tools/controls_check.py
    (the bindings in the pad report and the game, presets, reset, vibration, menu screenshots)."""
    sys.path.insert(0, TOOLS)
    import controls_check as cc  # noqa: E402
    d = os.path.abspath(os.path.join(out, "controls"))
    os.makedirs(d, exist_ok=True)
    c = cc.Check(d, 47673)
    good = c.run()
    return {"pass": good, "failed": [name for name, ok, _ in c.results if not ok], "checks": len(c.results)}


def step_fire(out, quick):
    """Fire on screen (game-internals.md 6u): tools/fire_check.py, one run as the player plays."""
    d = os.path.abspath(os.path.join(out, "fire"))
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "fire_check.py"), d, "--variant", "as-is="], capture_output=True,
                       text=True, timeout=900)
    try:
        res = json.load(open(os.path.join(d, "result.json")))["as-is"]
    except (OSError, ValueError, KeyError):
        return {"pass": False, "error": (r.stdout or r.stderr)[-300:]}
    return {"pass": res.get("pass") is True, "best": res.get("best")}


def step_distance(out, quick):
    """Object distance (game-internals.md 9h): tools/distance_check.py - 100 % is the game's, 300 % draws more."""
    d = os.path.abspath(os.path.join(out, "distance"))
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "distance_check.py"), d], capture_output=True, text=True,
                       timeout=900)
    try:
        res = json.load(open(os.path.join(d, "result.json")))
    except (OSError, ValueError):
        return {"pass": False, "error": (r.stdout or r.stderr)[-300:]}
    return {"pass": res.get("pass") is True, "visible": [s["visible"] for s in res["steps"]]}


def step_menushape(out, quick):
    """The pause menu's shape (SETTINGS_MENU_WIDESCREEN.md): 32:9 game, 16:9 screen; every presented frame for 50 frames
    after Start, in and out (control command "burst"). Each frame's picture rectangle (4:3 or wide) has to fit its
    content - the menu has no sky at the top, the game outside the barracks has. Fixed 2026-10-27: VCS shows the previous
    frame, and frames of the game were shown squashed into the menu's 4:3 box."""
    sys.path.insert(0, TOOLS)
    import rig  # noqa: E402
    import toolchain as tc  # noqa: E402
    d = os.path.abspath(os.path.join(out, "menushape"))  # the game writes it (its own working directory)
    env = {"VCS_ASPECT": "329", "VCS_HUD": "2", "PS2X_HW_OUTPUT": "1920x1080", "PS2X_CAPTURE_SIZE": "480x270",
           "VCS_PERF_OVERLAY": "0"}
    g = rig.Game.start(save=rig.default_save(), name="regress_menushape", env=env)
    try:
        g.wait_ingame()
        g.prime()
        g.set_time(12)
        g.weather(0)
        time.sleep(3)
        for phase in ("in", "out"):
            os.makedirs(os.path.join(d, phase), exist_ok=True)
            g.cmd("burst %s 50" % os.path.join(d, phase).replace(os.sep, "/"))
            g.cmd("press Escape 150")
            time.sleep(4)
    finally:
        g.quit()
    res = {}
    for phase in ("in", "out"):
        seq, bad = "", 0
        for k in range(50):
            path = os.path.join(d, phase, "burst_%04d.png" % k)
            if not os.path.isfile(path):
                break
            w, h, rows = tc.read_png(path)
            lit = lambda x, y: sum(rows[y][x * 3:x * 3 + 3]) > 30  # noqa: E731
            xs = [x for x in range(w) if lit(x, h // 2)]
            ys = [y for y in range(h) if lit(w // 2, y)]
            if not xs or not ys:
                seq += "."
                continue
            pw, ph = xs[-1] - xs[0] + 1, ys[-1] - ys[0] + 1
            blue = red = 0
            n = 0
            for y in range(ys[0] + 2, ys[0] + ph // 6):
                for x in range(w // 2 - pw // 8, w // 2 + pw // 8):
                    red += rows[y][x * 3]
                    blue += rows[y][x * 3 + 2]
                    n += 1
            game = blue / n > 150 and blue / n > red / n + 40
            narrow = pw / ph < 1.5
            seq += ("G" if game else "M") + ("4" if narrow else "W") + ("" if narrow != game else "!")
            bad += narrow == game
        res[phase] = {"frames": seq, "mismatched": bad}
    res["pass"] = all(r["mismatched"] == 0 and len(r["frames"]) >= 50 for r in res.values() if isinstance(r, dict))
    return res


def compare(base, new):
    notes = []
    bt, nt = base.get("tour", {}), new.get("tour", {})
    for p, c in nt.get("per_point", {}).items():
        b = bt.get("per_point", {}).get(p)
        if b and b != c:
            notes.append("tour %s: %s -> %s" % (p, b, c))
    for k, r in new.get("missions", {}).items():
        b = base.get("missions", {}).get(k)
        if b and (b.get("result") != r.get("result") or (r.get("dark_shots") or 0) > (b.get("dark_shots") or 0)):
            notes.append("mission %s %s: %s -> %s" % (k, r.get("name"), b, r))
        if b and b.get("worst_frame_ms") and r.get("worst_frame_ms") and r["worst_frame_ms"] > b["worst_frame_ms"] * 1.1:
            notes.append("mission %s worst frame %.1f -> %.1f ms" % (k, b["worst_frame_ms"], r["worst_frame_ms"]))
    bd, nd = base.get("drive", {}), new.get("drive", {})
    for k in ("frame_ms_mean", "frame_ms_max"):
        if bd.get(k) and nd.get(k) and nd[k] > bd[k] * 1.1:
            notes.append("drive %s %.1f -> %.1f" % (k, bd[k], nd[k]))
    for k in ("black_frames", "dip_frames"):
        if nd.get(k, 0) > bd.get(k, 0):
            notes.append("drive %s %d -> %d" % (k, bd.get(k, 0), nd.get(k, 0)))
    for t in new.get("evening", {}).get("black_times", []):
        notes.append("evening: black picture at %s (the evening black frames are back?)" % t)
    for k, ok in new.get("saves", {}).items():
        if ok is not True:
            notes.append("saves: %s failed (see saves.log and the screenshots)" % k)
    if "shakedown" in new and new["shakedown"].get("pass") is not True:
        notes.append("shakedown: the shop entry failed %s (see shakedown/)" % json.dumps(new["shakedown"]))
    if "haze" in new and new["haze"].get("pass") is not True:
        notes.append("haze: Purple Haze's cutscene or its haze at 16:9 failed %s (see haze/)" % json.dumps(new["haze"]))
    if "controls" in new and new["controls"].get("pass") is not True:
        notes.append("controls: failed checks %s (tools/controls_check.py)" % new["controls"].get("failed"))
    if "fire" in new and new["fire"].get("pass") is not True:
        notes.append("fire: no fire on screen %s (tools/fire_check.py, game-internals.md 6u)" % json.dumps(new["fire"]))
    if "meters" in new and new["meters"].get("pass") is not True:
        notes.append("meters: per-frame mission meters off the time step %s (PHI_A2's heat at 144 vs 30)" % json.dumps(new["meters"]))
    wp = new.get("widepref", {})
    if wp and wp.get("pass") is not True:
        notes.append("widepref: the game's widescreen preference or the cutscene bars %s (widepref/)" % json.dumps(wp))
    sm = new.get("scopemask", {})
    if sm and sm.get("pass") is not True:
        notes.append("scopemask: the 3D world shows beside the scope / camera mask at 16:9 %s (scopemask/)" % json.dumps(sm))
    ms = new.get("menushape", {})
    if ms and ms.get("pass") is not True:
        notes.append("menushape: frames whose shape does not fit their picture at the pause menu %s" % json.dumps(ms))
    for aspect, r in new.get("widecull", {}).items():
        if isinstance(r, dict) and r.get("pass") is not True:
            notes.append("widecull %s: entity culling edge %s, view window %s (models culled at the 4:3 edges again?)" % (
                aspect, r.get("right_edge"), r.get("view_window")))
    for s in new.get("scenes", {}).get("compare_failed", []):
        notes.append("scene %s differs from the baseline (see scenes_compare.log, diff images)" % s)
    return notes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--set-baseline", action="store_true")
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.join(ROOT, "build", "regress", time.strftime("%Y%m%d-%H%M%S")))
    os.makedirs(out, exist_ok=True)
    metrics = {"started": time.strftime("%Y-%m-%d %H:%M:%S"), "quick": a.quick}
    for name, fn in (("scenes", step_scenes), ("tour", step_tour), ("missions", step_missions), ("drive", step_drive),
                     ("evening", step_evening), ("saves", step_saves), ("shakedown", step_shakedown),
                     ("haze", step_haze),
                     ("widecull", step_widecull), ("widepref", step_widepref), ("scopemask", step_scopemask),
                     ("menushape", step_menushape),
                     ("meters", step_meters), ("controls", step_controls), ("fire", step_fire), ("distance", step_distance)):
        print("[regress] %s" % name)
        t0 = time.time()
        try:
            metrics[name] = fn(out, a.quick)
        except Exception as e:  # a broken step must not hide the others
            metrics[name] = {"error": str(e)[:300]}
        metrics[name + "_s"] = round(time.time() - t0)
    json.dump(metrics, open(os.path.join(out, "metrics.json"), "w"), indent=1)

    base_path = os.path.join(BASELINE, "metrics.json")
    notes = compare(json.load(open(base_path)), metrics) if os.path.isfile(base_path) else ["(no baseline yet)"]
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write("# Regression run %s\n\n" % metrics["started"])
        f.write("## Changes against the baseline\n\n" + ("\n".join("- " + n for n in notes) if notes else "- none") + "\n\n")
        f.write("## Tour\n\n%s\n\n## Missions\n\n%s\n\n## Drive\n\n%s\n" % (
            json.dumps(metrics.get("tour", {}).get("classes")), json.dumps(metrics.get("missions"), indent=1),
            json.dumps({k: v for k, v in metrics.get("drive", {}).items() if k != "events"})))
    print(open(os.path.join(out, "report.md")).read())
    if a.set_baseline:
        if os.path.isdir(BASELINE):
            shutil.rmtree(BASELINE)
        os.makedirs(BASELINE)
        shutil.copy(os.path.join(out, "metrics.json"), base_path)
        if os.path.isdir(os.path.join(out, "scenes")):
            shutil.copytree(os.path.join(out, "scenes"), os.path.join(BASELINE, "scenes"))
        print("baseline stored in", BASELINE)
    return 0


if __name__ == "__main__":
    sys.exit(main())

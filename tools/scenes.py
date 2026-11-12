#!/usr/bin/env python3
"""Reference scenes: run the game along a fixed script, capture screenshots + perf, compare two captures.

    python tools/scenes.py capture OUT [--hidden]            the reference script (SCRIPT below) -> OUT/
    python tools/scenes.py explore OUT [--seconds 120] [--every 3] [--press 40:Return,45:Shift:300] [--hidden]
    python tools/scenes.py compare REF NEW [--out DIR]       per-scene difference + diff images

Every run starts from an empty memory card (VCS_DATA_DIR = build/scenes_data, wiped first), so the boot path is
the same each time: legal screens, loading, title, Start = new game, intro cutscene, Fort Baxter.
Timings are seconds after the control channel answers. The game is driven over the control channel
(tools/gamectl.py), so the window may be covered; it is shown unless --hidden.

capture writes OUT/<scene>.png, OUT/perf.csv (PS2X_PERF_LOG, the whole run), OUT/perf_<scene>.txt (the perf
overlay's numbers for the second before the shot), OUT/run.log and OUT/scenes.json.

compare: scenes marked static should match closely between runs (same renderer -> mean difference ~0);
moving scenes differ with timing, so they are reported but only fail when the picture is black or missing.
Needs Pillow for compare / contact sheets.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamectl  # noqa: E402
import toolchain as tc  # noqa: E402

# (seconds, action, argument, static?) - action: shot <scene> | press <key[:ms]> | perf <scene>
# Timeline from `explore` runs (empty card, PS2X_GS=hw, i7-7700K): legal screen ~0.5 s, loading screen 0.7-1.2 s (1.2-1.6 s
# before zlib's inflate ran natively, 2026-10-26: GAME.DTZ is inflated faster - with VCS_NATIVE_INFLATE=0 shoot it at 1.4),
# flyover ~3-38 s (a fade at ~40 s), intro cutscene from ~44 s (Cross skips it), gameplay next to Martinez's bike from
# ~100 s. (Until 2026-10-25 the boot took longer - loading 1-4 s, flyover 6-45 s: the game thread got ~2x faster.)
# Moving scenes drift by a second or two between runs; static ones are loading screen and the pause map.
SCRIPT = [
    (1.0, "shot", "loading", True),
    (12.0, "shot", "flyover_road", False),
    (24.0, "shot", "flyover_tower", False),
    (36.0, "shot", "flyover_base", False),
    (38.0, "perf", "flyover", False),
    (72.0, "shot", "cutscene_office", False),
    (86.0, "perf", "cutscene", False),
    (88.0, "press", "Shift", False),            # Cross: skip the cutscene
    (106.0, "shot", "gameplay_hud", False),
    (108.0, "perf", "gameplay", False),
    (110.0, "press", "Escape", False),          # Start: pause menu (map)
    (116.0, "shot", "pause_map", True),
    (118.0, "press", "Escape", False),          # back out of it (Triangle on the tab bar)
    (124.0, "shot", "gameplay_resumed", False),
]
STATIC_TOLERANCE = 2.0  # mean absolute difference (0-255) a static scene may have


def launch(out, hidden, extra_env=None):
    if not os.path.isfile(tc.VCS_EXE):
        tc.fail("not built yet: python tools/build.py --recompile")
    iso = tc.find_iso(None)
    data = os.path.join(tc.BUILD, "scenes_data")
    shutil.rmtree(data, ignore_errors=True)
    os.makedirs(data)
    env = dict(os.environ)
    env.setdefault("PS2X_CAPTURE_SIZE", "960x672")  # screenshots independent of the window size
    # the original display settings whatever vcs.ini says (captures compare across runs); a caller's env wins
    for key, value in (("VCS_PERF_OVERLAY", "0"), ("VCS_ASPECT", "43"), ("VCS_HUD", "0"), ("VCS_FPS", "30"),
                       ("PS2X_HW_SCALE", "1"), ("PS2X_HW_MSAA", "1"), ("PS2X_HW_ANISO", "0"), ("VCS_SKIP_MOVIES", "1")):
        env.setdefault(key, value)
    env["PS2X_CONTROL"] = "1"
    env["VCS_DATA_DIR"] = data
    tc.test_window(env, visible=not hidden and os.environ.get("VCS_TEST_VISIBLE") == "1")
    env.update(extra_env or {})
    log = open(os.path.join(out, "run.log"), "w", encoding="utf-8")
    proc = subprocess.Popen([tc.VCS_EXE, iso], cwd=out, stdout=log, stderr=subprocess.STDOUT, env=env)
    if not gamectl.wait(timeout=60):
        proc.kill()
        tc.fail("the game did not open its control channel (see " + os.path.join(out, "run.log") + ")")
    return proc, log, time.time()


def stop(proc, log):
    try:
        gamectl.send("quit", timeout=5)
    except OSError:
        pass
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    log.close()


def wait_until(t0, seconds, proc):
    while time.time() - t0 < seconds:
        if proc.poll() is not None:
            tc.fail(f"the game exited early (code {proc.returncode})")
        time.sleep(0.05)


def press(arg):
    key, _, ms = arg.partition(":")
    gamectl.send(f"press {key} {ms or 150}")


def shot(path):
    reply = gamectl.send("screenshot " + os.path.abspath(path))
    if not reply.startswith("ok"):
        print(f"  screenshot failed: {reply}")
    return reply


def run_script(out, steps, hidden):
    os.makedirs(out, exist_ok=True)
    proc, log, t0 = launch(out, hidden, {"PS2X_PERF_LOG": os.path.abspath(os.path.join(out, "perf.csv"))})
    taken = []
    try:
        for seconds, action, arg, static in steps:
            wait_until(t0, seconds, proc)
            if action == "press":
                press(arg)
            elif action == "perf":
                with open(os.path.join(out, f"perf_{arg}.txt"), "w") as f:
                    f.write(gamectl.send("perf") + "\n")
            elif action == "shot":
                shot(os.path.join(out, arg + ".png"))
                taken.append({"scene": arg, "t": round(time.time() - t0, 1), "static": static})
                print(f"  {time.time() - t0:6.1f}s {arg}")
    finally:
        stop(proc, log)
    with open(os.path.join(out, "scenes.json"), "w") as f:
        json.dump({"exe": tc.VCS_EXE, "gs": os.environ.get("PS2X_GS", "hw"), "scenes": taken}, f, indent=1)
    return taken


def contact_sheet(paths, out_path, columns=6, width=320):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    if not paths:
        return
    thumbs = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        thumbs.append((os.path.splitext(os.path.basename(p))[0], im.resize((width, width * im.height // im.width))))
    h = max(t.height for _, t in thumbs) + 16
    rows = (len(thumbs) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * width, rows * h), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    for i, (name, t) in enumerate(thumbs):
        x, y = (i % columns) * width, (i // columns) * h
        sheet.paste(t, (x, y + 16))
        draw.text((x + 4, y + 2), name, fill=(230, 230, 230))
    sheet.save(out_path)


def cmd_explore(a):
    steps = []
    for item in filter(None, (a.press or "").split(",")):
        t, _, key = item.partition(":")
        steps.append((float(t), "press", key, False))
    t = a.every
    while t <= a.seconds:
        steps.append((t, "shot", f"t{t:05.1f}", False))
        t += a.every
    steps.sort(key=lambda s: (s[0], s[1] != "press"))
    run_script(a.out, steps, a.hidden)
    shots = sorted(os.path.join(a.out, f) for f in os.listdir(a.out) if f.startswith("t") and f.endswith(".png"))
    contact_sheet(shots, os.path.join(a.out, "sheet.png"))
    print("contact sheet: " + os.path.join(a.out, "sheet.png"))


def cmd_capture(a):
    if not SCRIPT:
        tc.fail("SCRIPT is empty")
    taken = run_script(a.out, SCRIPT, a.hidden)
    contact_sheet([os.path.join(a.out, s["scene"] + ".png") for s in taken], os.path.join(a.out, "sheet.png"))
    print(f"{len(taken)} scenes in {a.out}")


def compare_images(ref, new, diff_path):
    from PIL import Image, ImageChops, ImageStat
    a = Image.open(ref).convert("RGB")
    b = Image.open(new).convert("RGB")
    if a.size != b.size:
        b = b.resize(a.size)
    d = ImageChops.difference(a, b)
    mean = sum(ImageStat.Stat(d).mean) / 3.0
    big = sum(d.convert("L").histogram()[25:]) * 100.0 / (a.width * a.height)
    brightness = sum(ImageStat.Stat(b).mean) / 3.0
    d.point(lambda v: min(255, v * 4)).save(diff_path)
    return mean, big, brightness


def cmd_compare(a):
    try:
        import PIL  # noqa: F401
    except ImportError:
        tc.fail("compare needs Pillow (pip install pillow)")
    with open(os.path.join(a.ref, "scenes.json")) as f:
        scenes = json.load(f)["scenes"]
    out = a.out or os.path.join(a.new, "diff")
    os.makedirs(out, exist_ok=True)
    failed = 0
    print(f"{'scene':24} {'static':6} {'mean':>6} {'>24 %':>6} {'bright':>6}  result")
    for s in scenes:
        name = s["scene"]
        ref, new = os.path.join(a.ref, name + ".png"), os.path.join(a.new, name + ".png")
        if not os.path.isfile(new):
            print(f"{name:24} {'':6} {'':>6} {'':>6} {'':>6}  FAIL missing")
            failed += 1
            continue
        mean, big, bright = compare_images(ref, new, os.path.join(out, name + ".png"))
        ok = bright > 3.0 and (not s["static"] or mean <= STATIC_TOLERANCE)
        failed += not ok
        print(f"{name:24} {'yes' if s['static'] else 'no':6} {mean:6.2f} {big:6.2f} {bright:6.1f}  {'ok' if ok else 'FAIL'}")
    for f in sorted(os.listdir(a.ref)):
        if f.startswith("perf_") and f.endswith(".txt") and os.path.isfile(os.path.join(a.new, f)):
            print(f"\n{f[5:-4]}:\n  ref {open(os.path.join(a.ref, f)).read().strip()}\n  new {open(os.path.join(a.new, f)).read().strip()}")
    print(f"\ndiff images (x4): {out}")
    sys.exit(1 if failed else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("capture")
    p.add_argument("out")
    p.add_argument("--hidden", action="store_true")
    p = sub.add_parser("explore")
    p.add_argument("out")
    p.add_argument("--seconds", type=float, default=120.0)
    p.add_argument("--every", type=float, default=3.0)
    p.add_argument("--press", help="T:KEY[:MS],... key presses at those seconds")
    p.add_argument("--hidden", action="store_true")
    p = sub.add_parser("compare")
    p.add_argument("ref")
    p.add_argument("new")
    p.add_argument("--out")
    a = ap.parse_args()
    {"capture": cmd_capture, "explore": cmd_explore, "compare": cmd_compare}[a.cmd](a)


if __name__ == "__main__":
    main()

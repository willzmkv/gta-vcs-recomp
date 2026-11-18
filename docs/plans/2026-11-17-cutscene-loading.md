# Cutscene loading: the dark sky and the late Vic

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax. Read the fork's AGENTS.md (hooks: work before the
> original, never after; test runs hidden, on card copies) and `docs/game-internals.md` 6 (the renderer) and 11 (the
> script VM) first.

**Goal:** a cutscene's first frames look as the rest of it: the sky drawn from the first frame, the characters
present from the first frame. The player: "in the first frame of some cutscenes (plainly in the opening ones) the sky
is completely black, and after a while it turns coloured"; "in the first mission's cutscene where Vic meets a man at
the airport, Vic's model is not loaded in the first frame and just appears". Also seen in emulators: the game's own
behaviour, which the port can fix (an enhancement or a fix, the player's call; default on if it is clearly a defect).

---

## What is known (2026-11-17)

**Reproducing the dark sky:** a new game from an empty card, no button pressed: the title, then the intro's first
cutscene - Vic arriving at Fort Baxter, the camera looking up at palm trees against the sky. Recorded with the rig's
`burst` (every presented frame as a PNG; script `boot.py` below). In the burst `b0` of that run, frames 62-80 show the
palms over a **dark grey** sky (brightness 0 rising to 27 of 255), frame 82 jumps to a **bright cyan** sky (181).

**The timecycle** (`*0x486838`, the object at 0x4E7200 in that run; current values from +0x3200, traced every poll
with the frame counter):
- At the cutscene's start (frame ~1180-1208) the "extra colours" switch on: +0x3370 = 4 (the extra colour's index),
  +0x3374 = 1, +0x3378 = 1.0, a counter at +0x336C rising 1 every ~15 frames (VC's `CTimeCycle::m_ExtraColour`,
  `m_bExtraColourOn`, `m_ExtraColourInter`; set by the cutscene's load, `FUN_00426220` writes +0x88 / +0x8C of the
  cutscene manager from +0x3370 / +0x3374).
- The sky colours (+0x32BC.. top RGB, +0x32C8.. bottom RGB as ints) become `0x15 0x15 0x15` (top) / `0x81 0x7C 0x66`
  (bottom) at that moment and **stay so for the whole cutscene** - while the screen's sky goes bright cyan ~20 frames
  in. So the dark sky is these extra colours' gradient; the bright sky comes from something drawn **over** it later,
  not from the timecycle.

**Hypothesis:** the bright sky is an object of the cutscene (a sky model or the clouds / a backdrop) that is still
streaming in during the first frames - the same as Vic's model at the airport. On the PS2 the cutscene starts when its
models are *requested*, not when they are *loaded*.

**Addresses:** cutscene manager `*0x48F820` (+0x34: 1 loading, 2 loaded - HAS_CUTSCENE_LOADED tests 2, 0 cleared;
+0x30 = 1 started by StartCutscene `FUN_00428170`), the load `FUN_00426220`, `FUN_00427120` / `FUN_00427600`. The 2D
pass `FUN_0021F348` (sky gradient `0x3E0C6C`, horizon band `0x3E0D4C`, clouds `0x3A1260`; game-internals 6 "The 2D
pass"). Decompiled in `build/analysis/decomp/cutscene.c` (`tools/eedecomp.py 0x428170 0x427120 0x427600`).

---

## Task C1: what draws the bright sky

**Files:** scratch scripts in `build/analysis/cutscene/`; later `src/game/vcs_cutscene_load.cpp` (new) and
`docs/game-internals.md` (a new section on cutscene loading).

- [ ] **Step 1: two draw logs.** Run the game with `PS2X_HW_DUMP_KEY=1` (F12's draw log into the run log and
  `build/run/hw_display.ppm`) from an empty card; press F12 through the control channel
  (`gamectl.py press F12`) once in a dark-sky frame (~1 s after the cutscene starts) and once in a bright one (~3 s).
  Script: start like `boot.py` (below), find the cutscene's start by the manager's +0x30 (poll `peek`), then press.
- [ ] **Step 2: diff the two logs.** Which draws exist only in the bright frame (texture names, the draw's source:
  native world / lit model / 2D)? Expected: a model (sky dome or a backdrop) or the clouds.
- [ ] **Step 3: name it.** From the texture / model, find the object in the cutscene's object list (the `.CUT` file's
  models; the manager's object array) and when its model's streaming state turns loaded (`CStreaming` model info
  state: find the field with `tools/eedecomp.py` on `FUN_00426220`'s model requests).
- [ ] **Step 4: write down** what was found in `docs/game-internals.md`.

**Done when:** the object that brings the bright sky is named, with the frame its model loads in.

## Task C2: start the cutscene with its models loaded

- [ ] **Step 1:** find where the cutscene waits (or not) for its models: in VC, `CCutsceneMgr::LoadCutsceneData`
  ends with `CStreaming::LoadAllRequestedModels`; in VCS read `FUN_00426220`'s end and `FUN_00428170`.
- [ ] **Step 2 (fix, in the port):** before StartCutscene's first frame, finish the requested models' loading -
  a hook on `FUN_00428170` (StartCutscene) **before** the original that runs the game's own
  `LoadAllRequestedModels` as a guest tail call (never call a guest function from host code expecting it to finish:
  AGENTS.md rule 2 - see how `VCS_TEST_SAVE` chains a guest call). Our disc reads complete at once, so this costs a
  few frames' worth of decoding, not seconds.
- [ ] **Step 3:** if the models cannot be loaded at once, hold the fade (keep the screen black) until the manager's
  objects all have their models - the player's "crop the first frames" idea, as the fallback.
- [ ] **Step 4:** a switch back (`VCS_CUTSCENE_PRELOAD=0`) and a setting if it changes the original's timing
  (Enhancements > Cutscenes), per AGENTS.md "every enhancement is an option".

**Done when:** in the intro's first cutscene the first presented frame after the fade shows the bright sky
(`burst` brightness of the sky band >= 150 from the first non-black frame), with the switch off it is as before.

## Task C3: Vic at the airport

The first mission's cutscene where Vic meets a man at the airport: Vic's model missing in the first frame.

- [ ] **Step 1: reproduce.** From the Fort Baxter card (`build/rig/saves/fortbaxter`, the player's "2 Soldado" after
  the first mission), start the next mission with the cutscene (find its index: `rig.py scripts` while it runs, or
  the trainer's mission list `docs/TRAINER.md`), `burst` the first 60 frames.
- [ ] **Step 2:** the same cause as C1 (the player's model streamed late)? Check with C2's fix.
- [ ] **Step 3:** a check: `tools/cutscene_check.py` - a cutscene's first non-black frames compared with frame 30
  (sky band brightness, a character's pixels present).

**Done when:** Vic is in the first frame, checked by `tools/cutscene_check.py` on both cutscenes.

---

## The scripts used (rebuild them in `build/analysis/cutscene/`)

`boot.py` - from boot, no button, continuous bursts:

```python
import glob, os, sys, time
sys.path.insert(0, "tools")  # (from the repository's root)
import rig
out = sys.argv[1]; n = int(sys.argv[2])
g = rig.Game.start(save=None, name="sky_boot", seconds=900, env={"VCS_PERF_OVERLAY": "0"})
try:
    for k in range(n):
        d = os.path.join(out, "b%d" % k).replace("\\", "/"); os.makedirs(d, exist_ok=True)
        g.cmd("burst %s 300" % d)
        last = -1
        while True:                    # the burst done: no new file for 2 s
            time.sleep(2); c = len(glob.glob(d + "/*.png"))
            if c == last: break
            last = c
finally:
    g.quit()
```

The timecycle trace: poll `frames`, `peek <tc+0x32BC> 44` (sky colours), `peek <tc+0x3364> 28` (extra colour
fields) and a screenshot every few polls; `tc = u32(0x486838)`.

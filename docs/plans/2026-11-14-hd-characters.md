# HD Characters (cutscene hands on the gameplay models) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every gameplay character that has a cutscene model of its own - Vic's 14 outfits and the story characters -
gets its cutscene model's hands (fingers apart) on its own gameplay body, generated from the player's own disc as a
mod folder, every one of them checked to the pixel the way Vic's `plr2` was.

**Architecture:** One tool, `tools/hdchars.py`, does what the scratch scripts in `build/analysis/hdped/work/` did for
`plr2` (commits 8672de7 and the session of 2026-11-13; game-internals.md 12g): extract the gameplay model and its
cutscene twin from `GTA3PS2.IMG` (the cutscene model sits in the gap after `cs<name>.xtx`), keep the gameplay body,
fold its hands into the wrists, add the cutscene hands posed onto the gameplay skeleton with Rockstar's own finger
pose (`MOCAPPS2.IMG`, the character's own cutscene keyframes, else Vic's 43 / 43 degrees from UMBB1), weld the wrist
vertex for vertex onto the forearm's end (bare wrist) or inside the strap of an accessory (watch), copy the gameplay
hand's skin weights, and add the cutscene hand texture to the gameplay dictionary. Output: `modloader/HD Characters/`
(never in git: game data). Every character is then photographed and measured; any defect found means a new version.

**Tech Stack:** Python 3 stdlib (tools/), the mod loader's game files (docs/MODS.md), the rig for pictures.

**The reference result:** `modloader/HD Hands/plr2.mdl` v27 (approved by the player 2026-11-13: "the position of the
hands is excellent, the cut is perfect").

---

## Task 1: `tools/hdchars.py` - the converter as a tool

**Files:**
- Create: `tools/hdchars.py` (from `build/analysis/hdped/work/{pedskin,hybrid,hands,mocap}.py`: the `Ped` reader,
  the `Ldm` writer, `convert()` with the wrist weld, the grip, the weight transfer, the texture merge)
- Create: `tools/hdchars_check.py` (Task 3's checks)
- Modify: `AGENTS.md` (tool list), `docs/MODS.md` (a section "HD characters")

- [ ] **Step 1: Port** the four scripts into one module with the CLI
  `python tools/hdchars.py build [OUT] [--only plr2,lansm] [--iso ISO]` and `python tools/hdchars.py list` (the pairs
  found on the disc). Pairs: every `cs<name>.xtx` whose gap holds an `ldm`, paired with the gameplay model of the same
  name (`plr`..`plr14` <- `csplr`..`csplr14`; `lansm` <- `cslansm`; `phil` <- `csphil`; `jerry`, `lou`, `marty`,
  `arman`, `barry`, `bry`, `diaz`, `diego`, `reni`, `renop`, `loum`, `umber` <- `csumb`, `mengga` / `menggb` <-
  `csmeng1` / `csmeng2`; `list` prints the ones found and the ones without a gameplay twin).
- [ ] **Step 2: Generalise what was Vic-specific**, each found from the data, printed by `build`:
  - the hands' materials (the cutscene mesh weighted to the hand bones: `cs_vance_arm_tattoo` for Vic; by weights,
    not by name), the gameplay hand material(s) (> 80 % of vertices on hand bones);
  - accessories kept (`KEEP`): a gameplay material around a wrist that is not skin (the watch `ig_vance_dtag`) - by
    its texture differing from the forearm's and lying within 4 cm of the wrist joint; such a wrist is "strap" (the
    hand inside the strap), the other "bare" (the weld);
  - the grip: the character's own finger angles over the cutscenes in which it holds a gun prop (`.CUT` `model`
    lines with `cs_beretta`, `cs_python`, `cs_uzi`, `cs_shotgun`...); else Vic's 43 / 43.
- [ ] **Step 3: Same bytes for plr2**: `python tools/hdchars.py build build/hdchars --only plr2` and compare with
  `modloader/HD Hands/plr2.mdl` (v27; the right hand moved 1.75 cm, `inverse bind x -0.4581 -> -0.4406`, is a
  per-character override: `--shift plr2:24=0.0175`). Expected: identical files (`cmp`). Until identical, the port is
  wrong.
- [ ] **Step 4: Reviewer, commit** (`tools/hdchars.py`: no game data in it, only the method).

## Task 2: Vic's other outfits (`plr`, `plr3` .. `plr14`)

- [ ] **Step 1:** `python tools/hdchars.py build build/hdchars --only plr,plr3,...,plr14` - per outfit, the log's
  numbers: folded vertices, kept hand vertices, wrist rings (vertices, radius), welds, accessories found.
- [ ] **Step 2: Measure each** with Task 3's checks (no picture first): any outfit failing a check gets its fix before
  pictures.
- [ ] **Step 3: Pictures** of each outfit (Task 3, step 3), compared side by side with the original.
- [ ] **Step 4: The player's review** - pictures sent; any pixel out = a new version of that outfit (record what was
  wrong and the fix in docs/MODS.md's section).

## Task 3: The checks (what "pixel-perfect" means, measured)

**Files:** `tools/hdchars_check.py [OUT] --mods DIR --only NAMES`

- [ ] **Step 1: Geometry, no game run** (from `build/analysis/hdped/work/{handfit,wristprofile}.py`): per wrist, the
  outermost surface radius at 8 angles, every 1 mm from 1.6 cm above to 1 cm below the joint, original vs new: no step
  larger than 1.5 mm across the seam, no gap (a ray with no hit) anywhere; the new hand within 2 mm of the original's
  surface up to 1 cm past the joint; no new-hand vertex outside a watch's inner strap surface.
- [ ] **Step 2: Rig pictures**, for each character (Vic: the outfit set by `STORE_PLAYER_OUTFIT` + UNDRESS_CHAR;
  others: `CREATE_CHAR` of the model beside the camera): free camera 0.5 m from each hand (front, side, back), empty
  hands, a pistol, a rifle, and **aiming** (the virtual gamepad: `gamepad lt 1` with free aim on, rig `gamepad`
  command) - 1920x1080, crops at 3x of each wrist. Original model the same way (`--original`).
- [ ] **Step 3: Picture compare**: per crop, the seam's line (pixels within 3 px of the wrist ring's projection) must
  have no colour step larger than its neighbours' (a luminance gradient check), and the hand's silhouette against the
  weapon's must match the original's within 4 px (the grip).
- [ ] **Step 4:** writes `OUT/result.json` and an HTML-free contact sheet PNG per character for the player's review.

## Task 4: The story characters

- [ ] Same as Task 2 for every pair `list` prints; peds use their own skeleton (the bone ids are the game's: 24 / 34
  hands, 25 / 35 fingers) - a character whose cutscene skeleton lacks a bone the gameplay one has (or the other way)
  is printed and skipped until handled.
- [ ] The player's review per character.

## Task 5: Delivery

- [ ] `docs/MODS.md` "HD characters": what it is, `python tools/hdchars.py build` -> `modloader/HD Characters/`, the
  switch in F2 > Mods (live: the player is dressed again at once; other characters when they are next streamed).
- [ ] README "What this fork adds".

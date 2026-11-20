# HD World Visual Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The detailed city (HD world, `src/game/vcs_world_hd.cpp`) draws every building the game draws, in every
mission, at the right height - no building gone near it, none sunk into the ground far away; and the distant proxies
and free-camera pictures without their two known defects.

**Architecture:** The HD world builds the sector list the game draws each frame from the game's own list for its cell
(the "slot") plus the detailed lists of extra cells around (cached copies in `vcs::memory::kHdWorldBase`). Every defect
below is a record the build takes or leaves wrongly; each is found with `VCS_HD_PROBE=x,y,r` (what the build did with
every record near a point, per list, and why), reproduced by a probe script, fixed in the build's rules, and checked by
the same probe plus pictures against `VCS_HD_RADIUS=0` (the game's own world, the reference).

**Tech Stack:** C++ (src/game), Python 3 stdlib probes (tools/rig.py), the rig's free camera (`campose`).

**Rules:** AGENTS.md (hooks, no game data in git, test on card copies, reviewer before each commit). Probe scripts
live in `build/analysis/` (scratch) until one becomes a check, then it moves to `tools/`.

---

## Task 1: Phil's warehouse still vanishes "the first time" (Boomshine Blowout)

Known (game-internals.md 9h, commit c78f4a5): detailed sector 3167 and low-detail-id sector 6285 share a sphere at
-1210,-267; the mission hides 3167 and shows 6285. The fix keeps 6285 when the **slot's own list** hides 3167.
Suspected gap (the reviewer's note): when the player approaches from a side whose cell is not the slot, the record
comes from an **extra cell's cached list**, whose flags are the ones of caching time - 3167 shown, 6285 hidden there,
or 6285 replaced by the extra list's rule.

**Files:**
- Modify: `src/game/vcs_world_hd.cpp` (`build()`, the extra-cell loop after `takenElsewhere`, ~line 1375 on)
- Create: `build/analysis/hdped/work/warehouse_approach.py` (scratch probe)
- Modify: `docs/game-internals.md` 9h (the warehouse bullet)

- [ ] **Step 1: Reproduce.** Write `warehouse_approach.py` from `build/analysis/hdped/work/warehouse_probe.py`: start
  mission 47, reach state 4 (as that script does), then teleport the player 400 m away to the N, E, S and W of
  (-1210,-267), wait 8 s, and drive the free camera (`campose`) in a straight line to the warehouse at 15 m/s,
  screenshotting every 25 m and logging `VCS_HD_PROBE=-1210,-267,10` lines. Run it:
  `python build/analysis/hdped/work/warehouse_approach.py a`. Expected: in at least one approach, a build where neither
  3167 nor 6285 is drawn (`grep "hdprobe.*-1210,-267" build/run/warehouse.log`). If no approach reproduces it, repeat
  with the mission started from Phil's (not the rig's `mission(47)`) and with a fresh boot straight into the mission.
- [ ] **Step 2: Read the failing build's lines**: which list (game or `cell x,y`) holds each record, its flag, and the
  verdict (`taken already`, `coarser, replaced`, `HIDDEN (replaced)`).
- [ ] **Step 3: Fix by rule, not by id.** The game's hide flag is the truth for the sectors it lists: gather, from the
  slot's list, the ids the game hides (flag 0x8000) and the ids it shows; in the extra-cell loop, a record whose id the
  game lists takes the game's flag (hidden stays hidden, shown is shown), and the twin rule (`twin()`) also runs against
  spheres collected from every list where the game hides the detailed twin. Keep `VCS_HD_PROBE`'s verdict strings so the
  log says which rule applied (add "game's flag" as a verdict).
- [ ] **Step 4: Verify**: rerun step 1 - every build draws exactly one of 3167 / 6285; pictures at 60 / 25 m show the
  warehouse from all four approaches; `VCS_HD_RADIUS=0` pictures for comparison.
- [ ] **Step 5: Regression**: `python tools/scenes.py capture build/scenes/hdfix` and
  `python tools/scenes.py compare build/scenes/reference build/scenes/hdfix` - no scene loses a building.
- [ ] **Step 6: Reviewer** (a helper agent, medium effort) on the diff; fix its findings.
- [ ] **Step 7: Commit** `HD world: the game's own hide flags over the extra cells' cached ones` + docs.

## Task 2: Other missions that switch buildings

**Files:**
- Create: `build/analysis/hdped/work/switched_sectors.py` (scratch: the survey)
- Create (when worth keeping): `tools/hdworld_missions.py` (a check)
- Modify: `docs/game-internals.md` 9h (the list of switched pairs and their missions)

- [ ] **Step 1: Find every switched pair.** In a running game (any cell), walk every cell's list the HD world caches
  (add a control command `hdpairs` in `vcs_world_hd.cpp` that prints, for each list, pairs of records with the same
  centre halves and radius within 2 % where one id is detailed and one low-detail, and records whose flag differs
  between the game's list and our cached copy). Run it once per level (MAINLA, BEACH, MALL: teleport there).
- [ ] **Step 2: Map them to missions**: the switch state lives in `g_level + 0x418 + kind * 4` (`switchedOn()`); log
  writes to that range (`VCS_WATCH=lo,hi`) during a story playthrough from the trainer's mission list and note which
  mission flips which kind.
- [ ] **Step 3: Check each pair** with the probe of Task 1 at its sphere, in its mission (`g.mission(n)`), approaching
  from two sides. Expected after Task 1: one version drawn in every build.
- [ ] **Step 4: Keep the check**: `tools/hdworld_missions.py [OUT]` runs step 3 for the list found, writes
  `OUT/result.json`, exit 0 = pass. Add it to AGENTS.md's tool list.
- [ ] **Step 5: Reviewer, commit.**

## Task 3: Distant buildings sunk into the grass at sunset

Reported 2026-11-13 (screenshot 140002): Vic in an Admiral at (-888.7, -1145.2, 13.4), camera (-892.0, -1154.2, 14.9)
heading 339.9, 21:35: the buildings far to the NW stand inside the grass of a near slope - their bases hidden, the
grass cut into them.

**Files:**
- Create: `build/analysis/hdped/work/sunk_probe.py`
- Modify (by the finding): `src/game/vcs_world_hd.cpp` (`zShift`, the extra cells' records moved to the slot's cell),
  or the low-detail rule

- [ ] **Step 1: Reproduce**: `campose -892.02 -1154.21 14.91 <yaw> -4.9` (campose's yaw = heading + 90), time 21:35,
  screenshot with the HD world on, with `VCS_HD_RADIUS=0`, and with `world.view_distance` 0. Expected: the defect only
  with the HD world on (if also at 0, it is the game's own LOD and Task 3 ends documented as authentic).
- [ ] **Step 2: Find the records**: `VCS_HD_PROBE` at the sunk buildings' x,y (read them off the map from the camera
  ray: `tools/rig.py peekf` of the picked sector isn't available, so bisect with `keeplod` / per-cell hide), note their
  list and the `zShift` applied (`rdf(slot + 0x14)` in `build()`).
- [ ] **Step 3: Hypothesis to test first**: the extra cells' detailed records are drawn with the **slot's** z shift
  (`zShift`), not their own cell's - a building of a cell lower or higher than the slot's ends at the wrong height.
  Print the z shift of the slot and of each extra cell for the sunk records; if they differ, take each record's own
  cell's shift.
- [ ] **Step 4: Verify**: pictures at the spot (HD on vs radius 0: the bases match), a drive along the route of
  `tools/bench_avenue.py` with `VCS_HD_DRAWDIFF=1` (no new drops), `tools/scenes.py compare`.
- [ ] **Step 5: Reviewer, commit, docs** (game-internals.md 9h).

## Task 4: Missing parts of the distant proxies (walkers / far cars)

The walkers past 185 m and the far cars are drawn as their template's posed mesh; some templates show a sitting pose
or lose parts (legs, a car's wheels).

**Files:**
- Modify: `src/game/vcs_distant.cpp` (template choice: `keptTemplate`, `p.still`, `t.moving`)
- Modify: `src/game/vcs_hle_world.cpp` (the proxy draw of a template)

- [ ] **Step 1: Catalogue**: `VCS_DISTANT_CROWD_BAND=20,60` (walkers near, to look at) at the Little Havana spot, free
  camera 10 m from them; screenshot each template in use (`proxies` command lists them); list the bad ones and why
  (pose of a sitting / driving ped; a part drawn by another draw call than the template's).
- [ ] **Step 2: Fix the choice**: a template must be a walking ped (state 5, `pedMoving`) whose every part was drawn
  in the frame it was recorded; reject others and keep the previous template.
- [ ] **Step 3: Verify** with step 1's pictures, then at the normal band; reviewer; commit.

## Task 5: The bright glow in free-camera shots

**Files:** to be found (`src/render/`, the post-processing; the HD world's night glow `g_glow`).

- [ ] **Step 1: Reproduce**: a free-camera picture at the avenue spot at noon and at 21:00 vs the game's own camera
  at the same place; find the extra glow.
- [ ] **Step 2: Bisect**: `graphics.*` post settings off one at a time, `VCS_HD_RADIUS=0`, and the HD world's
  `g_glow` (night glow of low-detail sectors) off.
- [ ] **Step 3: Fix** where found (a free camera outside the game's camera planes, or a glow drawn twice); reviewer;
  commit.

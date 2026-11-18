# The HD world in cutscenes

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax. Read `PROJECT_STATUS.md` (the HD world handoff),
> `docs/game-internals.md` 9j (the world's streaming and the HD world) and 5a (cutscenes: the manager, widescreen)
> first; `src/game/vcs_world_hd.cpp`'s comments say why each rule exists. Hooks work before the original, never after.

**Goal:** an outdoor cutscene shows the detailed city as far as the player's Detailed city distance (`world.hd_radius`),
as gameplay does - today the city "shrinks" to the game's ~260 m the moment a cutscene starts and grows back when it
ends. Interiors stay as they are.

## What is known (2026-11-18)

- The HD world draws nothing of its own **while a cell is forced** (`level + 0x260` non-zero). The game forces a cell
  for interiors **and for cutscenes** (`.CUT` scenes stream the cutscene's place, not the player's):
  - `src/game/vcs_world_hd.cpp` `update()`: "nothing new while a cell is forced (level + 0x260: interiors, cutscenes)"
    - the cells stay loaded but none is added or dropped;
  - `install()` (at the world draw): `why = 1` - the frame drawn with the game's own list only (counted in the
    `[hdworld]` line's `fallback frames` 2nd number, and in `hdworld`'s counters);
  - the draw check skips forced-cell frames ("the forced slot's copies are not among those gatherGame knows").
- The cutscene manager: `*0x48F820` (+0x30 = 1 once StartCutscene `FUN_00428170` ran, +0x34: 1 loading, 2 loaded,
  0 cleared). Cutscenes switch the camera's widescreen on (`TheCamera + 0x844`). The cutscene camera can be far from
  the player: the cells must follow the **camera** (they already do: `kCameraPos`).
- Phil's warehouse (Boomshine Blowout, #32): in its cutscene the game shows **both** versions (3167 the shell, 6285
  the building with its inside) - the twin rule must keep doing what the game's flags say there.

## Task H3: draw the extra cells during outdoor cutscenes

**Files:** `src/game/vcs_world_hd.cpp` (`update()`, `install()`, the draw check), `docs/game-internals.md` 9j,
`PROJECT_STATUS.md`.

- [ ] **Step 1: what a cutscene's forced cell is.** Log `level + 0x260` (and the slot it names: `level + 0x248`,
  `+0x2C + k x 0xA0`) in three places: an interior (the safehouse), an outdoor cutscene (Shakedown's opening, mission
  50 from the Fort Baxter card - `tools/shakedown.py` starts it) and the intro's first cutscene (a new game). Is the
  forced cell the cutscene's place, and is the current slot's list (the game's records `build()` starts from) the
  forced cell's?
- [ ] **Step 2: tell them apart.** Outdoor cutscene = the cutscene manager running (`*0x48F820 + 0x30` == 1) and the
  area 0 (outdoors: `currArea`, game-internals 9j / the rig's `area`). Interiors (area != 0) keep today's behaviour.
- [ ] **Step 3: build around the forced cell.** In an outdoor cutscene, let `update()` add / drop cells around the
  camera (not the forced cell itself) and let `install()` put the built list in, with the forced slot's list as "the
  game's records". Keep every other rule: a neighbour's low-detail twin never drawn (4b09d33), the twin rule for the
  game's own list (the warehouse), the fades. Hold the change for the cutscene's first frames if cells are still
  loading (no pop-in on the first frame: the cutscene-loading task's fix starts the scene with its models loaded).
- [ ] **Step 4: a switch back** (`VCS_HD_CUTSCENES=0`) and a mention in the setting's help (Graphics > Distance and
  population > Detailed city distance: "also in cutscenes").
- [ ] **Step 5: checks.**
  - Pictures at radius 3 vs 0 in Shakedown's opening cutscene and the intro's first cutscene: the far city present at
    3, the near picture identical (the same frame: `burst` from the cutscene's start).
  - `tools/shakedown.py` and `tools/haze_check.py` (Purple Haze's cutscene) pass with `VCS_HD_RADIUS=3`.
  - Boomshine Blowout's cutscene (mission 47): the warehouse as the game shows it (both versions), radius 3 vs 0.
  - `cruise.py` draw checks still 0 (`VU1 reserved`, `list record`, `hdworld-draw`); the `[hdworld]` line's forced-cell
    fallback count drops to the interiors' frames only.
  - An interior (the safehouse) unchanged.

**Done when:** outdoor cutscenes show the detailed city at the player's radius, matching radius 0 near the camera;
interiors unchanged; the checks above pass; `VCS_HD_CUTSCENES=0` brings back today's behaviour.

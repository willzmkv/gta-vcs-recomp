# HD world further - status (branch hd-world-impl)

Handoff written 2026-11-11 (night). Read this, then docs/game-internals.md 9j (everything learnt about the world's
streaming, with addresses), then `src/game/vcs_world_hd.cpp` (its comments say why each part exists).

## Goal

The detailed city (the world's sector geometry: buildings, streets, props, vegetation) reaches ~260 m around the camera
in the original; beyond it the game draws a low-detail image of the map. Goal: the detailed city further, authentic by
default. Setting: Graphics > Distance and population (experimental) > **Detailed city distance**: Original (~260 m) /
~450 / ~580 / ~720 m (`world.hd_radius`, env `VCS_HD_RADIUS`, live). Default stays Original. The player plays at ~720 m.

## State (the player's verdict, 2026-11-11: "quase tudo perfeito")

Works: the far city at ~720 m, no VU1 garbage, no full-screen textures, no noise textures, no flickering foliage,
the detail fading in / out on the horizon, ~145 fps uncapped on the cruise route (Original ~144), measured on an idle
machine (runs while the player compiled shaders elsewhere showed 77-93 fps - not comparable).

### Open (the player's last report) - the work list

1. **Lamp-post coronas** - done (src/game/vcs_world_hd_coronas.cpp, game-internals.md 9k): the far lamp dummies are
   projected and drawn through the game's own corona pass. Pictures at the avenue at 23:00 show the far glow; not yet
   checked by the player. Possible follow-ups: a minimum on-screen size for very far lamps (Project2DFX-like), other
   light types (2dfx type != 1/3: traffic lights, neon) left as they are.
2. **Some buildings appear only very close** - especially Little Havana near the Pay 'n' Spray. Check whether they are
   sectors at all (`probe_view.py` with the live game, below) or entities (CRenderer / IPL instances - vcs_distance.cpp's
   LOD scale). If sectors: which cell's list names them, whether `taken` or the low-detail rule leaves them out, whether
   their cell is loaded (nearest first, one a frame, within `kReach`).
3. **Trees "sprout" far away** - vegetation popping in at distance. The translucent buckets (6-7: leaves, bushes) are
   deliberately not faded (a lower alpha on them made the game's own near foliage flicker; game-internals.md 9j,
   "Fades"). Ideas: fade them another way (dithered, or an alpha that does not touch the state shared with the game's
   entries), or only at the very edge.
4. **Parked cars / the ambulance** - done (game-internals.md 9l): the generators' 160 / 170 m follow Population
   distance (instruction patches in overrides.toml and vcs.toml, the values written by vcs_distance.cpp). Not yet
   checked by the player.
5. Not tested with the setting on: interiors, save / load, missions, a soak (`tools/soak.py`), the bridge at speed.
6. Clean-up: done 2026-11-12 (the previous agent's scripts and old test output here and in D:\Games\VCS Recomp; its
   two old agent worktrees removed). The previous agent's settings `world.hd_focus_*`, `world.preload_*`,
   `world.rsl_*` (still in vcs.ini) do nothing.

## How it works (src/game/vcs_world_hd.cpp)

1. Cells around the camera (not the current one) are read from the disc image (`vcs::IsoImage`) into the port's memory
   (arena at `0x4000000`, 48 MB; EE RAM is 128 MB), with their area and conditional pieces, relocated as the game does
   (FUN_00324AC0); their {gid, geometry} pairs entered into the level's geometry table **only where the entry is empty
   or stale** (the game's own entries are never taken; ours removed only while still ours).
2. At the world draw (FUN_0023E748) the current slot's sector list is replaced by a built one: the game's records, plus
   every detailed sector (id < 4000) of every loaded extra list once, with the low-detail sectors (id >= 5500) left out
   where a list leaves them out for its own detail (the rule the lists were made by). Same for the previous slot while
   the game cross-fades. The game's list is put back at the entry of every streaming function.
3. Memory: the sector packet heap (0x6F3DB8) rebuilt at `0x7000000` with 8 MB (src/game/vcs_memory.cpp); the
   translucent list moved to an 8192-entry buffer while on (it lives inside the level object, unbounded).
4. Fades: an extra cell's opaque sectors beyond 250 m come in through the game's translucent list (FUN_0023E548, as its
   cross-fade does) with the alpha rising over 1 s; the low detail they replace stays until then; a cell out of reach
   fades out before it is dropped.

### What broke, and what each taught (all fixed; game-internals.md 9j "What broke drawing more lists")

| Symptom | Cause | Fix |
|---|---|---|
| VU1 garbage, full-screen textures, <10 fps | translucent list overflowing inside the level object | moved to our buffer |
| same | stale table entries (the game never clears an area's entries) | good = ours or what the game's current / next slots hold (every copy of a gid) |
| same | cached packets orphaned by the cross-fade swap | swept on release |
| same | a sector's cached packet CALLs its textures' cached packets, freed by the game with their area | every packet put on the free ring collected; its callers freed before the next draw |
| purple / green noise blocks, leaves changing | cached packets REF texture data in a slot buffer the game starts reading the next cell into | packets pointing into a buffer that starts a read are freed |
| sectors missing | only one game copy of a gid counted; a failed cell load marked empty for good | every copy counts; failed loads retried after 5 s |
| foliage flickering near the camera | fading translucent entries (lower alpha) disturbed the game's own foliage | only opaque buckets fade |
| signs / building parts vanishing on the way in | a 200 m "nothing extra near the camera" rule (a wrong theory) | reverted |

## Tools and how to investigate

- Correctness on the hidden cruise route: `python tools/cruise.py run --name N --fps 0 build/cruise/N VCS_HD_RADIUS=3
  VCS_HD_DRAWCHECK=1`, then in `build/run/N.log`: `grep -c "VU1 reserved"` (garbage), `grep -c "list record"`
  (refused records), `grep -c hdworld-draw` (packets reading a loading buffer) - all must be 0. `perf_report.py
  build/cruise/N/perf.csv` for fps. (`out` is a path: a bare name lands in the repo root.)
- With the player's game open (`python tools/run.py 0 NAME --control`, log in `build/run/NAME.log`):
  `python tools/gamectl.py gameinfo` (position, camera), `gamectl.py screenshot F.png`, `hdworld` (counters: cells,
  added, hidden, fading, lod_held, ...), `peek`, `dump PATH` (all 128 MB of guest RAM at once), `burst DIR N` (the next
  N presented frames as PNGs), `press F12` (a frame's draw log into the run log + `build/run/hw_display.ppm`). Never
  kill the player's game; never run tests while the player plays (and they can't relink while it runs).
- build/analysis/hd_world (ignored by git): `probe_view.py LO HI [DMIN DMAX]` (sectors of our built lists in a bearing
  range from the live camera, with geometry and texture entries; slow over `peek`, ~1 min - the player must stand
  still), `ramcheck.py RAM.bin` / `pktcheck.py RAM.bin` (offline checks of a `dump`), `flicker.py NAME` +
  `flickmap.py DIR [OUT]` (fixed camera, bursts per radius, pixels that change and change back), `shot_purple.py` (the
  player's Little Havana spot; env TIME, WEATHER, DRIVE, REPS, VCS_SCREENSHOT_DIR for F8 window shots with
  `PS2X_HIDDEN_WINDOW=3840x2160`), `shot_islands.py`, `shot_fade.py`, `crop.py`, `ppm2png.py`, `purple.py`.
- Debug switches (docs/PROGRESS.md table): `VCS_HD_DRAWCHECK`, `VCS_HD_DRAWDIFF` (sectors missing for one frame, faded
  draws), `VCS_HD_PKTDEBUG` (ring pushes / frees / CALL targets), `VCS_HD_FADE=<s>` (0 off), `VCS_HD_NODRAW`,
  `VCS_HD_DRAWMODE=1|2|3`, `VCS_HD_FREE_DELAY`; runtime `PS2X_VIF_CHECK=1` (bad VIFcodes with their DMA tags).
- The method that worked: reproduce hidden with a fixed camera, bisect with switches, confirm with a counter that must
  be 0, then ask the player. The player's own sessions find what hidden runs miss (night, weather, their save, 4K).

## Rules (AGENTS.md, and learnt here)

Never commit game code or data; analysis dumps to build/analysis. Use the Edit tool for C++ string literals: shell
heredocs turn `\n` into real newlines (it happened four times on this branch). Hooks work before the original, never
after it. Runtime changes get `[gta-recomp]` and a PS2RECOMP_PATCHES.md row. The player speaks pt-BR.

# HD world further - status (branch hd-world-impl)

Revised 2026-11-11 after a review of everything the previous agent did on this branch. The earlier version of this
file described a "Phase 1-8" roadmap and several results that the code does not support; the corrections are below.

## Goal

The detailed city (buildings, streets, props of the world's sector geometry) reaches ~260 m around the camera; beyond
it the game draws its low-detail image of the map. Goal: the detailed city further, with the original kept as the
reference (setting at 0 = untouched game path).

## Review of the previous work (what was claimed vs what the code does)

| Claim | Reality |
|---|---|
| Phase 3A "Preload ahead": 15x frame time improvement (548 -> 37 ms) | `vcs_world.cpp` `chooseHook` only **logs** "bias would be ..."; nothing is changed. No measurement of 548/37 ms exists in the repo. |
| Settings `world.hd_radius`, `hd_focus_*`, `preload_*`, `rsl_cells_radius`, `rsl_stream_budget_mb`, `rsl_crossfade` | 8 menu entries, **all no-ops** (empty `onChange`, or read by stubs). Why moving sliders showed nothing. |
| Phase 3B Option C: `HostCellManager` implemented | Stub: `loadCell` / `submitDraws` are TODO comments; `computeVisibleCells` inserted map entries every frame and never removed them. |
| Phase 3B Option D: piece cache hook (uncommitted) | Checked the piece magic against `0x006D646C` ("ldm") - the index entries are `'DLRW'` (`0x57524C44`), so it **never cached anything**. Had it matched: a world piece is not an LDM model (wrong decoder), the ISO was opened and read synchronously inside a guest hook, draws were submitted with zero constants (no transform), the index list was submitted as a triangle strip without indices, and a 64-bit hash of every vertex was computed per batch per frame. Saved as `build/analysis/hd_world/optionD_piececache_abandoned.patch` and reverted. |
| "Crashes when leaving the safehouse" | Not reproduced by any test here; the tests ran at Fort Baxter (no city in view) - the locations could not show any result either. |
| `vcs_ldm.cpp` "decoder for cell pieces" | A model (`.mdl`) decoder; cell pieces are WRLD sector data. Kept, unused. |

Root causes: no reverse engineering of how a cell's list and its geometry relate before writing code; no visible test.

## What the game actually does (decompiled + measured, docs/game-internals.md 9j)

- Three cell slots (`level + 0x248` current, `+0x250` previous/next, `+0x258` loading) and three area slots
  (`+0x24C/0x254/0x25C`). A cell slot: `+8/+0xC` cell, `+0x14` z shift, `+0x18` complete, `+0x1C` piece count,
  `+0x20..` pieces (the cell piece = its sector list, then its conditional pieces).
- Every piece starts with `{gid, geometry}` pairs entered into the level's geometry table (0xC per gid: geometry, cached
  packet, owner). The draw (`FUN_00240ED8`) draws a record when its gid's table entry has geometry.
- **A cell's list names only its own cell's, its own area's and the always-resident low-detail geometry** (offline over
  the disc: police cell 14,25 = 47 own-cell, 53 own-area, 386 resident records; no other cell's geometry anywhere).
  So loading more pieces alone shows nothing: more *lists* must be drawn.
- The world draw already merges two lists during the cross-fade (`FUN_0023E748`): the mechanism exists in the game.
- Read watch on the list pointer: only the world draw and the streaming functions (`0x23C...-0x2413..`) read it.

## The implementation now (src/game/vcs_world_hd.cpp)

1. Cells around the camera (not the current one) read from the disc image into the port's memory (`0x4000000`, 48 MB;
   host EE RAM raised 64 -> 128 MB), with their area and conditional pieces; relocated as the game does
   (`FUN_00324AC0`); their pairs entered into the table **only where the entry is empty** (the game's entries never
   touched; ours removed only while still ours; cached packets freed through the game's own deferred ring).
2. At the world draw, the current slot's list replaced by a built one, by the rule the game's lists are made by:
   every detailed sector (id < 3500) of every loaded list once; a low-detail sector (id >= 5500) left out where a
   loaded list leaves it out and has detailed sectors inside it. Same for the previous slot during the cross-fade.
3. The game's list put back at the entry of every streaming function (chooser, area-for-position, swap, drop, flush);
   everything dropped before the level's data is read again (`FUN_0023BAF0`: the islands share one level object and
   one table - the bridge reloads it).
4. The sector packet heap (`0x6F3DB8`, 512 KB) rebuilt in the upper memory with 8 MB.
5. Setting: Graphics > Distance and population (experimental) > **Detailed city distance**: Original / ~450 / ~580 /
   ~720 m (`world.hd_radius`, `VCS_HD_RADIUS`, live). `hdworld` control command for the numbers.

## Results (2026-11-11, quick build, RX 7700 XT)

| | Original | ~720 m |
|---|---|---|
| Avenue spot: sectors drawn | 229 | 439 (+629 records, 110 low-detail hidden) |
| Extra memory | - | 24-33 MB |
| Hooks' own time per frame | - | ~10 us; 1.5-2.6 ms in a frame that loads a cell / rebuilds |
| Cruise route (138 s, 144 km/h, uncapped) | 138 fps, p95 12.6 ms | 129 fps, p95 12.7 ms; no hang |
| Packet buffer peak (cap 0xE6670) | 57 KB | 89 KB; nothing skipped |

Bugs found on the way (fixed): the neighbours' far low-detail sectors drawn as if detail (near buildings in low
quality - the user's report); a sphere rule that cut large ground sectors (dark holes); the list rebuilt every frame;
the island switch reloading the table under our entries (the world nearly empty, then the game spinning ~1 s a frame
on the cruise route).

## Status 2026-11-11 evening - NOT ready to play with the setting on

The player's session (`build/run/jogo_hd.log`): full-screen textures, the world "unloading", under 10 fps at ~720 m.
The log had 216k `[VU1 reserved lower]` lines - the VU1 executing garbage. The hidden cruise route reproduces it
(it had some in every ~720 m run, none at Original; earlier runs were not checked for it - a miss). Measure now:
`grep -c "VU1 reserved" build/run/<name>.log` after `tools/cruise.py run ... VCS_HD_RADIUS=3`.

Found and fixed (each confirmed by the counts): stale game entries for textures (taken back), cached packets orphaned
by the cross-fade swap (swept), the game's copies of the slot being read into (not trusted), the level reload handing
back torn-down pointers, the translucent list overflowing inside the level object (moved). Bisection: cells read and
entered without drawing - 0 in two runs; the game's records through our list - 0 in two runs; plus the extra detailed
sectors - garbage. The copies are byte-identical to the game's (`hdcompare`), the microcode uploads all come from the
executable (`PS2X_LOG_MPG_SOURCES=1`). Still: 0 / 2015 / 986087 / 2450 in the last four runs - something else that
drawing extra detailed sectors overflows or leaves stale. Next: dump the VIF1 chain of the first garbage frame
(`PS2X_DUMP_CHAIN`) to see which packet carries it.

Debug switches (vcs_world_hd.cpp): `VCS_HD_NODRAW=1`, `VCS_HD_DRAWMODE=1|2|3`, `VCS_HD_FREE_DELAY=<frames>`; the
`hdcompare` command; `[hdworld]` log line every 10 s; `GAME-list` / `built-list` lines for entries refused at the draw.

## Test spot

`tools/spots.py` **avenue** (the user's spot): the police spot ~15 steps west, facing south - the whole avenue in view.
Daytime (13:00). Script: `build/analysis/hd_world/shot_avenue.py NAME --hd 0 --hd 3`.

## Plan

1. The player's look in game (radius 1-3), with Fog at 100 % (this worktree's vcs.ini has `fog.distance=30`, which
   swallows the city past ~250 m).
2. Interiors, save / load, missions, a soak (soak.py) with the setting on.
3. Hitches while cells load: spread the copy (one piece a frame now, up to 0.9 MB).
4. Then: entities/props of the far cells (they are CRenderer's, not the sector lists - `vcs_distance.cpp`).
5. Clean-up of the branch: the root `apply_patch*.py` / `patch_*.py` / `final_patch*.py` scripts and `60/` are the
   previous agent's leftovers (untracked) - to delete once the user agrees.

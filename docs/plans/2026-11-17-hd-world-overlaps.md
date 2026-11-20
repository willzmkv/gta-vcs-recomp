# HD world: a second version of a building near the camera

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax. Read `PROJECT_STATUS.md` (the HD world handoff) and
> `docs/game-internals.md` 9j in the fork first; `src/game/vcs_world_hd.cpp`'s comments say why each rule exists.
> Never commit game data; probes in `build/analysis/hd_world/`.

**Goal:** with Detailed city distance up (`world.hd_radius` 3, the player's setting), nothing near the camera is drawn
twice - no light triangles over walls and pavements, no doors cut, nothing flickering with the camera - and the
frame rate where it happened recovers (the doubled sectors cost draws).

## What was fixed (4b09d33, 2026-11-17)

A neighbour cell's **low-detail twin** of a building the slot draws in detail (the same sphere: centre halves equal,
radius within 2 %) was drawn over it: the twin rule (kept for Phil's warehouse, issue #1) never replaced it, and an
extra list's records are drawn shown whatever their flag. Found by bisecting with `VCS_HD_DRAWMODE` at the player's
spot (-1188,-1214 at 00:30: 1 and 2 clean, 3 not) and `VCS_HD_TWINS=1` (5976 over 2519 at -1217,-1154; 5952 over 2800
at -986,-1153). A neighbour's twin is now never drawn.

The player (same day): "much better, but some spots of the map still show it, on a smaller scale".

## Tools

- `tools/hd_overlap_scan.py` - radius 3 vs 0 pictures in four directions at points of a grid or along routes
  (`--route havana_downtown.txt --route city.txt --step 150`), the lower half compared; `worst.txt` ranks the views.
  First run (night, the two city routes): 200 views at 50 points, no second version found (the worst were traffic,
  the radar, a player moved into the sea). Note: `world.vehicle_density 0` is the game's own traffic, not none.
- `VCS_HD_DUPES=1` - every 10 s, detailed sectors drawn twice under two ids (`[hddupe]`): 2 found on the mainland
  (2562 / 2527 at -1435,-826,44 r 103; 55 / 3193, both gid 71, at -1217,-340).
- `VCS_HD_TWINS=1` - once a minute, the twins kept and replaced (`[hdtwin]`).
- `VCS_HD_PROBE=x,y,r` - what the build did with every record near a point, per list, and why.
- `VCS_HD_DRAWMODE=1|2|3` - the game's records only / + the extra detailed ones / everything (bisecting).

---

## Task H1: the spots the player still sees

- [x] **Step 1: collect the spots & wide scan.** `tools/hd_overlap_scan.py --step 200` updated to keep `world.hd_radius 0` until landing is confirmed (0.5 s + 0.25 s freefall check) before enabling radius 3; scanned mainland at night and noon, flagging Downtown `(-975, 475)` yaw 0 (missing translucent blue glass on the construction building).
- [x] **Step 2: reproduce** at `(-975, 475)` at noon/afternoon/night (`hd_overlap_scan.py --at=-975,475` and `campose`): confirmed only the blue glass column/windows vanished under `world.hd_radius 3`.
- [x] **Step 3: bisect** with `VCS_HD_DRAWMODE` (1 and 2 showed the glass; 3 lost it), `VCS_HD_KEEPDUPES=1` (restored it), and `VCS_HD_PROBE=-975,475`: extra cell `12,23` holds detailed `914` (`gids 1282, 1283`, `r 31`) and `915` (`gids 1284, 1285`, `r 23`) across buckets `4` (opaque) and `7` (translucent glass), while low-detail `6039, 6040, 6041` were replaced. Because `r >= 20`, `sameSphere()` dropped `bkt7` on the second bucket of the same `id`, while `[hdprobe]` still logged `added`. (`VCS_WATCH` and disassembly also ruled out `0x8000` guest writes to `b.buffers` / `FUN_00241278`.)
- [x] **Step 4: fix the rule** in `build()` (`src/game/vcs_world_hd.cpp`): skip `sameSphere()` when `it->second.id == id` (with Rule-1 fallback switch `VCS_HD_SAME_ID_DUPES=1`), respect `0x8000` (`isHidden(rec)`) when populating `detailed`, and log `SKIPPED (twin)` / `SKIPPED (dupe)` / `SKIPPED (skipextra)` in `[hdprobe]`. Verified `(-975, 475)` at `12:00`, `16:00`, `22:00` (`R=3` vs `R=0` and `VCS_HD_SAME_ID_DUPES=1` fallback) and `tools/hdworld_missions.py` (`6/6` mission building switches PASS, including Phil's warehouse).
- [x] **Step 5: wider scan:** `hd_overlap_scan.py --step 200` night & noon ran with ocean/freefall skip (596 views across 149 land points each); `(-975, 475)` Downtown construction glass was investigated and fixed.
- [x] **Step 6:** `cruise.py run build/cruise/agent36 VCS_HD_RADIUS=3 VCS_HD_DRAWCHECK=1`: `VU1 reserved: 0`, `list record: 0`, `hdworld-draw: 0`.

**Done when:** the player's spots match radius 0 near the camera, the wide scan's worst views are all explained by
traffic / radar / position, the draw checks are 0.

## Task H2: detailed sectors drawn twice under two ids

`VCS_HD_DUPES` found two in the game's own slot list: `2562 (gid 3151) / 2527 (gid 3088)` at `(-1435,-826,44 r 103.0 / 102.6)` and `55 / 3193 (gid 71)` at `(-1217,-340,16 r 46.2 / 46.2)`.

- [x] Inspect both spots with `VCS_HD_DUPES=1` and `VCS_HD_PROBE`: both pairs arrive already with `flag 0` in the game's own slot list (`(game list)`), matching `R=0` (especially `2562/2527`, which carry distinct gids).
- [x] Decision (Option B): keep the game's own slot list untouched, suppress duplicate candidates from `extra` cells via `sameSphere()` (so `[hdprobe]` reports `SKIPPED (dupe)`), filter `isHidden(rec)` (`0x8000`) in `[hddupe]` and `detailed`, and tag `[hddupe]` lines as `(game list)` / `(game+extra)` / `(extra)` with `(game: %u, extra: %u)` summary.
- [x] `VCS_HD_DUPES=1` on `cruise.py`: `0 twice under two ids (game: 0, extra: 0)` across all 14 samples along the route.

**Done when:** `[hddupe] ... 0 twice under two ids` along the cruise route and Option B behaviour verified at both spots.

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

- [ ] **Step 1: collect the spots.** Ask the player for F3 positions (the panel's `pos` and `CAMERA` lines) and the
  hour when they see it; or a screenshot with the panel on. Note them in this plan.
- [ ] **Step 2: reproduce each** with the free camera at that position and hour (`campose x y z yaw pitch`, yaw from
  +x toward +y: convert the panel's heading), radius 3 vs 0 pictures (`hd_overlap_scan.py --at=X,Y` gives four
  directions; add the exact camera for the player's view).
- [ ] **Step 3: bisect** with `VCS_HD_DRAWMODE` (1/2/3) and name the sector pair with `VCS_HD_PROBE` and
  `VCS_HD_TWINS` / `VCS_HD_DUPES`.
- [ ] **Step 4: fix the rule** in `build()` (`src/game/vcs_world_hd.cpp`), keeping Phil's warehouse (issue #1's
  checks) and the twins of the game's own list as they are.
- [ ] **Step 5: run the scan wider**: `hd_overlap_scan.py --step 200` over the whole map (slow: ~1 h; skip the sea
  faster - the current version waits 5 s at every sea point) at night and at noon; the worst views looked at.
- [ ] **Step 6:** `cruise.py run ... VCS_HD_RADIUS=3 VCS_HD_DRAWCHECK=1`: `VU1 reserved`, `list record`,
  `hdworld-draw` all 0 (PROJECT_STATUS.md "Tools").

**Done when:** the player's spots match radius 0 near the camera, the wide scan's worst views are all explained by
traffic / radar / position, the draw checks are 0.

## Task H2: detailed sectors drawn twice under two ids

`VCS_HD_DUPES` found two: 2562 / 2527 (-1435,-826, r 103: a large sector, maybe ground) and 55 / 3193 (gid 71 both:
the very same geometry twice, -1217,-340 - Fort Baxter / the airport road).

- [ ] Look at each pair: pictures at the spot with the build as is and with one of the two left out
  (`VCS_HD_SKIPEXTRA=<id>` exists as `g_skipExtra`: check its env name in the source).
- [ ] If they are the same building: take each id from one list only (the slot's when it lists it, else the nearest
  cell's - the rule `takenElsewhere` already applies per id; extend it to "the same gid" or "the same sphere").
- [ ] `VCS_HD_DUPES=1` on a cruise route: 0 after the fix.

**Done when:** `[hddupe] ... 0 twice under two ids` along the cruise route and at both spots.

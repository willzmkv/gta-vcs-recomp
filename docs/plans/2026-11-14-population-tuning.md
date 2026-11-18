# Population Tuning and Crowd Bands by Default Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every population slider does what it says standing still as well as driving, the crowd bands hold their
numbers standing still, and - once a quiet-machine benchmark confirms the gain - the bands are the default with the
raised population (docs/CROWD_BANDS_PLAN.md step 5).

**Architecture:** `src/game/vcs_distance.cpp` sets the game's population numbers and caps from the sliders
(`applyDensity`), the spawn rings (`bandRings`, `addToPopulation`, `pedSpotFinder`) and trims the crowd (`trimBands`).
Measurements come from `popview` / `distancestate` (control commands) through probe scripts and
`tools/bench_avenue.py` (the player's benchmark).

**Tech Stack:** C++ (src/game), Python 3 stdlib probes.

---

## Task 1: Vehicle density does nothing standing still

Measured 2026-11-13 (slider probe at Little Havana, bands on): Vehicle density 0 / 50 -> the number 6.3 / 15.75 and
cap 52, but ~35 cars either way.

**Files:**
- Create: `build/analysis/hdped/work/cars_probe.py` (scratch)
- Modify: `src/game/vcs_distance.cpp` (by the finding), `docs/game-internals.md` 9h

- [ ] **Step 1: Count by band and state**: at the Little Havana spot and the police spot, standing, Vehicle density
  0 / 50 / 100 for 90 s each: cars by distance (0-60, 60-160, 160-300 m), parked vs moving, random vs mission, and the
  game's car count / cap words (`distancestate`). And the same driving (`tools/cruise.py` route).
- [ ] **Step 2: Find the limit**: candidates - the road nodes' density class (`applyRoadDensity`), the car spawn rings
  (`FUN_00430290` / `FUN_00433378` and the 100-160 m ring), parked cars counted in the 35, the frame budget
  (`VCS_POPULATION_FPS=0` in the probe). Change one at a time in the probe (env switches exist for most:
  `VCS_POP_CAPS`).
- [ ] **Step 3: Fix** so that standing at density 100 holds clearly more moving cars than at 0 within the spawn
  ring; measure; reviewer; commit.

## Task 2: Crowd bands standing still (near under, middle over)

Measured 2026-11-13 (density 50, standing): near 13-21 of 25, middle 28-37 of 25.

**Files:** `src/game/vcs_distance.cpp` (`bandRings`, `trimBands`, `kBandMidFade`)

- [ ] **Step 1: Why near stays under**: log for 2 minutes standing - each spawn try's ring, whether the finder found a
  spot, the band it landed in; and each fade's band. Expected cause: the room rule (no spawn while near + middle are at
  the total) blocks near while the middle is over.
- [ ] **Step 2: Fix**: room per band (near spawns while near is under its own number, whatever the middle holds), the
  middle's excess faded first (already: `midOver`); re-measure standing (both bands within +-3 of their number after 2
  minutes) and driving (`drive_probe.py`: unchanged).
- [ ] **Step 3: Reviewer, commit.**

## Task 3: The quiet-machine benchmark (the player's machine, nothing else running)

- [ ] The player closes Premiere / OBS; run
  `python tools/bench_avenue.py build/bench/bands_quiet --rounds 3 VCS_CROWD_BANDS=0,1`
  (alternating rounds). Record game thread, frame, p99, frames > 25 ms per variant in PERFORMANCE.md 49.
- [ ] Decision: bands on by default only if p99 and frames > 25 ms are not worse than off (the busy-machine run had
  them doubled).

## Task 4: Bands on by default (CROWD_BANDS_PLAN.md step 5)

**Files:** `src/game/vcs_distance.cpp` (`world.crowd_bands` default 1), `docs/CROWD_BANDS_PLAN.md`,
`docs/PROGRESS.md`, `AGENTS.md` (the "Decorative crowds" line about the >= 320-350 m rule: walkers start at 205 m with
the bands), `docs/DECISIONS.md`.

- [ ] Default on; env `VCS_CROWD_BANDS=0` keeps the old behaviour; docs; reviewer; commit.

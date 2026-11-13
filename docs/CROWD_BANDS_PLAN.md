# Crowd bands: few real peds near, a cheap crowd far, handed over at 200 m

The player's plan (2026-11-13), after measuring that the raised population costs most of the frame:

| Band | What stands there | How many |
|---|---|---|
| **Near**, 0-100 m | real peds (the game's AI, physics, animation, draw) on the player's pavement and the one across | ~15-20 |
| **Middle**, 100-200 m | real peds; past 150 m drawn as instanced proxies (PERFORMANCE.md 45-47) | ~10 each side of the avenue (~20) |
| **Far**, 200 m and out | decorative walkers (vcs_distant.cpp: host-simulated on the pavement links, smooth, instanced) | as many as the view needs |

A walker that comes nearer than 200 m becomes a real ped in its place (same model, place, heading); a real ped that
goes past 200 m becomes a walker the same way. Then GPU skinning, then render submission at the RSL level
(RSL_SUBMISSION.md).

This supersedes, for peds and only while the bands are on, the T2 plan's rules 1 and 2
(T2_POPULATION_AND_DISTANT_LIFE_PLAN.md: decorative walkers strictly past the real band at >= 310-350 m, the real band
never shrunk): those rules were made because walkers met real peds with no handover - the player saw "fake" people turn
into NPCs (DECISIONS.md 2026-11-13, "Crowd bands"). Cars keep the T2 rules.

## Why it should pay

Measured (PERFORMANCE.md 43, 2026-11-12; police station, ~130 peds against the game's own ~7): the population adds
~6 ms to the game thread - CWorld::Process (the peds' AI, physics, scripts) ~3 ms, the game's own render of each ped
(PreRender, the skin palette, the packets) ~3.6 ms - and on the render worker the replay copies and the skinning. Most
of those peds stand 100-325 m out where they are a few pixels tall. The real-ped work is per ped and on one thread
(DECISIONS.md 2026-11-13: the simulation stays single-threaded), so the lever is how many real peds there are: ~40
instead of ~140 should take most of the 6 ms, while the walkers cost a host loop and one instanced draw per part.

## What the game does now (the facts the plan builds on)

- AddToPopulation `FUN_002B17A8`(nearest, farthest in view, nearest, farthest out of view), from CPopulation::Update
  `FUN_002AFE80`, makes every random ped through CPopulation::AddPed `FUN_002B2908`; the spot comes from
  `FUN_00268048` (game-internals.md 12d). It spawns while the ped count `0x4CD550` is below the cap
  `0x488004` / `0x488008`.
- vcs_distance.cpp scales the rings: in view 35-60 x gen x the population distance (~175-300 m at the player's
  settings, stratified by `pedSpotFinder`), out of view 10-60 m (`VCS_PED_OUT_FAR_K`), the camera-cut fill
  `FUN_002B0070` within 1.5 x. `trimCrowd` fades (bFadeOut, +0x1CC bit 31) the farthest ped out of view past 40 m or
  anywhere past 250 m while the crowd is at its cap.
- New peds in view within ~100 m are seen standing up (game-internals.md 12d, `popin=`), which is why the in-view ring
  stays far today - and why the crowd piles up in a band far down the street (PERFORMANCE.md 46).
- Walkers: `crowdBand` puts them from `realPedOuterRadius() + 5` to + 3 x the Distant crowds setting; up to 60
  (`kMaxWalkers`), placed a few a frame in front of the camera on the ped path nodes, drawn as their model's template's
  posed mesh (templates per model and walking / standing, sticky while drawn).
- Commands for counting: `popview` (peds / cars in view within 50 / 100 / 200 m), `distancestate`, `proxies`
  (walkers, `realorphans=`), `worlddetail` (`popin=`).

## The steps

Each step: a switch (`VCS_CROWD_BANDS`, later a setting), measured on the avenue benchmark (`tools/bench_avenue.py`,
the player's card, 4K, population full, alternating A / B rounds), pictures at the player's spots, the helper's
review before the commit. Defaults stay as they are until the handover (step 4) is in and checked.

### 1. Baseline

`tools/bench_avenue.py` with today's defaults, plus one run with the population at the game's own (the floor): the
numbers every later step is compared with. Per band counts at the Little Havana spot (`popview`).

Done when: PERFORMANCE.md 49 has the table.

### 2. The real bands (`VCS_CROWD_BANDS=1`)

In vcs_distance.cpp, only while the population is raised:
- **Cap** the random peds at near + middle (`VCS_CROWD_NEAR` 20, `VCS_CROWD_MID` 20); the game's cap word gets it.
- **Rings**: no ring in view (a ped made there is seen standing up) except the camera-cut fill; out of view the ring
  goes where a band is short - near (10-60 m) while the near band is below its number, else 100-200 m (side streets,
  behind). The middle band in view is fed by walkers handed over (step 4) and by peds walking into view.
- **Edge**: a random ped past 210 m (10 m of hysteresis) goes: out of view faded as now (`trimCrowd`), in view handed
  to a walker (step 4; until then faded, a few pixels tall).
- `realPedOuterRadius()` answers 200 m, so `crowdBand` starts the walkers at 205 m without other changes.
- `popview` gains the band counts and the cap; `crowdbands` command for the knobs.

Done when: at the Little Havana spot and the police station, near / middle hold their numbers within a few (pictures
after 2 minutes), nothing stands up in view within 200 m (`popin=`), and the benchmark shows the game thread's gain.

### 3. Walkers from 200 m, more of them, on the avenues

- `kMaxWalkers` 60 -> up to 160 (the instancing takes 64 a draw); their number from the view, not a flat setting.
- Placement: today a few a frame on links in front of the camera. Spread them along the pavements in view like the
  real ones would be (both sides of a street), weighted by the node's density class (the real spawner's
  `rand & 15 <= class` table at `0x487498`).
- Their pose stays their model's template's (walking); templates now come mostly from the middle band (150-200 m
  proxies' real peds) - check `realorphans=` and that a walker never shows a model with no template.

Done when: the avenue and the police station's view hold a crowd past 200 m in pictures (zoomed with the free camera),
the walkers' draws stay instanced (draw count), no flicker.

### 4. The handover

- **Walker -> real** (a walker crossing 200 m inwards while the bands have room): the game makes the ped itself, in the
  walker's place. Hook points to find in Ghidra (`tools/eedecomp.py 0x2b17a8 0x2b2908 0x268048`): the spot finder's
  result (`FUN_00268048` returns the node / position AddToPopulation uses) and the model choice inside AddToPopulation
  (the zone's ped group). With both answered from the walker, CPopulation::Update's own call makes the ped - no guest
  call from host code (AGENTS.md rule 2). Found (decompiled 2026-11-13, `build/analysis/decomp/pedspot.c`): the model is
  `FUN_002B35B0`(the zone's ped group) (else gang / type paths `FUN_002B3B90` / `FUN_002B3C80`, the cops' 35 / 60 m rings
  only with 3+ stars), chosen before the spot and again if the spot's zone differs; the spot is the finder's out position
  (`&fStack_910`, the nodes in `iStack_a8` / `iStack_a4`); then `FUN_0026B510` jitters it across the pavement and
  CPopulation::AddPed `FUN_002B2908`(type, model, position) makes the ped. So: the finder replaced for one call (the
  walker's place, its nodes, return 1) and the model call answered from the walker. Then its heading set from the walker's (the ped's matrix and the heading
  fields, +0x30.. and the ped's own; to find), the walker dropped the same frame.
- **Real -> walker** (a real random ped past 210 m in view): a walker of its model placed at its position and heading on
  the nearest pavement link, and the ped removed at once (not faded: a fade at 200 m reads as a ghost). The game's own
  immediate removal path to find (CPopulation's remove of a ped / the pool's delete through the world).
- Mismatches: a walker whose model the game cannot make now (not loaded, the zone's group) keeps walking until it can
  or turns back; never vanishes in view.

Done when: pictures of a handover at 200 m from the free camera 30 m behind (both ways) show the same model at the
same place; no ped appears or vanishes in view within 250 m over 5 minutes (a counter of births / removals in view by
distance); the benchmark holds step 2's gain.

### 5. On by default

The bands become the population's behaviour with the raised sliders (Graphics > Distance and population), the env
switches kept for A / B. CONTROLS-style notes in PROGRESS.md, AGENTS.md's "Recognising old problems" line about the
>= 320-350 m rule updated, the T2 plan pointed here.

### 6. GPU skinning

The render worker skins each ped's vertices on the host (~10 % of its time, PERFORMANCE.md 43; `skinVertices`). Move it
to the vertex shader: the bone palette in a constant buffer, weights / indices as vertex attributes. Measure first:
with ~40 real peds its share is smaller.

### 7. Render submission at the RSL level

RSL_SUBMISSION.md (milestone 7): draws from the game's render calls instead of its DMA packets - cuts both the game
thread's packet building and the worker's replay copies.

## Status

| Step | State |
|---|---|
| 1 Baseline | done, on a busy machine (PERFORMANCE.md 49): to repeat on a quiet one |
| 2 Real bands | in (`VCS_CROWD_BANDS=1`, off): near fills, the middle band 2-5 of 20 from out of view; game thread -1.6-1.9 ms |
| 3 Walkers from 200 m | - |
| 4 Handover | - |
| 5 Default | - |
| 6 GPU skinning | - |
| 7 RSL submission | - |

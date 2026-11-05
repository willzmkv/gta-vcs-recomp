# T2 Population, Distant Life & Distant Lights — Action Plan & Agent Guide

> **Authoritative Architecture & Rules for Task T2 (`src/game/vcs_distance.*`, `src/game/vcs_distant.*`, `src/game/vcs_hle_world.cpp`)**
>
> This document defines the mandatory behavior, distance bands, invariants, root causes of historical regressions, and implementation plan for real population vs. decorative distant life (`Distant traffic`, `Distant crowds`, and `Distant lights`/coronas).
> **Every future agent touching population, draw distance, or distant proxies MUST follow these rules without exception.**

---

## 1. Golden Architectural Rules (Mandatory for All Agents)

1. **Never Shrink or Replace Real Population When Decorative Settings Are Enabled**
   - Enabling `Distant crowds` (`world.distant_crowds`) or `Distant traffic` (`world.distant_traffic`) **MUST NEVER** clamp, override, or reduce `pedDistance()` or `carDistance()` in `src/game/vcs_distance.cpp`.
   -Decorative systems exist **strictly to extend life beyond the simulated population horizon** — never to substitute real AI peds or real vehicles near the player.

2. **Two Non-Overlapping Distance Zones**
   - **Zone 1 — Real Simulated Population (`0 m` to `~300–400 m` at max slider)**:
     - Controlled by `src/game/vcs_distance.cpp` (`world.population_distance`, `world.ped_density`, `world.traffic_density`).
     - Real pedestrians and real vehicles with full PS2 AI, physics, collision, animations, and headlights/taillights cover the close and medium range around the player (up to **~300–400 m** when `Population distance` is raised).
     - Visibility culling in `FUN_0036EBF8` (`visibilityInit`, `0x004CD5D8` / `0x004CD5CC` for peds and `0x004CD5C4` / `0x004CD5B8` for vehicles) must scale with `pedDistance()` and `carDistance()` so real peds and cars are never culled inside their live simulation radius.
   - **Zone 2 — Decorative Distant Life (`>= 300–400 m` outward, strictly beyond Zone 1)**:
     - Controlled by `src/game/vcs_distant.cpp` (`world.distant_traffic`, `world.distant_crowds`).
     - Decorative proxy cars and decorative walkers may **ONLY** spawn and exist in the far band (`[lo, hi]`) where:
       - `carLo = max(kMinDistantCarNear, realCarOuterRadius + kBandMargin)` (at least `320–350 m`, and always strictly past the real vehicle removal radius).
       - `pedLo = max(kMinDistantPedNear, realPedOuterRadius + kBandMargin)` (at least `300–350 m`, and always strictly past the real pedestrian removal radius).
     - Any decorative proxy car or walker whose distance to the camera drops below `lo` (`d < lo`) must be **immediately culled and recycled** (no `0.9 * lo` hysteresis encroaching into the real zone).

3. **Never Read Transient Guest Globals Across Subsystem Phases**
   - In `CGame::Process` (`0x003BA8D8`), `CPopulation::Update` (`0x002AFE80`) runs *before* `CWorld::Process` (`0x001933C0`), while `CCarCtrl::GenerateRandomCars` (`0x00430148`) runs *after* `CWorld::Process`.
   - Therefore, during `CWorld::Process` (`vcs::distant::worldProcess`), the guest float `0x006F4CA0` (`GenerationDistMultiplier`) holds the **pedestrian** value written by `CPopulation::Update`, **not** the vehicle value!
   - `vcs::distant` **MUST NEVER** read `0x006F4CA0` directly from guest RAM to infer real traffic or crowd radii. Always query explicit C++ helpers in `vcs::distance` (`vcs::distance::realCarOuterRadius()`, `vcs::distance::realPedOuterRadius()`).

4. **Off-Screen Template Entities Must Never Leak Near-Player Draws**
   - When `addTemplates` appends an off-screen real vehicle or ped to `kVehicleList` (`0x00744AC0`) so its meshes can be piggybacked for distant proxies, the template entity itself is off-screen (`all != 0` in clip test) and must be skipped while only its distant proxies (`>= 300 m`) are drawn.
   - Candidate template search radius (`kTemplateRange`, `kPedTemplateRange`) must cover the full real population radius (`~400 m`) so templates are always found even when real cars and peds are spread across `0–400 m`.

---

## 2. Root Causes of the T2 Bugs Diagnosed on 2026-11-05

### Bug 1: `Distant crowds` Replaced Real Peds Near the Player (54–60 m)
- **Cause 1A (`src/game/vcs_distance.cpp`)**:
  `pedDistance()` was implemented as:
  ```cpp
  float pedDistance() { return vcs::distant::crowdsOn() ? 1.0f : std::min(percent(g_population), kMaxPedDistance); }
  ```
  Turning on `Distant crowds` forced `pedDistance()` to `1.0f` (PS2 default: 35–65 m), ignoring the user's `Population distance` slider.
- **Cause 1B (`src/game/vcs_distant.cpp`)**:
  `kCrowdNear` was hardcoded to `60.0f` meters, and `move()` kept decorative walkers down to `lo * 0.9f = 54.0f` meters. Because decorative walkers clone the posed mesh of 1–4 template peds and slide along pavement links without collision or AI, having them at 54 m right in front of the player looked broken and replaced real pedestrians.
- **Cause 1C (`src/game/vcs_distance.cpp` & guest `FUN_00268048` / `FUN_002B17A8`)**:
  - `kMaxPedDistance` was capped at `2.0f` (~120 m) instead of scaling out to ~300–350 m.
  - In `FUN_00268048` (`CPopulation::AddToPopulation` spot finder), `FUN_00269228` (`kNodeList`) is called at return address `0x0026812Cu` with `$f12 = 160.0f` centered on the player and a 240-node cap (`$a3 = 240`). When `pedDistance()` scales up to ~300–350 m, `nodeList` in `vcs_distance.cpp` must also scale the ped node search radius `$f12` and bias the search center forward along the camera view (just as it does for cars at `kSpotFinder`), and `FUN_002B17A8` (`addpop.c`) must scale its fallback `35.0f`/`60.0f` band so real peds spawn and stay alive out to ~300–350 m.

### Bug 2: `Distant traffic` Spawned Decorative Cars at 153–170 m
- **Cause 2A (`src/game/vcs_distant.cpp`)**:
  `band()` read `rdf(kGenMultiplier)` (`0x006F4CA0`) inside `worldProcess` (`CWorld::Process`). Because `CPopulation::Update` had just overwritten `0x006F4CA0` with `g_genBase * pedDistance()` (which was `1.0f`), `band()` computed `lo = 170.0f * 1.0f = 170.0f` (and `move()` kept proxy cars down to `0.9f * 170.0f = 153.0f` m) even when real traffic was configured to reach 400+ m.
- **Cause 2B (`src/game/vcs_distant.cpp`)**:
  When `Population distance` was at 100%, `170.0f * gen` allowed proxy cars at 153–170 m instead of enforcing a minimum distant floor (`>= 320–350 m`).

### Bug 3: Distant Lights & Coronas Were Broken / Missing
- **Cause 3A — Misplaced Code in `drawLitProxies` (`src/game/vcs_hle_world.cpp`)**:
  The distant vehicle headlight/taillight quad generator (`if (px.night && px.lights.count > 0 ...)`) was placed inside `drawLitProxies` (used exclusively by `drawNativeLit`, table `0x04` — **skinned pedestrians**, where `px.lights.count` is always `0`!) instead of `drawProxies` (used by `drawNativeRigid`, table `0x10`/`0x13` — **rigid vehicles**). As a result, distant vehicle light quads never rendered.
- **Cause 3B — Wrong Transform Matrix & Dark Lighting on Light Quads (`src/game/vcs_hle_world.cpp`)**:
  - The light quad builder wrote world-space positions (`worldPos`) into `GSNativeDraw::VcsLit` vertices while reusing `d.constants` from the template mesh (`m` and `cm` = `templateObjectToScreen` / `templateObjectToClip`). Because `d.constants` includes the template car's local part matrix `a` (which for an off-screen template car is translated to `(0, 0, -2000)`), transforming world-space vertices by `d.constants` sent the quads miles off-screen or to random positions near the player.
  - Furthermore, `VcsLit` shades vertices using `L.dir * L.col + L.ambient`. With zero normals `(0, 0, 0)` and night-time `L.ambient` (~0.15), light quads were dimmed by 85% and fogged out instead of rendering as unlit emissive sprites (`ambient = {1, 1, 1, 1}`, directional colors = `0`, fog = `0`).
  - `g_lastLightTemplate` was a static atomic never reset per entity (`beginEntity`) or per frame (`frameKicked`), so if only one template model was active, its lights were skipped on every subsequent frame.
- **Cause 3C — Distant World / Street-Lamp Coronas (`CCoronas` 56-Slot PS2 Limit)**:
  - In VCS PS2 (`FUN_0027C418` / `FUN_0027DD80`), `CCoronas::aCoronas` at `0x00512560` has only **56 slots** (`0x38`, stride `0x90`), and `CEntity::ProcessLightsForEntity` (`FUN_0027E430`) culls 2DFX coronas beyond each effect's PS2 `coronaShowDist` (`offset +0x18`, typically `100–150 m`).
  - Injecting distant coronas into `CCoronas::aCoronas` would immediately overflow the 56-slot table and starve nearby car headlights and traffic lights.
  - Therefore, distant light coronas (both distant vehicle headlights/taillights and distant 2DFX street/building lights beyond the real corona range) must be rendered natively in the HLE pass without consuming slots in `0x00512560`.

---

## 3. Target Distance Specification

| Layer | Controlled By | Inner Radius (`lo`) | Outer Radius (`hi`) | Notes |
|---|---|---|---|---|
| **Real Pedestrians** | `vcs_distance.cpp` | `0 m` | `~65 m` (at 100%) up to **`~325 m`** (at 300% slider, `5.0x` multiplier) | Full PS2 AI, physics, collision, bones. Never reduced by `Distant crowds`. |
| **Real Vehicles** | `vcs_distance.cpp` | `0 m` | `~160 m` (at 100%) up to **`~400 m`** (at 300% slider, `2.5x` multiplier) | Full PS2 pathing, physics, headlights/taillights. Never reduced by `Distant traffic`. |
| **Decorative Crowds (`Distant crowds`)** | `vcs_distant.cpp` | `max(320 m, realPedOuter + 20 m)` | `lo + 3.0f * setting` (up to `~620–650 m` at 100) | Posed walker proxies on pavement links strictly beyond real peds. |
| **Decorative Traffic (`Distant traffic`)** | `vcs_distant.cpp` | `max(350 m, realCarOuter + 20 m)` | `lo + 5.5f * setting` (up to `~900–970 m` at 100) | Rigid vehicle proxies on car path links strictly beyond real cars. |
| **Distant Lights / Coronas** | `vcs_distant.cpp` + `vcs_hle_world.cpp` | `lo` of distant traffic (for car lights) / `150 m` (for world 2DFX lights) | `hi` of distant traffic / `~900 m` | Emissive camera-facing billboards at night (19:00–06:00). |

---

## 4. Step-by-Step Implementation Plan

### Step 1: Fix Real Population Distance & Ped Spawning (`src/game/vcs_distance.h` & `src/game/vcs_distance.cpp`)
1. **Decouple `pedDistance()` from `vcs::distant::crowdsOn()`**:
   - Remove the `vcs::distant::crowdsOn() ? 1.0f : ...` override in `pedDistance()`.
   - Map `g_population` (`100..300%`) so:
     - At `100%` (PS2 default): `carDistance() = 1.0f` (~160 m), `pedDistance() = 1.0f` (~65 m).
     - At `300%` (slider 100 in UI): `carDistance() = 2.5f` (**400 m** outer radius), `pedDistance() = 5.0f` (**325 m** outer radius).
2. **Scale Real Ped Visibility & Spawn/Keep Radii**:
   - In `visibilityInit` (`FUN_0036EBF8` hook):
     - Scale `kPedVisSq` (`0x004CD5D8`) and `kPedLodSq` (`0x004CD5CC`) by `(own * peds * 70.0f)^2` so real peds remain visible out to `350 m` at max slider.
     - Scale `kVehicleLodSq` (`0x004CD5C4`) and `kBigVehLodSq` (`0x004CD5B8`) so real vehicles remain visible out to `>400 m`.
   - In `nodeList` (`FUN_00269228` hook):
     - Handle both the car spot finder (`ra == 0x00267A4Cu`) and the ped spot finder (`ra == 0x0026812Cu` in `FUN_00268048`).
     - When `pedDistance() > 1.0f`, scale the ped node search radius `$f12` (`160.0f * pedDistance()`, capped to keep within the 240-node buffer or shifted forward along the camera view direction so pavement nodes out to 300–325 m are cached).
   - In `FUN_002B17A8` (`addpop.c` / `kAddToPop` hook):
     - Ensure `GenerationDistMultiplier` (`0x006F4CA0`) is applied consistently during `CPopulation::Update` (`0x002AFE80`) and that `FUN_00268048` receives the scaled generation/removal distances.
3. **Expose Deterministic Radius Queries in `vcs::distance`**:
   - Add `float realCarOuterRadius()` and `float realPedOuterRadius()` to `vcs::distance` so `vcs::distant` can query the exact real population outer radii on any thread/phase without reading transient guest RAM (`0x006F4CA0`).

### Step 2: Fix Decorative Traffic & Crowd Bands (`src/game/vcs_distant.h` & `src/game/vcs_distant.cpp`)
1. **Fix `band()` for Proxy Cars**:
   - Replace the broken `rdf(kGenMultiplier)` read with `vcs::distance::realCarOuterRadius()`.
   - Set `lo = std::max(350.0f, vcs::distance::realCarOuterRadius() + 20.0f)`.
   - Set `hi = lo + 5.5f * static_cast<float>(setting)`.
2. **Fix Crowd Band (`worldProcess` / `placeWalkers` / `move`)**:
   - Set `pedLo = std::max(320.0f, vcs::distance::realPedOuterRadius() + 20.0f)`.
   - Set `pedHi = pedLo + 3.0f * static_cast<float>(crowds)`.
3. **Strict Inner-Boundary Culling**:
   - In `move()`, immediately recycle any proxy car with `d < lo` or `d > hi * 1.05f` (and any walker with `d < pedLo` or `d > pedHi * 1.05f`). Never allow `0.9f * lo` to let decorative entities drift inside the real population zone.
   - Also reject any proxy car or walker behind the camera (`dot < -0.15f`) so slots are concentrated in the player's view frustum.
4. **Update Template Selection Ranges**:
   - Increase `kTemplateRange` (`160.0f -> 420.0f`) and `kPedTemplateRange` (`60.0f -> 350.0f`) so `addTemplates` can always find loaded real cars/peds to use as mesh templates even when real entities are spread out to 300–400 m.

### Step 3: Fix Distant Lights & World Coronas (`src/game/vcs_distant.cpp`)
1. **Fix `CRegisteredCorona` Table Stride (`kCoronaStride = 0x30u`)**:
   - Each `CRegisteredCorona` entry in `0x00512560` is `0x30` bytes (`48` bytes, 56 entries from `0x00512560` to `0x00512FE0`), as proven by `FUN_0027dd80` (`addiu $a0, $a0, 0x30`) and `FUN_0027c418` (`addiu $s2, $s2, 0x30`). Never use `0x90`.
2. **Hook `CCoronas::RegisterCorona` (`0x0027DD80`)**:
   - Guard with `if (ctx->pc == kRegisterCorona)` so mid-function checkpoint resumes pass straight through.
   - Suppress corona registrations from off-screen template vehicles added by `addTemplates(false)` (`g_addedCarTemplates`) so off-screen template cars never pollute the 56-slot `CCoronas::aCoronas` table (`0x00512560`).
   - Cull behind-camera and near-camera-plane coronas (`dot < 1.2 m` or `dist2 < 1.8 m^2` with camera forward `0x006F44E0`) and free any existing slot (`id` at `+0x20`, `fadeState` at `+0x24`, `registeredAlpha` at `+0x25`) so raising `View distance` does not fill all 56 corona slots with behind-camera streetlamps.
   - Cap base world-space `size` (`std::abs(ctx->f[12])`) to `0.65 m` for vehicle lights and `0.90 m` for world/2DFX lights, and clamp `normalAngle` (`ctx->f[14]`) to `>= 0.85 rad` so `CCoronas::Render` Branch 1 (`6.0 * (1 - normalAngle * 2/pi)^8 + 0.5`) never multiplies corona radius by `6.5x`.
   - Scale vehicle corona `farClip` (`$f13 <= 55.0 m`) with `realCarOuterRadius() / 160.0f` (up to `250 m`) and building/streetlamp 2DFX corona `farClip` (`55 < $f13 <= 200 m`) with `ms_lodDistScale` (`0x004887F8`, up to `300 m`).
   - When all 56 slots are occupied, evict the farthest / behind-camera non-sun/moon (`slotId > 2`) slot in favor of closer lights and vehicle headlights/taillights.
3. **Hook `CCoronas::Render` (`0x0027C418`) to Eliminate Close-Range / Widescreen Edge Perspective Explosion**:
   - `CSprite::CalcScreenCoors` (`0x0039FC48`) divides a corona's world-space `size` by camera-space depth `z_cam` (`dot`), not Euclidean distance `dist`. In widescreen 16:9, when the player drives or walks past a streetlamp or car on the left/right side of the road, `z_cam` approaches `0` while `dist` is still `5–15 m`, causing `1 / z_cam` to explode by `15x–50x`.
   - Before calling `FUN_0027c418`, temporarily scale each active non-sun/moon slot's `size` (`+0x00`) by `dot / std::max(dist, 22.0f)` and damp `fadeState` (`+0x24`) when `dot < 6.0 m` or `dist < 16.0 m`, then restore both after `FUN_0027c418` returns. The `dot` factor cancels `1 / z_cam`, making screen radius proportional to `baseSize / std::max(dist, 22.0f)`.

### Step 4: Multithreaded World Chunk & Model Streaming (`src/game/vcs_stream.cpp` & `src/game/vcs_distance.cpp`)
1. **Unblock `CRenderer::ScanSectorList` (`0x0035FAE8`) & Expand `ShouldModelBeStreamed` (`0x0035F910`)**:
   - `CRenderer::ScanSectorList_Priority` (`0x0035FED0`) sets `ms_bStreamingLocked` (`0x004CD5AC`) to `1` on the very first unloaded model in the immediate camera sector, which caused `CRenderer::ScanSectorList` (`0x0035FAE8` at `0x35fe10`) to skip `RequestModel` for all unloaded trees, sidewalks, streetlamps, and props in all other visible sectors.
   - Hook `0x0035FAE8` (`scanSectorList`) to clear `0x004CD5AC = 0` when the stream manager queue (`*(0x490158) + 0xE20`) has `< 32` entries, and hook `0x0035F910` (`shouldModelBeStreamed`) to apply `std::max(ms_lodDistScale, LODDistMultiplier) * 1.35f` so separate world props are prefetched well ahead of their draw distance.
2. **Multithreaded Worker Pool & Parallel Queue Prefetch (`src/game/vcs_stream.cpp`)**:
   - Spawn `4–8` worker threads (`std::clamp(hardware_concurrency(), 4u, 8u)`) reading concurrently from the lock-free memory-mapped ISO (`IsoImage::readBytes`).
   - Hook `FUN_002213d0` (`0x002213D0`, `streamEnqueue`) so every callback-less `CStreaming` request (`callback == 0`) queued behind `mgr->activeReq` (`+0xE24`) immediately dispatches its `(dest, lsn, bytes)` read to the worker thread pool in parallel while `CRenderer::ScanWorld` is still scanning sectors.
   - Hook `FUN_00220fa8` (`0x00220FA8`, `streamPump`) to drain all prefetched/ready callback-less requests in `mgr + 0xE24` and `mgr + 0xE18` in a single batch when called outside `streamEnqueue`, eliminating the 1-request-per-call DFF/TXD dependency stall.
   - Hook `CStreaming::ProcessLoadingChannel` (`0x003C7048`) to convert up to `6` ready resources (TXDs and DFFs) per frame within a strict `1.2 ms` time budget instead of stopping after a single resource per frame.
   - Reduce `streamPoll` (`0x002218F0`) wait from `20 ms` to `250 us` on channel 4 (`CWorldStream` chunk streaming) and `2 ms` on other channels, and add a lock-free fast path to `overlay()` when no mod overlays are active.

---

## 5. Verification Checklist for Agents

Before considering any T2 change complete, agents **must** verify via the hidden test rig (`tools/distance_check.py` and `toolchain.test_window --hidden` with isolated `VCS_DATA_DIR`):

1. **`distancestate` & Pool Check**:
   - With `world.population_distance = 100` (`300%`), `world.distant_crowds = 100`, `world.distant_traffic = 100`:
   - `pedDistance()` (`peddist`) is `5.00` (~325 m survival / ~350 m visibility), **not** collapsed to `1.0f`.
   - Real peds (`maxpedd`) and real cars (`maxcard`) populate `0–350+ m`.
2. **`proxies` Distance Band Check**:
   - Query the `proxies` control command and verify that **every** active proxy car has `mincar >= 340 m` (`>= 495 m` at slider 100) and **every** active decorative walker has `minped >= 310 m` (`>= 340 m` at slider 100). Zero decorative entities may exist within `0–310 m` of the player.
3. **Night Distant Lights Check (`coronas`)**:
   - At night (`set_time(22)`, `view_distance = 50`), query `coronas` and verify `active <= 56`, `veh > 0` (vehicle headlights/taillights are not starved by streetlamps), `tmplskip > 0` (off-screen template car coronas suppressed), `behindskip > 0`, and `evicted > 0`.
4. **Performance & Stability**:
   - No NaN cascades, no pool overflows (`overflow=0` or bounded in `EntryInfoNode` reserve), `stale=0` in `proxies`, and `tools/distance_check.py` passes.

---

## 6. Implemented & Verified Baseline (2026-11-05)

All three steps above are implemented and verified in `src/game/vcs_distance.h`, `src/game/vcs_distance.cpp`, and `src/game/vcs_distant.cpp`:
- `distancestate` (Ocean Drive / Washington Beach boulevard, `population_distance=100`, `vehicle_density=50`, `pedestrian_density=50`):
  - `Distant crowds = 0`, `Distant traffic = 0`: `peddist=5.00`, `maxcard=355 m`, `maxpedd=356 m`
  - `Distant crowds = 100`, `Distant traffic = 100`: `peddist=5.00`, `maxcard=350..371 m`, `maxpedd=351..371 m`
- `proxies` (`Distant crowds = 100`, `Distant traffic = 100`):
  - `count=200`, `walkers=299..300`, `carband=495..1045 m`, `pedband=340..640 m`, `mincar=496..500 m`, `minped=341..343 m`, `stale=0`
- `coronas` (Night 22:00, `view_distance=50`):
  - `active=56`, `veh=27`, `maxdist=249..262 m`, `tmplskip=397..488`, `behindskip=296k+`, `evicted=644..732`
- `tools/distance_check.py`: `PASS` (`original 100% -> view28 352% -> back 100%`).

---

## 7. Critical Learnings & Anti-Patterns (Post-Mortem: 2026-11-05)

### Anti-Pattern 1: Wrapping Recompiled Functions with Guest Branches (`dispatchGuestBranch`) in C++ Loops
- **Failure Mode**: Attempting to hook `FUN_003c7048` (`kProcessLoadingChannel`) or `FUN_00220fa8` (`kStreamPump`) with a C++ `for (int pass = 1; pass < 6; ++pass)` loop.
- **Root Cause**: Recompiled functions in PS2Recomp are **not** ordinary C++ functions. When a guest instruction invokes `runtime->dispatchGuestBranch` (e.g. `JAL`), the function returns immediately (`return;`) to the outer guest scheduler. Calling it in a C++ loop decrements `$sp` multiple times, corrupts stack frames and `$ra`, and triggers `[guest-branch:missing-target] ... target=0x1fdabb0` which terminates the game thread (`[ee] thread 1 -> dormant`).
- **Rule**: Never wrap any recompiled function that contains internal guest branch dispatches in a synchronous C++ loop.

### Anti-Pattern 2: Multithreading Sequential Channel Streaming without Protocol Synchronization
- **Failure Mode**: Spawning multiple worker threads that pop concurrently from the CDVD read queue across channels 0–5.
- **Root Cause**: The PS2 streaming protocol relies on strictly ordered sequential delivery per channel. When multiple threads process requests concurrently on the same channel, later requests finish ahead of earlier ones. `streamPoll` returns ready prematurely, causing the game engine to parse half-written textures and models from guest RAM, corrupting memory and crashing during loading.

### Anti-Pattern 3: Wrapping `CCoronas::Render` (`0x0027C418`) vs. Adjusting `CCoronas::RegisterCorona` (`0x0027DD80`)
- **Failure Mode**: Attempting to intercept `CCoronas::Render` to temporarily scale corona slot radii.
- **Root Cause**: `FUN_0027c418` has over 30 guest branch dispatch resume points. Any cleanup code placed after `g_coronasRender()` runs prematurely on the very first yield.
- **Solution**: Handle corona size capping and near-camera dampening entirely inside `CCoronas::RegisterCorona` (`0x0027DD80`). By modulating `$f12` (radius) and `$t0` (alpha) with a smooth proximity factor:
  ```cpp
  const float fade = std::clamp((dot - 1.5f) / 7.5f, 0.0f, 1.0f) *
                     std::clamp((dist - 2.0f) / 18.0f, 0.0f, 1.0f);
  ctx->f[12] = std::copysign(clampedBaseSize * std::max(0.04f, fade), ctx->f[12]);
  SET_GPR_U32(ctx, 8, static_cast<uint32_t>(origAlpha * fade));
  ```
  The near-camera perspective singularity (`1 / dot` approaching infinity) is algebraically cancelled out, preventing coronas from blowing up across the screen when passing streetlamps or cars.


### Anti-Pattern 4: Testing Model-View Matrix (.v[3]) instead of MVP Matrix (.v[3]) for Proxy Distance & Frustum Culling
- **Failure Mode**: In drawProxies and drawLitProxies (src/game/vcs_hle_world.cpp), size culling tested size < 0.008f * transform(cm, centre).v[3] and frustum culling tested clipFlags(q, q.v[3]) using cm (VU row 24).
- **Root Cause**: setupMatrices(tmp, 0, 16, 12, 24) outputs Model-View to rows 24-27 and MVP (Model-View-Projection) to rows 28-31. In an affine Model-View matrix, v[3] is always 1.0f. Testing transform(cm, centre).v[3] resulted in size < 0.008f * 1.0f = 8 mm, meaning no part of any car or pedestrian was ever culled, causing all 240 distant cars to submit ~20 individual draw calls (wheels, doors, headlights, wipers) even at 800 meters, flooding the worker thread with 7,906 draws per frame and collapsing framerates to 30 FPS. Furthermore, clipFlags against q.v[3] == 1.0f on eye-space coordinates completely broke bounding-box frustum culling.
- **Solution**: Fold MVP row 28 (m) first. In row 28, pc.v[3] is true camera depth z_cam. Sub-meshes smaller than threshold are culled before matrix setup, and transform(m, v) produces true clip-space coordinates for exact frustum culling. Row 24 (cm) is folded only for visible surviving meshes. This reduced drive draw calls from 7,906 down to 2,494 (-68.5%) and restored 57-60 FPS driving performance.

### Anti-Pattern 5: Streaming Queue Flooding and Premature streamPoll Timeouts Triggering Input Lag
- **Failure Mode**: After attempting to prefetch distant props by clearing `ms_bStreamingLocked = 0u` in `ScanSectorList` and lowering `streamPoll` wait timeout to 250 us, driving experienced severe stuttering and perceptible input lag.
- **Root Cause**:
  1. `ScanSectorList` (`0x0035FAE8`) runs for hundreds of visible sectors with high view distance. Clearing `ms_bStreamingLocked = 0u` unconditionally caused every visible sector to call `CStreaming::RequestModel(...)` for thousands of distant props, saturating the streaming queue and raising game thread time (`game_ms`) to 15.7 ms.
  2. The game engine polls `streamPoll` (`0x002218F0`) immediately after issuing reads. SSD/OS file reads take ~0.5 - 1.5 ms. A 250 us timeout returned `kBusy` (0xFA), forcing the engine to forfeit the current frame and defer each read by a full 16.7 ms frame.
  3. When frame time exceeded 16.67 ms, the presenter queue accumulated a backlog (`g_framesPosted - g_framesTaken > 1`). The host-paced frame gatekeeper in `ps2HostTick` withheld ticks to prevent drift, starving `ps2WaitHostTick` on the game thread and buffering input by 1-2 frames.
- **Solution**: Removed the `ScanSectorList` hook to let `CStreaming` naturally throttle requests, and restored `streamPoll`'s 20 ms conditional wait (which wakes immediately when reads complete without losing frames).

### Anti-Pattern 6: Running Heavy setupMatrices (128 VU Ops) on Culled Distant Proxies
- **Failure Mode**: Even with MVP frustum and size culling, `worker_ms` remained high (16.95 ms) during driving, leaving zero margin for a consistent 60 FPS.
- **Root Cause**: In `drawProxies` and `drawLitProxies`, `setupMatrices(tmp, 0, 16, 12, 24)` and `foldMatrix` were executed inside the proxy loop before testing whether the proxy was behind the camera or too small. With 240 proxies and ~20 parts per template car, 4,800 iterations per frame each executed 128 VU floating-point operations (~600k ops/frame) just to compute `m` for culling tests that 90% of proxies failed anyway.
- **Solution**: Loaded the camera View matrix `br` (VU row 16) once outside the loop. For each proxy `n`, transformed the part's world center `W` into camera view space using `br` in 12 basic floating-point operations. If `z_cam <= 0.0f` or `size < threshold * z_cam` or outside view bounds, the proxy is immediately skipped without touching `tmp` or executing `setupMatrices`.
- **Result**: `worker_ms` dropped from 16.95 ms down to **11.7 ms** (-31.0%), `game_ms` dropped from 15.73 ms to **12.2 ms**, and driving FPS jumped from 57.02 to **73.5 FPS** (+28.9%), eliminating all slowdown and input lag.


### Anti-Pattern 7: Quadratic Population Multipliers Spawning ~1000 Unrealistic Entities vs OLA Calibration
- **Failure Mode**: Setting all sliders to 100% resulted in ~1,000 entities in the scene simultaneously (175 real cars, 250 real peds, 240 distant cars, 300 distant walkers), causing surreal Miami traffic gridlocks, NPC swarms, and high physics collision load on the single-threaded EE guest thread.
- **Root Cause**: Area scales with radius squared ($r^2$), so a 3x population distance multiplied by 4x density yielded $36x$ spawn demand, saturating the expanded memory pools to their absolute ceilings.
- **Solution**: Calibrated the 100% targets to authentic Open Limit Adjuster (OLA) and SkyGFX / Project2DFX standards for 1986 Vice City:
  - Real active cars: capped at 42 cars (PS2 base was 16).
  - Real active pedestrians: capped at 70 peds (PS2 base was 30).
  - Distant traffic proxies: capped at 60 cars on bridges and highways.
  - Distant crowd proxies: capped at 60 walkers on distant beaches and promenades.
  - Pools adjusted to PC OLA baseline: 160 peds, 100 cars.
- **Result**: Scene entity count at 100% normalized from ~965 down to **~232 entities** (~5x the PS2 original, maintaining vibrant, full avenues without absurd congestion). Drive benchmark jumped from 73.5 FPS to **92.6 FPS**, frame time dropped to **10.8 ms**, and game thread time dropped to **9.7 ms**, guaranteeing zero slowdown at 60 FPS with huge safety margins.

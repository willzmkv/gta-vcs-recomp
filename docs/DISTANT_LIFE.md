# Distant life - traffic and lights beyond the simulated world (plan, 2026-11-04)

> **IMPORTANT**: Read [`docs/T2_POPULATION_AND_DISTANT_LIFE_PLAN.md`](T2_POPULATION_AND_DISTANT_LIFE_PLAN.md) for the mandatory distance-band invariants (`0–325/400 m` real population vs. `320/350 m+` decorative distant proxies and lights) and rules for future agents.

FORK.md T2's path A, chosen by the player after the dense-crowd measurements (PERFORMANCE.md 34): each real car or ped
runs the PS2 game's whole code (AI, physics, collision, bones, the render setup) - ~0.085 ms of the game thread each,
plus ~10-20 draws on the main thread - so the densities the player wants cannot all be simulated at 70 fps. As
Project2DFX does on PC ("3D distant cars", LOD coronas), what is far away is drawn, not simulated: real traffic and
peds near the player (the population distance), decorative traffic beyond, distant lights at night. Then path B
(draw batching on the main thread).

## What exists to build on

- The native renderer draws rigid lit models from the VU1 packets' memory in `vcs_hle_world.cpp` (`drawNativeRigid`):
  VU qwords 0-3 the object's matrix (`a`), 16-19 the view (`b`), 12-15 the projection (`p`); the routine 0x04d8
  (`setupMatrices`) makes 24-27 = b x a and 28-31 = p x (b x a); lights 44-46 / `rigidLights`, fog 52-53. The mesh is
  cached by its content key (`GSNativeDraw::meshKey`), the GS state (texture, blend) is the GS's at that point.
- Draws reach the worker as draw items (`vcs_rsl_world.cpp`, FULLY_NATIVE.md step 4c) made on the game thread by the
  RSL draw calls, in the order CEntity::Render (`FUN_003c3e58`, vtable +0x?? per entity) issues them.
- The car path network (`*(0x487490)`: nodes of 10 bytes {x*8 s16, y*8 s16, z s8, ..., links}, game-internals.md 9h
  "Random traffic", the spot finder `FUN_00267928`).

## Design

1. **Proxy traffic (host, game thread)**: up to N proxies placed on car path links beyond the real traffic's reach
   (past the population distance's ring, out to a "distant traffic" distance), moving along the links at road speed,
   turning at nodes; recycled when they leave the band. A frame's proxies (world matrix each) are published for the
   worker with the frame number.
2. **Templates (piggyback drawing)**: when CEntity::Render draws a real vehicle far from the camera (past ~120 m), its
   draws are tagged as a template group (the entity's world matrix with them): the items it makes carry the group, the
   worker sets the group while it runs them. In `drawNativeRigid`, a draw of a template group is also drawn for the
   proxies assigned to the group: the object matrix `a' = a x inv(A_car) x A_proxy`, the rest of the VU setup redone
   as 0x04d8 does, the same mesh key (cached), the same GS state (texture, colours), the same lights - one more
   submitNative each. Proxies wear the models the traffic has loaded at that moment.
3. **Culling and cost**: a proxy's draws are tested against the frustum as the original's clip test does (per vertex,
   the mesh is small at LOD); draws joined by the native merge where the state allows (later: path B's instancing).
4. **Lights**: at night proxies get head / tail light coronas; then LOD lights (street lamps) from the world's light
   effects, drawn as a native corona pass.
5. **Settings**: Graphics > Distance and population > Distant traffic (0 off .. 100), later Distant lights.

## Steps

- A1 proxy simulation on the path network, checked with the rig (positions on roads, speeds, no pile-ups). Done.
- A2 template tagging (CEntity::Render -> items -> worker) and proxy draws in `drawNativeRigid`; screenshots. Done
  (2026-11-04), as built:
  - Proxies are placed in front of the camera (within 60 degrees, at most 4 a frame) and those behind it placed again:
    they exist to be seen.
  - CEntity::Render (`FUN_003c3e58`) opens a group for a pool vehicle of the cars' classes (vtables `0x48F8C0`,
    `0x490F10`; game-internals.md "Rendering the lists"); the next entity closes it. Each draw item made meanwhile keeps
    the group (`Item::group`); the worker sets it around the item (`vcs::distant::workerItem`).
  - At the kick (`vcs::distant::frameKicked`) the farthest car of each model is its template; the proxies go to the
    template of their model (a model taken from the templates seen when placed), else to one chosen by their slot.
  - Templates whatever is on screen: before the PreRender of the visible lists (`FUN_0035c4b8`), the farthest pool car
    of each model within 450 m (`kTemplateRange`) that is not listed joins the vehicles' list - the game renders it, the
    worker culls its draws (off screen) and draws its proxies. Without this an empty street in view had no proxies at all.
  - The draws' world origin (`0x6FE980`, minus the cell's centre) is added to the car's and the proxies' matrices: the
    VU1 matrices are relative to it. A draw whose matrix is more than 12 m from its car's is not the car's (made after
    it by another path) and is not copied.
  - In `drawNativeRigid` (and the env pipeline's culled / clipped paths): per proxy a' = a x inv(car) x proxy, 24-31
    made again as 0x04d8 makes them, lights turned with it, the mesh's box against the frustum, parts under ~6 pixels
    of a 1080-line picture skipped.
  - Measured (hidden rig, Starfish Island avenue, 200 proxies): GS draws 2100 -> 4600 a frame, game thread +0.3..1.6
    ms. Too many draws for D3D11 on the player's AMD card: path B (a D3D12 backend, then instancing) next.
- A3 band hand-off (2026-11-05):
  - **Strict two-band separation** (`docs/T2_POPULATION_AND_DISTANT_LIFE_PLAN.md`):
    - **Real population (Band 1)**: real vehicles cover `0..480 m` (with stratified forward-shifted node caches out to `~350–400 m` in view) and real pedestrians cover `0..325 m` (`kMaxPedDistance = 5.0f`, with alternating `+40 m` / `+145 m` forward-shifted ped path node cache and 4-band stratified `pedSpotFinder`). Enabling `Distant crowds` or `Distant traffic` **never** shrinks real population distances.
    - **Decorative traffic & crowds (Band 2)**: `band()` starts strictly at `max(340.0f, realCarOuterRadius() + 15.0f)` (`495..1045 m` at population slider 100), and `crowdBand()` starts strictly at `max(310.0f, realPedOuterRadius() + 15.0f)` (`340..640 m` at population slider 100). Any proxy or walker with `d < lo` is immediately recycled.
- A4 distant lights & corona management (2026-11-05):
  - `CCoronas::RegisterCorona` (`0x0027DD80`) is hooked in `vcs_distant.cpp`:
    1. Off-screen template vehicles added by `addTemplates(false)` are suppressed from registering headlight/taillight coronas.
    2. Behind-camera coronas (`dot < -5.0 m`) are culled (and any existing behind-camera slot in the 56-slot `CCoronas::aCoronas` table at `0x00512560` is freed) so high `View distance` does not fill all 56 corona slots with behind-camera streetlamps.
    3. Vehicle corona `farClip` (`$f13 <= 55 m`) scales with `realCarOuterRadius() / 160.0f` (up to `250 m`), and building/streetlamp 2DFX corona `farClip` scales with `ms_lodDistScale` (up to `300 m`).
    4. When all 56 slots are occupied, the farthest / behind-camera non-sun/moon slot is evicted in favor of closer lights and vehicles.

## Distant crowds (2026-11-05)

- Graphics > Distance and population > Distant crowds (`world.distant_crowds`, 0..100, `VCS_DISTANT_CROWDS`): real pedestrians keep their full `pedDistance()` (`up to 5.0x = ~325 m` survival / `350 m` render visibility; `crowdLod` throttles distant real peds to 1/2 or 1/3 rate and root-motion-only animations off-cone), while up to 300 decorative walkers walk the pavement nodes strictly beyond the real pedestrian band (`max(310 m, realPedOuterRadius() + 15 m)`, i.e. `340..640 m` at population slider 100).
- The pavement network: the path nodes' array holds the car nodes [0, paths +0x10) then the ped nodes [.., + paths
  +0x14), one links table (8387 = 3079 + 5308; game-internals.md 9h).
- Templates: random peds (vtable 0x490840) open a group in CEntity::Render (the ped Render 0x201300 calls it); per model a
  walking one (move speed at +0x140) rather than the farthest; one of each model within 360 m (`kPedTemplateRange`) joins the
  visible list when off screen, as the cars do.
- Drawing (`drawLitProxies`, the skinned lit pipeline): a skinned ped's VU object matrix is the identity and its skinned
  positions are in the draws' space (the bones carry the ped's place), so a' = inv(ped) x walker moves the posed mesh
  onto the walker; the walker wears the template's pose of that frame.
- Verified (hidden rig, Washington Beach / Ocean Drive boulevard, `population_distance=100`, `distant_traffic=100`, `distant_crowds=100`):
  - Real population reach: `peddist=5.00`, `maxcard=350..371 m`, `maxpedd=351..371 m` (identical whether `Distant crowds` is `0` or `100`).
  - Decorative bands: `carband=495..1045 m` (`mincar=496..500 m`), `pedband=340..640 m` (`minped=341..343 m`).
  - Night coronas (22:00, `view_distance=50`): `active=56`, `veh=27`, `maxdist=249..262 m`, `tmplskip=397..488`, `behindskip=296k+`, `evicted=644..732`.

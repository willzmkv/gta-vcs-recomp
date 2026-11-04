# Progress

## 2026-11-02 - This fork (willzmkv/gta-vcs-recomp)

The game's fog as a setting; texture packs in DDS (BC7) or PNG and a texture dump; a mod loader in PC Mod Loader's
style - `modloader/` folders, `modloader.ini`, F2 > Mods with switches and priority sliders, applied live. The changes,
their checks (`fog_check.py`, `texture_check.py`, `mods_check.py`) and the work list are in docs/FORK.md; the mods in
docs/MODS.md. The section below (2026-10-31) is upstream PR #8's, which this fork took onto 0.2.0 and builds on.

## 2026-10-31 - Optional PCSX2 PNG texture packs

- (Since 2026-11-02 in this fork: F2 > Mods > Replace textures, live, and packs as mods in `modloader/`.)
- F2 > Graphics > Texture replacements (restart; off by default), existing `SLUS-21590/replacements` found beside
  the exe or in the source root. Configurable path, async PNG decoding, shared GPU images, byte-budgeted LRU,
  original fallback and high-resolution sampling with the game's wrap/region/alpha rules. `docs/TEXTURE_PACKS.md`.
- Checked: build; loader tests (legacy names, palettes, non-integer PNG sizes, unchanged alpha, asynchronous
  dedup/reload and corrupt-file fallback); all 160 user PNGs decoded; five actual gameplay locations, 25 pack files
  matched/loaded; disabled static captures mean difference 0.051 / 0.128 (loading / pause map, tolerance 2.0).
- Cache-pressure check found empty alternate VBlanks evicting active textures; protect two previous frames too.
  The targeted 64 MiB / bilinear check passes without repeated per-VBlank reloads. Native render targets unchanged.
- Same-build off/on benchmark: all six spots held 60 fps; AI drive game 5.50 -> 5.58 ms, worker 3.32 -> 3.32 ms,
  GS 2.95 -> 2.97 ms. One capped pair; full numbers and conditions in PERFORMANCE.md 29.
- Fixed the rig's empty-card readiness: area 0 plus advancing frames also described the title flyover. It now
  requires a player ped; earlier empty-card bench results from that check should not be treated as gameplay.

## Phase 0 - setup (done)
- Repo, GPL-3.0, `.gitignore` blocking game files; `tools/vcs_disc.py`
- PS2Recomp vendored (changes in `docs/PS2RECOMP_PATCHES.md`)
- Toolchain: clang-cl 23 + MSVC 14.29 STL, CMake, Ninja, Ghidra 12.1.4 + EE ext, JDK 21

## Phase 1 - analysis (done)
- 9,550 Ghidra functions + 833 indirectly-referenced ones Ghidra missed
  (`tools/find_missing_functions.py`), SDK identification, `game/vcs/vcs.toml`

## Phase 2 - boot (done)
- ISO mapped as cdrom0: (EE, VFS, IOP), real IRX modules on the emulated IOP
- Fixed: interrupt stacks clobbering the main stack, ';1' names for the
  game's file table, cdvdman imports for CDSTREAM, many SDK stubs
- Game loop runs; title/intro 3D scene renders (CPU renderer)
- Movies skipped for now (`src/game/vcs_overrides.cpp`)

## Phase 3 - graphics / performance (in progress)
- [x] VU1: hctp's throughput-mode VU1 core (13 -> 23 VBlanks/s, GS discarded)
- [x] IOP time batching (23 -> 60 VBlanks/s, GS discarded) = full speed without GS
- [x] paraLLEl-GS integrated but opt-in: hangs Pascal GPUs into TDR resets
- [x] GS usage survey (`VCS_GS_STATS`): see docs/game-internals.md
- [~] Hardware GS backend (`PS2X_GS=hw`, `src/lib/gs/gs_hw.cpp`; D3D11 since stage 3 of NATIVE_RENDERER.md, was OpenGL): runs at
      full speed and shows the loading screen. Fixed: overlapping render targets
      (back buffer FBP 140 = FBP 0 + 448 lines), stale page cache for CLUT loads.
- [x] Loading screens had wrong colours: VIF1 fed the next DMAtag's VIFcodes to
      the GIF as image data (8-byte shift of every texture/CLUT). Fixed.
- [x] ~25 s bad-pointer crash: gone after the IOP heap fix (CDSTREAM's 256 KB
      streaming buffer failed to allocate; the game ran an error path).
- [x] Stuck on the loading screen: IOP `sprintf` didn't format, so the sound
      bank file was never found. Fixed; the game now reaches the first in-game
      scene (Fort Baxter: Vic + army truck).
- [x] Crash in the first scene (returns to 0x3F333333-style addresses): runtime
      treated a preempted recursive call as returned (stack off by inner frames).
      Fixed; the game now plays the opening cutscene (Vic, Sgt. Martinez).
- [x] Props near-invisible: VIF UNPACK V4-5 didn't expand RGBA5551. Fixed.
- [x] Tilted horizon / broken camera maths: SQRT.S read the wrong register field
      (every sqrtf). Fixed: horizon level, palm trees and vehicles in place.
- [x] Static world (terrain, buildings) not drawn: VCALLMSR used vi[27] instead of
      CMSAR0, so the VU0 sector frustum test culled everything. Fixed; ground,
      buildings and the skyline render.
- [~] In-game speed: 11 -> ~55-60 VBlanks/s in gameplay (paced: never above 60) (compiled VU1, native VU0 box clip /
      matrix programs, texture/CLUT/upload/timer fixes, no FP contraction).
      Remaining: guest code ~31%, T4 swizzle ~9%, DMA chain copies ~7.5%
- [ ] Performance: per-instruction `ctx->pc` stores (emitter now skips them for
      pure ALU ops - needs a regenerate + test), `__restrict` for generated code

## Phase 4 - audio, input, memory card, movies
(Audio has been native since milestone 6 - the SPU2-emulation items below describe how it started; the current state
and the 2026-10-26 audit are under "Milestone 6".)
- [x] Input: keyboard + mouse + gamepad (docs/CONTROLS.md), verified in game: after the
      intro cutscene the player controls Vic at Fort Baxter (HUD, radar, mission marker)
- [x] Audio: SPU2 emulation (`ps2xIOP/.../iop_spu2.cpp`: 2x24 ADPCM voices, ADSR, loops,
      IRQ address, AutoDMA, IOP DMA 4/7) mixed to a 48 kHz host audio stream (SDL3 since stage 1 of NATIVE_RENDERER.md). Music, SFX and
      voices confirmed by ear. IOP time follows the wall clock, so audio no longer stutters
      when the game drops below 60 VBlanks/s (0 underruns at 20-60 VBlanks/s).
- [x] Audio: reverb, noise, pitch modulation, Gaussian interpolation, exact volume sweeps / ADSR timing
      (2026-10-23, from psx-spx; PS2RECOMP_PATCHES.md). VCS keeps reverb off and uses no PMON / NON in gameplay
      (game-internals.md 9), so what is audible is the interpolation and the envelopes
- [x] Memory card save: the game's own save routine writes its folder, icons and slot file
      to `%LOCALAPPDATA%/VCSRecomp/mc0` (verified with `VCS_TEST_SAVE`); boot finds the save
- [x] Memory card load (2026-10-22): the game finds its saves at start-up and loads the newest
      ("Load Successful", back at the save point) since `sceMcGetDir` lists folders in card order
      (PS2RECOMP_PATCHES.md); before, saves were written but never seen. Saving over a slot replaces its
      file (2026-10-23). Save (slot 2), Load Game (slot 1) and Delete Game through the game's own menus all work
      (2026-10-23, `tools/saves.py`, regress step "saves"; game-internals.md 10e)
- [x] Frontend menus render (pause menu, LOAD/NEW/DELETE menu when a save exists)
- [x] World vanishing when moving fast / driving: HW renderer never cleared the bottom half of
      the Z buffer (see PS2RECOMP_PATCHES.md, gs_hw.cpp depth fill)
- [x] Vic / vehicles / HUD flickering out in busy views: DMA chains were cut at 4096 tags.
      Cost: full frames are 7-14k tags; the title flyover dropped from ~45 to ~16 game fps.
      First perf recording: render submit (VIF/VU1/GS recording) 41 ms of a 61 ms frame,
      game code 15 ms, IOP 0.7 ms, GL execution 2.5 ms on the main thread.
- [~] Performance: render submit (title flyover, frames 10-60 s, ~245k vertices/frame):
      | step | render ms | game fps |
      |---|---|---|
      | start (full DMA chains) | 44.6 | 15.8 |
      | recycle HW frame records, chain buffers; perf counters per frame | 39.1 | 17.5 |
      | slice-addressed texture uploads | 39.9* | 16.9 |
      | lazy uploads (block level) | 36.8 | 18.3 |
      | texture binding cache | 33.9 | 19.3 |
      (* flyover load varies run to run; compare per-vertex.) Next: HwBackend::Submit (7%),
      VIF1 unpack (5%), compiled-VU1 pipeline bookkeeping (runLoop / progressXgkick /
      commitReadyPipelines ~8%), GS::writeRegisterPacked (4%), lazy-upload closure (3%).
- [x] Whole screen goes black for a few frames now and then - **no longer seen** in the user's testing
      (2026-10-24); gone with the renderer / timing work since (which change is not known). The report: (2026-10-22, after the
      DMA chain fix; not investigated yet). Still seen by the user after the native renderer
      (2026-10-22): objects and the world turn black for a few frames. To check first: whether
      it follows native draws (PS2X_HLE=0 run for comparison), the prelit colour stage inputs
      (0x9c-0x9e tint) and lighting constants at those moments, texture cache misses, and
      render-target drops (RtDrop) around the black frames.
- [x] Water surface looks missing (dark grey where sea should be): the sea draws (turquoise, waves) in the
      2026-10-23 tours - gone with the render-target fix (milestone 2)
- [x] Movies (milestone 5, 2026-10-24: played natively)
## Milestone 1 - automation and coverage rig (NATIVE_PORT_ROADMAP.md), started 2026-10-22
- [x] Control-channel extension (`ps2control::addCommand`) and the rig (`src/debug/rig.cpp`, `tools/rig.py`):
      memory peek / poke / search / dump, pause, frame counter
- [x] Script VM mapped (game-internals.md 11): `scm` runs our bytecode as a game script; teleport (area, scene
      load, ground find), time, weather, invincibility, health, mission launch (game's own loader) all work
- [x] Player ped, current area (interior) found (game-internals.md 12)
- [x] World tour (`tools/tour.py`): grid over the map, classes ok / water / relocated / falling / dark / slow /
      error, map image, screenshots, perf; restarts the game on a crash
- [x] Brightness probe (`PS2X_FRAME_STATS=1`, on with the control channel): every presented frame's middle
      region shrunk to 32x24 and read back; perf line `black_frames` / `dip_frames`; log lines
      `[frames] black run ...` (whole picture black) and `[frames] dip ...` (much darker than the scene for up to
      2 s). Normal walking / running: 0 of ~3000 frames; driving: one-game-frame full-black flashes seen
      ("dip of 2 host frames ... scene luma 113, lowest 0.9") - the user's black-flash bug, now measurable
- [x] Mission smoke test (`tools/missions.py`): each mission launched from a fresh load, screenshots, perf
- [x] Drive loop (`tools/drive.py`): car spawned by script, driven with the steering alternating
- [x] Regression suite (`tools/regress.py [--set-baseline]`): scenes + tour subset + missions + drive against a
      baseline, report.md
- [ ] Soak runs; in-process falling detector; unconverted-draw counter; map image with the land outline

## Milestone 2 - correctness sweep (started 2026-10-23)
Findings so far (rig runs; details in game-internals.md):
- [x] **Evening black frames** (the user's "world turns black"): fixed 2026-10-23 - render targets were tracked
      taller than the scissor, so the Z-area texture the game uploads at night dropped the picture
      (game-internals.md 6g). Regression check: `tools/regress.py` step "evening".
- [x] After a death the world near the hospital drew only lights for ~40 s - it was the evening black frames
      (fixed with them; checked 2026-10-23: drawn from the first frame after the respawn fade).
- [ ] Frame time in busy scenes: 35-39 ms (game code ~18 ms, render submit ~16 ms), drive loop mean 39 ms (was
      54 before the render-target fix). Profile (busy street, 2026-10-23): VIF1 processing 8%, T4 texture
      swizzle 7%, scheduler checkpoints 4%, the VU1 interpreter for the programs still on it (sprites 0x02c0, the
      env-map pass) ~8% together - milestones 3 and 7.
- [x] The VU path's darker noon picture was another symptom of the dropped render targets: after the fix native
      and VU path agree (hospital, luma 90 / 95 at 12:00, 73 / 76 at 20:00).
- [x] Black flashes at teleports / car warps are the game's own load transitions (not bugs).
- [x] "Relocations" after loading: the safehouse script (rig.Game.prime).
- [x] Sea: drawn correctly everywhere the full tour went (240 points, 2026-10-23); the old "dark grey sea" was
      the dropped render targets too.
- [x] Audio: the missing SPU2 features are implemented (reverb, Gaussian interpolation, PMON, noise, exact
      envelopes / sweeps) with unit tests; the game itself uses none of reverb / PMON / noise (traced over
      gameplay and missions 36-54: 0 of 532 samples) - it is a PSP game first.
- [x] Saves: save to another slot, Load Game, Delete Game through the game's own menus (`tools/saves.py`, regress
      step "saves"); a card written by older builds (the user's) loads in the current one.
- [x] Full mission smoke test (`tools/missions.py`, missions 6-100, 2026-10-23): 95 / 95 ok (started, no crash or
      hang, pictures); dark shots were fades and night cutscenes except one bug:
- [x] **GON_C3 poolside ground black** on the native path: GPU clipping of the packets the VU sends through its
      clipper. Clipped world packets stay on the VU1 by default now (`PS2X_HLE_CLIPPED=1` for GPU clipping); cost
      flyover 30 -> 25.5 game fps (render 18.2 -> 24.6 ms), cutscene / gameplay still at 30. Reproducing the VU's
      per-triangle clipper natively is milestone 3 (game-internals.md 6h).
- [x] Soak (`tools/soak.py --hours 0.5`, 2026-10-23; the user settled on a short one): 60 samples over 30 min of
      autodrive across six spots, no crash, hang or falling; private memory 347 MB at start, then a saw-tooth between
      486 and 560 MB from minute 10 on (streaming), no drift. Driving frame time median 46.8 ms, p90 70.7 ms. (The
      "6-8 ms worse with clipped packets on the VU1" read from this was wrong: a same-session A/B of `drive.py --auto`
      gave 54.8-57.1 ms mean for GPU, VU1 and C++ clipping alike - driving runs vary more than the clipping costs.)
      One car per leg spawned against something and was replaced (the tool's retry).

Milestone 2 closed 2026-10-23. Open, carried to milestone 3: the VU clipper reproduced natively, bloom buffers
dropped on uploads (lazy render-target patching).

## Milestone 3 - native rendering, the rest of the frame (started 2026-10-23)
- [x] **The VU1 clipper in C++** (`src/game/vcs_vu_clipper.cpp`, game-internals.md 6h): bit-exact against the VU's
      XGKICKs (GON_C3 1.2M GIF packets, reference scenes 2.8M, 0 off); clipped world packets drawn from it. Section
      means over the reference scenes (C++ / VU1 / GPU clipping): flyover render 14.6 / 17.7 / 14.1 ms, cutscene
      11.8 / 14.5 / 11.3, gameplay 5.9 / 7.6 / 5.6; flyover frame 34.3 ms (30 fps cap) vs 36.2 on the VU1. GON_C3's
      poolside ground right natively.
- [x] **Environment-map pass** (stage table 0x13, vehicles): reproduced and checked against the VU's kicks (27,243
      packets exact); native by default (`PS2X_HLE_ENV=0` = VU1). Flyover render 14.7 -> 14.0 ms (same-session A/B);
      pictures identical. game-internals.md 6d.
- [x] Survey reports native and VU1 work apart (`PS2X_HLE_STATS=1` also lists the stage tables left).
- [x] **0x02c0** (instanced re-draws, entry 0x1c; two-pass shine over resident meshes, entry 0x0d) and the **leftover
      prelit packets** (a float code image, "variant C") native: reproduced with their VU memory writes and checked
      against the VU (174,110 packets exact over the reference scenes; two missions and a drive exact too). Only clipped
      environment-pass packets still run on the VU1 (63 in two minutes of scenes). Render time 0.3-0.7 ms lower per
      section; pictures identical (game-internals.md 6d). `PS2X_HLE_REDRAW=0` = VU1. Milestone 3 step 1 done.
- [ ] **Step 2: 2D at the RenderWare level** (started 2026-10-23). Found: Im2D (FUN_002fbe08 / FUN_002fbe68), its
      vertex format, RwRenderStateSet, CHud::Draw (FUN_0031c628) and the 2D pass's order (game-internals.md 6f);
      `src/game/vcs_hud.cpp` hooks them.
  - [x] **16:9 weapon icon at the screen edge** (reported by the user): the icon is drawn off the far plane, so the
        renderer's HUD placement missed it; HUD-context primitives without a depth test now get the HUD's z. Checked at
        16:9 (icon beside the clock / ammo as at 4:3, radar still round) and over the reference scenes (no other
        change). `VCS_HUD_RW=0` = before.
  - [x] **Per-element anchoring**: HUD "Screen edges" (`VCS_HUD=2`) - each HUD element squeezed towards the edge it
        sits near (radar left; clock / weapon / money / zone name right; help text left), the elements found from the
        frame's Im2D calls (game-internals.md 6f, DECISIONS.md).
  - [x] **Screen edges: text and radar jumping** (reported by the user, 2026-11-01): subtitles, mission text and the
        stunt results at the bottom switched between the left, middle and right, the radar sometimes going to the
        middle with them. The radar's map tiles reach far past its circle, so text near it was grouped with the radar
        and the pair's anchor flipped as the map turned. The radar and centred lines are now known by who draws them
        (game-internals.md 6f "Radar and text"): the radar always left, centred lines always in the middle (the stunt
        results' right-shifted rect moved there too). `tools/hud_check.py` (airport jumps at 16:9): 1240 frames, radar
        always left, centred lines in 426 frames, all centred on x 320.
  - [x] **Native HUD layer**: the HUD's primitives kept out of the PS2 frame buffer and drawn over the shown picture
        at the window's size (NATIVE_RENDERER.md "HUD layer"); on by default, `VCS_HUD_NATIVE=0` = into the frame
        buffer. Textures from the GS backend's decoded cache, not a RenderWare raster decoder (DECISIONS.md).
        Checked with `scenes.py` at 4:3 and 16:9 against `VCS_HUD_NATIVE=0` (build/m3/cmp43, cmp169): same HUD, the
        radar round and clipped, the static scenes (loading 0.95, pause map 0.8-0.9 mean difference) as close as two
        runs of one build; at 16:9 the HUD is sharper (no squeeze-then-stretch). No cost: 4:3 flyover 29.3 -> 30.0
        fps, gameplay at the 30 fps cap in both.
  - [x] **Lines in the HUD at full-screen sizes** (reported by the user at 1080p: a line through the radar's north
        icon, lines under / beside the clock's digits): sub-pixel gaps and atlas bleeding that 640x448 never showed.
        The layer now covers the PS2's pixels and samples inside each quad's own texels (NATIVE_RENDERER.md "HUD
        layer"). Checked at 1920x1080 (`PS2X_HW_OUTPUT`) outside the barracks; at 640x448 the layer still matches the
        emulated HUD.
  - [x] **Menus** in the native layer (pause menu pages; NATIVE_RENDERER.md "HUD layer"): sharper at 1080p, same
        layout and colours (scenes at 16:9 1080p against `VCS_MENU_NATIVE=0`: static scenes the same but the pause
        map, which differs by its sharper text). HUD placement default stays "Keep 4:3"; "Screen edges" is the menu
        choice Display > HUD (user's decision).
  - [x] **Loading screens** in the native layer (2026-10-24; FULLY_NATIVE.md step 2): `LoadingScreen` (FUN_0021E170)
        opens a context like the menu's (centred, full-width pieces stretched) to its frame kick. Static `loading`
        scene 1.12 against the reference (ok); at 1080p the progress bar and its shadow draw at the output's
        resolution instead of a scaled-up 640x448 frame. `VCS_LOADING_NATIVE=0` = into the frame buffer. There is no
        title menu (game-internals.md 6f: the game boots into the opening sequence; the frontend is the pause menu).
- [x] **Textures without emulated VRAM in steady state** (2026-10-24, FULLY_NATIVE.md step 3a): CLUT loads take the
      palette from the queued upload that wrote it, and data-less upload calls no longer force the queue; swizzled VRAM
      writes while driving ~245k -> ~7 per 5 s, render worker idle 27% -> 41% on the drive (`writeRectFast` was 11% of
      its samples). Verify: 2.7M CLUT loads the same as from VRAM. Scenes ok, tour classes unchanged (frame p90 19.1 ->
      15.6 ms). `PS2X_HW_CLUT_FROM_UPLOAD=0` = from VRAM.
- [x] **Streamed uploads cheaper** (2026-10-24, FULLY_NATIVE.md step 3b): PATH2 packets processed in place, packet / upload
      buffers reused, a four-lane upload hash; render worker busy on the drive ~59% -> ~50% of samples, standing worker
      -0.3 to -0.5 ms (A/B, alternating).
- [x] **Textures decoded from their uploads** (2026-10-24, FULLY_NATIVE.md step 3c): a texture cache miss reads the
      queued upload's data at each texel's VRAM address (`GSMem::PixelBits`) instead of emulated VRAM; ~4,100 decodes over
      the reference scenes identical to VRAM's. Left in VRAM: the loading screen's art (its upload arrives in pieces).
      `PS2X_HW_DECODE_FROM_UPLOAD=0` = from VRAM.
- [x] **Textures named by raster, render targets by role** (2026-10-24, FULLY_NATIVE.md step 3d): every texture made
      over the reference scenes and the drive named by the game's raster (RAM address) through the chain's upload REFs;
      targets named by the game (`gsHwSetTargetRole`), the reflection map at the window's resolution. Step 3 done.
- [x] **Unchanged world / model blocks not copied** (2026-10-24, FULLY_NATIVE.md step 4a / 4b): guest RAM under the
      OS's write watch; a block whose pages are unchanged since a confirmed copy is replayed from its recording without
      being copied (2.4M decisions checked, 0 wrong). Chain copy per frame 4.6 -> 2.1 MB; drive 50 -> 54 fps, worker
      -0.5 ms (A/B alternating). "Other call" packets skipped too since 2026-10-25 (a 12-byte
      over-skip in the MARK handler had lost their marks; 2.06M decisions checked, 0 wrong). `VCS_RSL_SKIP=0` = copy everything.
- [x] **Draw calls as items** (2026-10-25, FULLY_NATIVE.md step 4c; step 4 done): world / model draws with a native
      flush write one 16-byte CNT (MARK) instead of their GS packet and CALL / REF; flush, GS registers and block go to
      the worker as an item, the block decided at the kick. Sectors cached by `FUN_00240ED8` keep their packets (the game
      sends the copy again). Verify: 3.0M draws / 1.4M blocks 0 differ. Speed even (the copy savings came with 4b).
      `VCS_RSL_ITEMS=0` = packets.
- [x] **State setters as item ops** (2026-10-25, FULLY_NATIVE.md step 5a): the GS queue senders, texture matrix,
      `FUN_001CEF18`, `FUN_002F9CE8` and all-CNT state flushes become VIF-byte ops in the item stream (not while a
      sector / material is cached). Verify 2.67M draws / ops 0 differ. `VCS_RSL_OPS=0` = their packets.
- [x] **Texture uploads as reference ops** (2026-10-25, FULLY_NATIVE.md step 5b): `FUN_003298D8`'s header and pixel
      REFs become references in the item stream, their payloads copied at the kick with their raster names. Verify
      4.85M draws / ops 0 differ; chain payload ~22k -> ~7.9k qwords a frame. `VCS_RSL_UPLOADS=0` = their REFs.
- [x] **Im2D as ops** (2026-10-25, FULLY_NATIVE.md step 5c): `FUN_002FB690`'s packet, as the game writes it, becomes an
      op at the next hook point (cursor rewound). 803k captured packets unchanged at the kick; HUD / pause-map scenes as
      before. `VCS_RSL_IM2D=0` = the packets.
- [x] **The other packet writers captured** (2026-10-25, FULLY_NATIVE.md step 5d): 16 effect / immediate-geometry /
      frame writers captured like Im2D, nesting told apart by the guest stack pointer, a region's convertible front
      taken when other packets follow it (NEXT over them). 766k regions unchanged at the kick. Drive census ~1,090 ->
      ~740 tags a frame.
- [x] **VU1 code uploads and double-buffer setup in recordings** (2026-10-25, FULLY_NATIVE.md step 5e in part): BASE /
      OFFSET / ITOP / MPG recorded as raw VIF bytes and replayed through the interpreter; the render-context packets
      ("other calls") all replayable now (15.6k refused per 10 s before). 1.79M recordings 0 differ. `VCS_RSL_RAW=0`.
- [x] **Plug-in callbacks in the native flush** (2026-10-25, FULLY_NATIVE.md step 5f in part): the two `+0x50`
      callbacks that write constants (`FUN_001CEF18`, `FUN_0040F6C8`) written natively (~50k flushes per 5 s driving);
      ~36k of ~870k flushes per 10 s still left to the original. 1.62M flushes 0 differ. `VCS_RSL_PLUGINS=0`.
- [x] **Captured regions take CALLs; fallback draws captured** (2026-10-25, FULLY_NATIVE.md step 5f done): a CALL in a
      captured region becomes the block of a continuation item (copied / skipped at the kick like a model item's); draws
      whose flush calls guest code run the original and their region is captured; draws inside regions leave the
      frame's draw list (the walker's matching was out of step: model items 31k -> 90k per 10 s driving). Im3D all ops.
      1.51M regions, 3.38M draws / ops, 640k blocks, 1.54M recordings: 0 differ. `VCS_RSL_DRAW_CAPTURE=0`,
      `VCS_RSL_CAPTURE_CALLS=0`.
- [x] **Gaps and runs** (2026-10-26, FULLY_NATIVE.md step 5h): what any code writes between two hook points is taken
      into items in place; regions taken in runs around the tags that must stay; texture binds captured. Driving chain
      742 -> 303 top-level tags a frame (245 items, ~32 NEXTs, ~16 fixed root tags), payload 780 -> 232 qwords. 2.15M
      regions unchanged, 0 tags reading inside one, 3.34M draws / ops, 631k blocks, 1.40M recordings: 0 differ.
      `VCS_RSL_GAPS=0`, `VCS_RSL_RUNS=0`.
- [x] **No GS front-end draws** (2026-10-26, FULLY_NATIVE.md 5e): the three render-target reads (A+D runs continuing
      an earlier PRIM, state writes before PRIM) and the flyover's Im2D line strip drawn by the packed path; 7.5M
      packets exact against per-primitive `Submit`. `gs_draws` 0 in the reference scenes and the tour subset.
- [x] **Native in every mission** (2026-10-26): mission smoke test, all 95 missions ok (the 14 whose script ends
      within 45 s of a fresh load do so on the earlier build too); GS-path draws 0 everywhere after MAR_A1's second
      0x0030 code image was accepted (verify exact: 1.55M packets). FULLY_NATIVE.md "Where it stands": point 2 of
      "done" holds. `PS2X_HLE_IMAGE2=0`.
- [ ] **Step 3: post effects** (started 2026-10-23; NATIVE_RENDERER.md "Post effects", game-internals.md 6g).
  - [x] **GS pixel centres**: the renderer sampled half a pixel off the GS, so VCS's per-frame copies blurred the
        picture (softer than the PS2 at 640x448). Fixed for every draw; `PS2X_HW_PIXEL_CENTRE=0` = before.
  - [x] **Higher internal resolutions kept through the copies** (`rtFull`): before, the shown picture was always a
        640x448 copy scaled up. `PS2X_HW_RT_FULL=0` = before.
  - [x] **Bloom and trails as native passes** with widescreen-correct proportions (NATIVE_RENDERER.md "Post
        effects"): at 4:3 640x448 the same picture as the emulation (static regions within run-to-run noise), at 16:9
        the glow / warp no longer a third wider than at 4:3. `PS2X_HW_POSTFX=0` = as recorded.
  - [x] Frame copy and motion blur as native passes (texture copy / one full-screen quad); the self-copy over the
        sky stays a GS draw (depth-tested, read at full resolution).
- [ ] **Step 4: native frame** (started 2026-10-23; NATIVE_RENDERER.md "Native frame").
  - [x] Per-target scales; "Internal resolution: Match window" draws the picture's targets at the window's size
        (1920x1080 at 16:9 1080p), the effect buffers at 1x. Default stays 1x (the original).
  - [x] MSAA (2x-8x) and anisotropic filtering (2x-16x) as options, off by default (NATIVE_RENDERER.md "Native frame").
  - [x] The reflection map at the window's resolution with "Match window" (2026-10-24, FULLY_NATIVE.md step 3d); the
        radar has no target of its own (drawn in the HUD layer, already at the window's resolution).
- [x] **Draw-source counter** (2026-10-24; NATIVE_RENDERER.md "Draw sources"): what each frame still draws through
      the GS path, by source (VU1 program, C++ reproduction, PATH2, PATH3) - perf line `gs_*`, overlay, the rig's
      `gsdraws`, tour / missions record it. Survey: no PATH3; PATH2 ~15 draws (reflection map, coronas, effect quads);
      45-90 draws from the C++ reproductions' GIF packets; VU1 draws only from clipped / culled 0x02c0 shine packets.
- [x] **Clipped and culled 0x02c0 shine packets natively** (2026-10-24, game-internals.md 6d): the second pass is the
      clipper again with tag 50. PHI_A1 verify: 421,694 packets exact (9,629 clipped, 26,518 culled shines). VU1 draws
      0 in the tour subset and 12 missions.
- [x] **VU1 XGKICKs went to the GS twice** (found with the counter; PS2RECOMP_PATCHES.md): `PS2X_HLE=0` scenes drew
      ~2x the vertices (cutscene 220,848 -> 122,823). Default path unchanged (scenes 0.00 static).
- [x] **VU1 setup programs natively** (2026-10-24, game-internals.md 6d "Setup programs"): 0x0000, 0x0148, 0x01e0,
      0x0250, 0x03a8, 0x0418, 0x0478, 0x2b50 and the slot routines 0xc8 / lit 0x4e0 / lit 0x57e, each tied to a hash of
      its code; verify exact against the whole VU memory (reference scenes 434,692, PHI_A1 217,489, a city spot
      80,557). Two more 0x02c0 code images accepted (they differ only in code the shine never runs). VU1 programs a
      frame: cutscene 195 -> 4, gameplay 86 -> 4, tour subset mean 104.6 -> 3.4 (worker 8.76 -> 8.05 ms); no VU1
      draws anywhere in the tour subset or the 12 missions. `PS2X_HLE_SETUP=0` = VU1.
- [x] **World draws from the RSL world draw calls** (2026-10-24, milestone 7 / RSL_SUBMISSION.md phase 2): the world
      draw's REF replaced by a VIF MARK as the chain is copied, the block drawn there from a recording (UNPACK results,
      MSCALs, meshes) - no copy, VIF parsing, unpacking, hashing or vertex conversion of world geometry. Verify: 174,301
      replays against the packet path, 0 differ. Timing within noise (world geometry is ~5% of VIF1's payload).
      `VCS_RSL_WORLD=0` = packet path.
- [x] **Model instances from the RSL draw calls** (2026-10-24, RSL_SUBMISSION.md phase 2): the CALLed packets of
      `FUN_002F7D58` / `FUN_002F80A0` (and every other CALL in the packet buffer) recorded once - the whole CALL ... RET
      stream, DIRECT GS data, STCYCL, all draw programs - and replayed at a MARK the chain walker puts before them. The
      marked bytes now stay in the chain's copy and are skipped (world blocks too: the old REF replacement read guest
      memory when the worker processed the chain, possibly after streaming replaced it). Replays write only the fields
      an UNPACK wrote (found by the verify: V3 / write-protected w lanes). Verify: 1,217,887 blocks over a bench run,
      0 differ. Render worker (one bench run each): Fort Baxter 5.5 -> 4.2 ms, busy city spot 10.8 -> 9.2, drive
      15.8 -> 13.4. Scenes ok, tour classes unchanged. `VCS_RSL_MODELS=0` = packet path.
- [x] **Game thread: chain copy, VU0, state flush** (2026-10-24, RSL_SUBMISSION.md "The game thread after phase 2"):
      a caller-aware drive profile found the VIF1 chain copy reallocating every frame (pool fixed: ~10% -> ~5.7% of
      the game thread), four VU0 programs still interpreted (0x0C10, 0x0068, 0x0E98, 0x0E30: native, `PS2X_VU0_VERIFY`
      0 differ; ~5%), and the RSL state flush (native for flushes without plug-in callbacks, byte-exact over ~700k
      flushes; 2.3% -> 1.3%). A/B, two alternating rounds: drive game thread 18.6 -> 17.8 ms, 45.8 -> 51.0 fps.
      Scenes ok, tour classes unchanged. `VCS_RSL_NATIVE=0` = the original flush.
- [x] **Packed draws** (2026-10-24, NATIVE_RENDERER.md "Draw sources"): the C++ reproductions' strips / fans recorded
      without the GS front end (`GS::submitPackedKick`), verified exact against the per-vertex recording (14M packets).
      Render worker at the busiest bench spot 14.5 -> 11.7 ms, AI drive 21.2 -> 18.2 ms (one bench run each).
      `PS2X_HLE_PACKED=0` = front end.
- [x] **No VU1 in a normal frame** (2026-10-24, game-internals.md 6d): the Z-clear image's programs (0x0000, 0x0200,
      0x0400), the last 0x0250 (slot routines now named by the hash of their code) and table-0x13 packets through the
      clipper reproduced; verify exact (PHI_A1 setup programs 160,468; reference scenes env pass 23,913 with 216
      clipped). `vu1_programs` 0 in the reference scenes and at all 18 tour-subset points. The emulated VU1 stays as
      the reference (`PS2X_HLE=0`, `PS2X_HLE_SETUP=0`, `PS2X_HLE_VERIFY=1`).
- [ ] Rigid / skinned lit models still use GPU clipping for their clipped packets (no wrong picture seen yet; same
      method when one shows up).
- [ ] Lazy render-target patching (bloom buffers dropped on uploads).


## Milestone 4 - frame rate (started 2026-10-23)

- [x] Main loop, VBlank handler, frame limiter and CTimer mapped (game-internals.md 4).
- [x] 60 fps switch (Display > Frame rate, `VCS_FPS=60`; off by default): the limiter lets a frame through after one
      VBlank. On the i7-7700K it reaches 31-37 fps in gameplay: the game thread needs ~27 ms a frame (milestone 7
      first for a steady 60).
- [x] Performance (milestone 7 work, 2026-10-23): VU0 collision programs 0x020 / 0x3A0 native (bit-exact): game code
      19-24 -> ~12 ms a frame; 60 fps mode 31-37 -> 45-50 fps outside the barracks (render submission ~8 ms is next).
- [x] Render submission, round 1 (all checked against the old path): VU1 code-write log (only re-uploaded ranges
      re-decoded); UNPACK fast path (`PS2X_VIF_UNPACK_FAST=verify`); rect uploads with hoisted addressing and whole
      PSMT4 blocks (`PS2X_GS_RECT_VERIFY=1`); lazy uploads found through a per-block index (`PS2X_HW_LAZY_INDEX=0` for
      the old scan). 60 mode outside the barracks 45-50 -> 56-60 fps (frame 16.6-17.8 ms: game ~10.5, render ~6).
      Driving (AI car from Fort Baxter, `fpsdrive.py`) is the hard case: 24-28 -> 26-29 fps, ~1,700 draws a frame,
      game ~18 ms + render ~17 ms one after the other on the game thread; 30 mode also dips to 27-28 there. Holding 60
      when driving needs render submission off the game thread (VIF1 / VU1 / GS front end on a worker), not more
      trimming.
- [x] Render submission worker (2026-10-23, NATIVE_RENDERER.md "Render submission worker", `PS2X_RENDER_THREAD=0`
      for the old path): chains, VU1 and the GS front end run on their own thread behind the game thread. 60 mode:
      outside the barracks at the cap with ~4 ms to spare; driving 26-29 -> 44-54 fps; flyover 42.5 -> 59.9 fps.
      Game code (~17-20 ms driving) is now the limit.
- [x] Game code (2026-10-23): inline dispatch / checkpoint fast paths (`PS2X_FAST_DISPATCH=0` to compare) and a
      ThinLTO + profile-guided build (`tools/build.py --opt pgo`, trained on `tools/bench.py` + the reference scenes).
      `tools/bench.py`, 60 mode, game-thread ms (two runs each):

      | spot | plain /O2 | + fast dispatch | + PGO + ThinLTO |
      |---|---|---|---|
      | Fort Baxter | 10.8 / 11.0 | 13.1 / 13.3 (more draws) | 9.7 / 10.8 |
      | city 575,175 | 7.9 / 8.2 | 6.7 / 7.5 | 5.9 / 6.0 |
      | city -675,1425 | 14.5 / 13.6 | 13.2 / 14.2 | 9.1 / 9.3 |
      | city 1325,-325 | 9.6 / 11.4 | 8.4 / 8.9 | 7.2 / 7.5 |
      | city -1425,-575 | 12.2 / 12.2 | 10.5 / 11.3 | 8.0 / 8.3 |
      | AI drive | 22.9 / 21.8 (40.6 / 42.6 fps) | 20.3 / 20.3 (45 fps) | 16.4 / 16.9 (56.7 / 55.1 fps) |

      Fast dispatch alone is ~1 ms (dispatch / checkpoint / lookup left the profile but their work stays inline);
      PGO + ThinLTO ~20-30% of game code. Checked: static reference scenes unchanged (loading 0.00, pause map 0.33),
      world tour subset classes unchanged, its worst frame 21.0 -> 16.7 ms. Remaining on the game thread (driving):
      game code ~65%, memmove ~3%, VU0 programs ~6%, IOP ~2%.

      Retrained 2026-10-24 (after RSL phase 2 and the game-thread work: `--opt pgo --train`, ~45 min). A/B against
      the same code without PGO (`--opt none`), two alternating rounds: standing at Fort Baxter 11.7-12.9 -> 8.1-9.3 ms,
      AI drive 19.4-20.2 -> 14.8 ms (44.6-46.9 -> 56.8-56.9 fps). Reference scenes all ok (a first capture right
      after the build caught `loading` / `flyover_base` on black / mid-fade frames - timing; the next capture matched,
      `loading` at the usual 1.17).
- [x] Frame-rate independence, phase 1 - timing (2026-10-23): Display > Game timing (Original / Frame-rate
      independent, restart) and Frame rate cap (30 / 60 / 120 / 144 / display refresh / unlimited). Independent:
      host-clock CTimer in whole ms, the 0.5 minimum step lifted, host pacing in the limiter, frames presented as the
      game finishes them, EE clock kept at real time on the VBlank chain. Game clock = wall clock at 30 and 144
      (was 1.5-4% slow from CTimer's per-frame ms truncation, in original timing too). Original timing unchanged
      (static reference scenes 0.00). game-internals.md 4.
- [x] Frame-rate independence, phase 2 - audit rig (2026-10-24): `tools/framerate_audit.py` runs fall / run /
      sprint / jump / car / clock scenarios at forced steps (`VCS_TIMESTEP`), compared per simulated second.
      Findings: steps below 0.5 break ped movement (independent timing capped at 100 fps, DECISIONS.md); in
      1/15-1/100 car acceleration and running agree, falling and jumping vary with the rate (+7% fall time,
      +6-9% jump height from 30 to 60-100). game-internals.md 4 has the numbers and the frame counter worklist.
- [x] Measured on the PGO + ThinLTO build (2026-10-24), independent timing: game clock = wall clock (x1.001) at
      caps 60 and 100; cap 100 runs 91 fps outside the barracks, `tools/bench.py --fps 100 VCS_TIMING=1`: city
      spots 80-100 fps, AI drive 68 fps (game 13.7 ms, worker 14.0 ms). Original timing unchanged (static
      reference scenes 0.00).
- [x] User playtest (2026-10-24): missions play well in frame-rate independent timing at 60 and 100 fps.
- [ ] Well past 100 fps everywhere (started 2026-10-25, docs/PERFORMANCE.md). Done: guest memory accesses with one
      compare and the scratchpad inline (game thread ~2x faster), lean pc and `__restrict ctx` in the recompiler, the EE
      clock no longer holding the game at ~100 fps, the flip's CSR read without the render-worker fence, the main loop
      unpaced for an unlimited cap. Uncapped (`--opt none`): Fort Baxter 111 -> 209 fps, city spots 118-166 -> 135-331,
      AI drive 95 -> 126 fps. Then the ped-movement bug below step 0.5 patched: independent timing goes up to 240
      fps (was 100). Then the render worker split in two (the GS stage), guest functions without register saves
      (preserve_none), smaller game-thread items, PGO retrained. PGO build, independent timing, unlimited (= 240):
      Fort Baxter 223 fps, city 201-240, AI drive 150, world tour (40 points) median 208 / slowest 120; 30-minute
      uncapped soak clean. Left: heavy views (~1,950 draws) at ~120 fps - game thread and render worker both ~6-8 ms.
- [ ] Frame-rate independence, phases 3-4. Done: falling / jumping explained (Euler integration error, <= 6%, not a
      bug - game-internals.md 4); real-time pacing checked (game clock = wall clock at caps 60 / 100 / refresh,
      presents at the cap). HUD / radar flashing at the original rate above 30 fps (2D pass on a 30 Hz frame counter). Left: the four
      blinking readers outside the 2D pass;
      what breaks below step 0.5 (FUN_00337208's max(step, 0.5), CBike's lean, patched 2026-10-28) to lift the 100 cap; missions at 60 / 100
      (driving, shooting, flying, cutscenes); VRR / frame-time variance. (Phase 1 / 2 above; the user's choice over interpolation, DECISIONS.md 2026-10-23): CTimer on the host
      clock, a frame-rate cap setting, a forced-step test rig, then the audit of frame-count assumptions. Replaces
      the "60 fps fixes" item below.
- [x] Mission meters on the time step (2026-10-27, user report: PHI_A2 "Boomshine Blowout"'s heat bar filled far
      faster at 144 fps). The mission adds fixed amounts per frame (heat floor +0.015, heat +-1); a survey of all of
      MAIN.SCM (disassembled: every bar's variables, every per-frame float rate, every int counter) found the same in 11
      more: LOU_B1 girls, UMB_B2 havoc, DIA_C2 time, REN_C3 Phil, UMB_B3 stress rates, FIREHEL water / heli damage,
      MEN_C6 pressure, the driving range / GON_C2 aim, DIA_C4 searchlights, LOU_B2 escape distances. 30 instructions
      scaled by step / (50/30) in the script VM's arithmetic handlers (`src/game/vcs_script_timing.cpp`), only when
      the frame isn't the PS2's. PHI_A2 at 1/144: heat floor 2.162 -> 0.4496 a second (0.450 at 1/30), heat at +1:
      144 -> 29.5 (29.1 at 1/30). `framerate_audit.py` scenario `heat`, `--sites` (all 30 found). game-internals.md 11f.
- [x] Vehicle physics on the time step (2026-10-28, user report: at high frame rates a car running over someone jumped
      far more than it should, and a stolen bike sometimes flipped forward). Two per-frame amounts without the step:
      FUN_003F34F0 pushes a wheel up once a frame while a dead or knocked-down ped is under it (the bump of a body;
      cars and bikes), and ProcessWheel's rolling resistance (no gas, no brake) is a fixed brake a frame (reVC fixes
      the same line under FIX_BUGS). At 144 fps a car over a body pitched up until its rear bumper hit the road and
      threw it up at 3.9 m/s (1.4 at 30), a bike rolling past the rider it was stolen from ended on its nose or side,
      a car coasted down at 16 m/s^2 (3.4) and a bike at 11 (2.4, hard enough to pitch it forward). Both scaled by
      step / (50/30) when the frame is shorter (`src/game/vcs_physics_timing.cpp`, `VCS_PHYSICS_TIMESTEP=0` off; the
      brake's constants patched to load from guest RAM 0x7FF00..), and CBike's lean `max(step, 0.5)` patched like the
      ped one (a gentle turn leant 22 degrees at 144 fps against 30). After: coasting 3.45 / 3.45 / 3.45 m/s^2 at 1/30
      / 60 / 144, the run-over pitch rate 0.042 / 0.043 / 0.041, the bike past a body upright at all three.
- [x] Ferris wheel and escalators above 50 fps (2026-10-31, GitHub issue 10: Vic fell out of the ferris wheel at 60
      fps). The wheel (FUN_00175620) and the escalators (FUN_002469A0) give their objects the move speed `displacement /
      max(step, 1.0)`, step times too small above 50 fps (0.35 at 144); a ped on them moves with that speed, so he
      slid off - out of a carriage within 2 s at 144 fps. Patched to max(step, 0.001) (overrides.toml; steps >= 1
      unchanged). `framerate_audit.py` scenario `ferris` (seconds he keeps his place in a carriage, of 10): 10 / 10 / 10
      at 1/30 / 60 / 144, 1.3 at 144 unpatched (11 m off). An escalator step's move speed at 144 equals 30's (0.0147 /
      0.0148). game-internals.md 4.
- [x] Follow cameras above 50 fps (2026-10-31, found with the ferris wheel's `max(step, 1.0)`): the on-foot camera
      (FUN_0029DAB0) and the vehicle / boat camera (FUN_0029FAF0) turned themselves behind the player step times too
      slowly (the stick and mouse were right). Patched the same way. `framerate_audit.py` carcam (the car's lag after a
      turn halving): 0.47 / 0.58 / 1.86 s at 1/30 / 60 / 144 before, 0.50 / 0.48 / 0.48 after; footcam (the camera
      following a player running sideways): 96 / 82 / 35 degrees a second before, 97 / 99 / 100 after.
      `framerate_audit.py` scenarios coast, runover, bikebody, bikecoast, bikelean; game-internals.md 4. Found with the
      rig's new `trace` (a vehicle's state every frame) and write watches attributing its speed changes to callers.
      Then the same `max(step, 0.5)` in vehicles and bikes in water and a boat's rudder, patched: at 144 fps a car kept
      1.6 m/s one second into the sea (0.6 at 30), a bike sank to the bottom; after, 0.76 m/s and the bike settles 2.95 m
      deep (2.75 at 30). `framerate_audit.py` carwater, bikewater. Left: the ped ones (CPed::ProcessControl).
- [ ] Fixes for what changes at the shorter step (physics, animations, script waits, particles, camera), each behind
      the switch; frame pacing. Left from the script survey (game-internals.md 11f): per-frame object / camera /
      text animations in missions (looks only).
## Milestone 5 - movies (2026-10-24) - done
- [x] STINGS.PSS (the Rockstar logos) and INTRO.PSS play natively at boot (`src/game/vcs_movie.cpp`): own PSS
      demuxer, MPEG-2 video through FFmpeg, shown by the presenter at 4:3 (pillarboxed in widescreen), 48 kHz PCM
      sound mixed into the audio output and used as the clock. Checked: logos 10.8 s, 321 pictures, none dropped;
      Start during the logos ignored (as on the PS2), the intro skipped by Start, the game then loads the save.
- [x] Display > Skip intro movies (`display.skip_intro`, `VCS_SKIP_MOVIES`; off = play them as on the PS2).
      Test tools (run.py, the rig, scenes) default to skipping them.
## Milestone 6 - retire the IOP (started 2026-10-24)
- [x] Step 1, streaming: CDSTREAM's stream server (RPC 0x65686577) replaced on the EE side by host-thread reads from
      the disc image (`src/game/vcs_stream.cpp`, `VCS_NATIVE_STREAM=0` for the IOP's). Loading as fast as before
      (boot 6.3-7.8 s, five LOAD_SCENE teleports 2.1 s - the game is the limit, not the reads: 14 MB in ~5 ms).
      Static reference scenes unchanged; world tour subset clean.
- [x] Step 2, sound (2026-10-24): CDSTREAM's two sound servers ported to the host (`src/game/vcs_sound.cpp`, the
      effects and music engines) on a native libsd (`src/sound/libsd.cpp`) and an SPU2 core of their own (the
      emulator's SPU2 class, now public), which is the audio output; `VCS_NATIVE_SOUND=0` for the IOP's. Tables the
      module carries (stream names, SPU2 addresses) are read from the disc's own CDSTREAM.IRX. Checked against the
      emulated IOP on identical input with `tools/soundcheck.py` (the IOP run's sound RPCs recorded with their IOP
      times and replayed into the native driver): **PHI_A1 + cutscene preload: 11 of 12 sounding 5 s windows
      bit-identical to the emulated IOP's output**, the 12th spans a jump in the reference's own timing; an AI drive
      with the radio: 99.5% of 19,314 SPU2 register writes / DMAs in the same second, none of the reference's
      missing, the rest of the audio differs by +-1-sample event timing (the emulated SPU2 runs up to one IOP batch
      behind the IOP clock). Found on the way: the radio started at volume 0 for 0.45 s after a station change (the
      preload must take its DMA time - game-internals.md 9a). The loop at a stream's end (no capture reaches one):
      a synthetic call sequence opening FLASH.VB 3 s before its end with looping on plays 13 s on without a gap.
      Fixed 2026-10-24 (the user heard it): bike engines restarting every frame, cars playing rev samples while
      slowing - the native "is voice v playing" (`0xA0A00`) read another voice's envelope. Same bike / car ride:
      156 key-ons on the IOP, 676 before the fix, 126 after; 0 / 91 / 0 voices re-triggered in back-to-back frames.
      The recordings now carry each call's reply and the replay compares them (43,138 calls: 21 differ, all a load or
      stop finishing sooner natively) - the check that would have caught it.
      Checked 2026-10-24 after the user missed handbrake screech and some door sounds: on scripted handbrake turns
      and exits / entries the game asks for the same sounds natively as on the emulated IOP (skid hiss 239, squeal 97,
      door open 217 / shut 216: 8 door sounds in both, at matching moments), the replay matches the IOP's replies, and
      through the strongest skid the native output equals the reference within ~1% loudness per quarter second.
      Same at the original 30 fps timing. Not reproduced as a native-driver difference; open until the user compares
      by ear with `VCS_NATIVE_SOUND=0`. (The scripted turns ran partly on sand, where GTA plays no screech.)
      2026-10-24, the user (comparing with the original): no tyre screech on a bike at speed with R1 held while
      turning, with native and emulated sound alike. On the airport runway the game does request the screech
      (sound 260, volume 25-36, pitch 16-18k, held through the skid; game-internals.md 9a) and the SPU2 plays it
      (voice output peak ~4000 against ~8300 for the engine; replay = live output). The user heard no screech in
      that clip: it was on voice 19, core 0 - see the next entry.
      **Fixed 2026-10-26: every sound on SPU2 core 0 was silent** (the user: machine-gun bursts where "only one bullet
      out of a few" sounds; also the missing screech and the doors that "sometimes" don't play). The SPU2 core fed
      core 0's output into core 1 through BVOL (the AutoDMA input's volume) instead of AVOL ("core external input",
      only core 1), and decoded both as sweepable volumes: libsd's sceSdInit sets core 1 AVOL 0x7FFF / BVOL 0, so core
      0 reached the output at volume 0 (0x7FFF as a fixed volume is -2). Both are plain signed 16-bit volumes (PCSX2:
      ExtVol / InpVol = SignExtend16). The game's 44 effect voices are core 0's 24 + 20 of core 1's, used in turn:
      ~70% of an M249 burst's shots (54 of 76) landed on core 0. Live after the fix: 76 of 76 shots sound; the bike's
      screech and its engine come through (a ride ~10x louder overall). Native and emulated sound share the SPU2
      core, so both had it - why the soundcheck comparisons agreed. `--iop-tests` checks a core 0 voice now.
      Mapping tools: `tools/irxdis.py --map / --decompile`
      (headless Ghidra), `tools/mipsdis.py` (MIPS I, standard library).
- [x] Step 3 (2026-10-24): no emulated IOP in the normal run - no IRX module loaded or run, no IOP time emulated
      (`retireIop`, game-internals.md 9b); `PS2X_IOP=1` runs it again (`VCS_NATIVE_SOUND=0` / `VCS_NATIVE_STREAM=0`
      do too). Boot to gameplay 4.9-6.1 s in all three configurations (game-bound); IOP 0.2 ms per frame (was
      0.3-0.4). Memory card save / Load Game / Delete Game through the menus pass (`tools/saves.py`, whose load check
      now looks at where the player ends up: the runtime's `[MC]` log lines arrive late). Tour subset (`--every 9
      --limit 18`): 15 ok, 2 relocated, 1 falling - identical with the IOP off, on, and everything emulated. Six story
      missions (JER_A3, PHI_A1, MAR_A1, LOU_A1, LAN_B1, BRY_B1) run without problems with the IOP off.
- [x] **Audio audit (2026-10-26)**: every sound path is native in the normal run. All 95 missions of the smoke test log
      the native sound driver and the emulated IOP off, with no sound / stream / libsd warning (libsd's one unported
      case, `VoiceTrans` modes other than 0, never occurs). Movie sound goes through FFmpeg into the same output.
      `tools/soundcheck.py` again against the emulated IOP: MAR_A1's opening cutscene 99.89% of 23,875 SPU2 writes
      in the same second, loudness envelopes correlating 0.999, 9 of 10 sounding windows within -40 dB (most of them
      identical); an AI drive with radio station changes 99.42% of 34,658 writes, envelopes 0.949, each 5 s window
      as loud as the reference within ~2 dB (sample-level differences are the known +-1-sample event timing). A
      live native recording of the same drive changes stations like the IOP: stops completed in 37-427 ms (IOP
      1-385 ms), every stream open accepted. Fixed in the check itself: a replayed stream open could land while a
      native stop was still finishing (the IOP's had taken 1 ms), be refused and leave the music engine out of step
      for the rest of the replay (4,580 replies "differing"); live the game polls the state until 0 first, so the
      replay now lets native time run as those polls would (one open waited 308 ms). The roadmap's "Where we are"
      table, still showing IRX modules and SPU2 emulation, now shows the current state.
- [x] **Long-run spikes** (2026-10-26, PERFORMANCE.md 14): an unused upstream VAG decode on sound-file reads removed,
      zlib's inflate_fast native (verified: 4,096 calls the same), the 30-second model-id search native, texture decodes
      straight from same-layout uploads (3,500 the same as VRAM). 24-minute soak: p99 13.7 -> 11.9 ms, frames over 17 ms
      357 -> 203, game-caused frames over 25 ms away from teleports 39 -> 15; what remains are loads.
- [ ] Open (found 2026-10-24, not milestone 6): tour point (-925, 1175) falls through the world when the tour reaches
      it after the points before it, in every configuration; a fresh teleport there from Fort Baxter lands (z 14.5).
      Collision streaming order / timing at that spot needs a look.
## Phase 5 - stability
- [x] Saves lost when saving (players' reports after 0.1.0: "crashed and destroyed my savegame", "deletes the save
      file when I save at Phil's safehouse") - **fixed 2026-11-01** in the card layer (game-internals.md 10f). The slot
      file is named after the last mission; "Kill Phil: Part 2" made NTFS put the save in a stream of an empty
      `1 Kill Phil` after the game had deleted the old slot, so the save was never found again. Names are now stored as
      `%XX` where the host can't take them, such lost saves come back at the next start, and both cards are zipped into
      `<data>/backups/auto-*.zip` before the game deletes or overwrites anything (it deletes the whole save folder after
      a failed write). `tools/saves.py` checks all three: recover, save, load, delete, backup - 5/5 ok. The crash itself
      was not reproduced: neither case crashed, and the only crash log from a player (#34) is a lost D3D11 device.
- [x] The game stopping at start in a non-ASCII folder, or with a Windows user name in Cyrillic (issue #27, "No mapping
      for the Unicode character...") - **fixed 2026-11-01**: vcs.exe's manifest sets the UTF-8 code page (DECISIONS.md).
      Checked hidden: the game in `...\Игры é\game` and a data folder `...\Пользователь é\VCSRecomp` both crashed before
      and now boot to the intro movie (321 pictures, 0 dropped), the cache and card made under the Cyrillic name.
- [x] Shakedown (MAR_A1): characters floating over / sunk into the ground in the opening cutscene, and the game stuck on
      "Loading..." when entering the shop (user report 2026-10-25) - **fixed 2026-10-25**, two separate bugs
      (game-internals.md 4 and 12). The cutscene: CCutsceneObject::ProcessControl's dead `step < 0.5` branch (only
      above 100 fps; patched like the ped-movement one). The shop, at every frame rate: the recompiler's compare-with-zero
      branches tested only the low 32 bits, so CPed::ProcessControl read bit 31 of the ped flags as bFadeOut and deleted
      the shopkeeper; his audio line was then dropped and the mission waited for it forever (PS2RECOMP_PATCHES.md).
      Check: `python tools/shakedown.py` (cutscene skipped, truck to the shop, the two gang members beaten, Vic through
      the door, shopkeeper alive and the loading screen gone; also a `regress.py` step). With the branch fix the gang
      members who start a fight outside the shop stay alive (the 32-bit test had faded them out too), so the mission now
      plays as written. Regression suite afterwards: missions, drive, evening, saves, tour, shakedown all ok; all 8
      scenes match the capture from before the change (`loading` / `pause_map` differ from the 2026-10-23 baseline
      before it too).
- [x] Vic falls through the world in some places - **no longer happens** at the spot the user saw it (user
      playtest 2026-10-24); fixed along the way by other work (which change is not known: the VU0 collision
      natives, the scheduler / timing work and the renderer changes all came in between). The original report:
      (reported by the user 2026-10-22, after the
      native renderer; not yet known whether it is new). Collision needs a look: whether the
      collision sectors / models are streamed in at those spots (CDSTREAM reads, IMG offsets),
      VU0 programs used by collision (`0x0000` in .vutext looked like line/sphere vs triangle
      tests, docs/game-internals.md "VU microcode"), and float edge cases in the recompiled code
      (FP contraction, round-to-zero, denormals). Compare with PS2X_HLE=0 to rule the renderer out.
- [x] Settings menu (F2) flickered between two layouts while the mouse sat between rows, and a click on a toggle
      (Skip intro movies) did not register - **fixed 2026-10-24**: the menu was an auto-sized, centred window whose
      help line grew and shrank with the hovered row, so the rows moved under a still mouse every other frame. Now a
      fixed-size window with a fixed-height help area (SETTINGS_MENU_WIDESCREEN.md "Menu v2"). Checked with real
      mouse / keyboard input (a script moving the cursor over the window, rows' boundary, clicks, arrows, Q / E,
      Escape). Settings locked by an environment variable (tools/run.py sets `VCS_SKIP_MOVIES=1`) now name it beside
      the label: that alone makes a click do nothing.
- [x] **Release check from a clean clone** (2026-10-28/29): the README's install (`git clone`, `build.py --recompile`)
      failed in a fresh folder - `unknown target 'ps2EntryRunner'`: recompile.py configures before generated code exists
      and build.py never configured again (dev trees were configured after `generated/ee` existed). Fixed in build.py.
      The first configure took 36 min, almost all of it toml11's test / docs submodules and full histories of three
      pinned dependencies: shallow, no submodules - a fresh configure 117 s (PS2RECOMP_PATCHES.md). The ThinLTO exe
      from the clone: full regression suite ok (scenes but the stale baseline's `loading` / `pause_map`, tour as the
      baseline - (-925, 1175) falling, the known spot -, 5 missions, drive, evening, saves, shakedown, widecull,
      menushape, controls 25/25); trainer 33/33 with all 110 vehicles. Two test fixes: the trainer's boost check ran
      into a hedge or a parked car in the Leaf Links garden (0.2 / 6.6 / 26.6 in three runs; now on open ground, 36.2
      three times), and the meters step's 30 fps reference varies 28.2-30.6 between runs (tolerance 5% -> 10%; the bug
      it guards against was 4.8x). Started on its own (the README's "Play"), vcs.exe ran the software GS at ~5 fps:
      only run.py and the test tools set `PS2X_GS=hw`, the runtime's default is cpu. The launcher now defaults to hw
      (`PS2X_GS=cpu` still picks the software path); a start without run.py's environment: D3D11, movies 0 dropped.
      The tools no longer set it either (they run the exe's own defaults), and the exe is now `build/vcs.exe` with
      its DLLs and vcs.ini (DECISIONS.md 2026-10-29). Fort Baxter gameplay with no PS2X_* / VCS_* set: gs_draws 0,
      vu1_programs 0, native sound, emulated IOP off, render worker on, 60 fps at the default cap.
- [x] Last Stand (DIA_C5, the story's last mission): after the roof and the final cutscene, "Loading..." over black for
      good (user report 2026-10-29) - **fixed 2026-10-29** in the trainer. Not a hang: the game ran on behind a screen
      left faded out for the credits, which the story starts only when every chapter is done; the mission had been
      started from the trainer's list (the user's newest save is LOU_A3), so nothing faded it in. "Loading..." is the
      game's own text over the fade (F12 draw log), the same on a PS2. The trainer now rolls the game's credits after a
      Last Stand it launched (game-internals.md 11g, TRAINER.md); story flags untouched. A first version CALLed the
      credits from trainer bytecode: the CALL's packed return IP cannot point outside ScriptSpace, the RETURN went to
      garbage and the game loop stopped - now a script of its own at the subroutine (`vcs::rig::startScriptAt`). Check:
      `python tools/finale_check.py --full` (9 checks, 0 failed: credits, back below the Mendez building, faded in) and
      `--story` (the story's own credits, one copy only).
- [x] White lines across the picture in the morning, from the planes' trails in the sky (user report 2026-10-30, seen
      after the Linux work but not caused by it: at 4:3 too, in every renderer path, with either frame timing) -
      **fixed 2026-10-30** in the recompiler. Its `VCLIPw` had the + / - clip bits swapped and compared with w, not
      |w|; RenderWare's Im3D line clipper (EE, `FUN_002F95E0`) then cut a trail segment leaving the view at the
      opposite side plane, next to the camera, and the wrapped projection drew across the screen (game-internals.md
      6k). One generated function changed. Check: `VCS_SELFTEST=1` (12 clip-flag cases); at the Little Havana Empire
      site at 08:20 the F12 dumps had a near-camera line vertex (Q ~8.9) every frame at 4:3 and 16:9, none after.

## Phase 6 - enhancements

- **Player settings** (2026-10-28, asked for by the user; roadmap milestone 8): every control rebindable - keyboard and
  mouse buttons / wheel with two slots per PS2 button and stick direction, a walk key, the gamepad's button map, stick
  deadzones, camera speed, trigger point, swapped sticks, inverted camera, a mouse camera (mouse speed as the right
  stick) with sensitivity / invert; the game's own vibration on the gamepad (it was dropped by the pad stub; 0-200%);
  master / movie volume and mono / swapped output; vertical sync (off / on / tearing for VRR); a frame-rate counter;
  mute or pause in the background; presets (Authentic, Enhanced, Widescreen light) and Reset all settings
  (docs/CONTROLS.md, SETTINGS_MENU_WIDESCREEN.md "Player settings"). `tools/controls_check.py`: 21 checks, 0 failed;
  regress step "controls". Defaults unchanged (the old fixed mapping, mouse camera off, vibration as on the PS2).
- **Mouse look and driving keys** (2026-10-29, asked for by the user): on foot the mouse turns the follow camera itself
  (heading written in the game with the camera's source points, pitch through the stick's pitch speed with the game's
  stick curve undone; exact amounts, no top speed for the heading), in other cameras it is the right stick past the
  game's dead third; Mouse camera on by default, forward looks up. While driving W / S / Space are Cross / Square / R1
  (accelerate, brake, handbrake) and do nothing else; lean on Keypad 8 / 5 (Keyboard > Driving, a key group of its own).
  Right click (R1) locks on, checked. game-internals.md 12a / 12b (the stick curve, three pad reads a frame, the
  follow camera FUN_0029dab0), DECISIONS.md. `tools/controls_check.py`: 36 checks, 0 failed (11 new: mouse heading /
  pitch / invert / off, the pause menu, lock-on, driving keys, lean rebind, W drives and Space stops the car, the mouse
  on the vehicle camera, mouse look after leaving the car).
- **Mouse pitch as direct as the heading** (2026-10-31, reported by the user: up / down far more sluggish than left /
  right): on foot the mouse's pitch went through the stick, capped at its top speed (1.5 rad/s, the rest carried over,
  past 0.6 rad dropped) and then eased by the game (20% of the gap a 1/50 s step, 10% indoors). Now it is written in
  the game as the heading: alpha and the source points turned up / down about the target point, in the game's pitch
  limits (src/game/vcs_input.cpp; why: game-internals.md 12b). The gamepad keeps the game's own speeds (pitch 0.6 x the
  heading's). `tools/controls_check.py`: "mouse pitch at once" (120 counts -> +0.300 two frames later, still +0.300
  after 22); 47 checks, 0 failed.
- **Free aim with the mouse and the gamepad** (2026-10-31, asked for by the user): Keyboard > Free aim / Gamepad > Free
  aim (off by default): with a gun, holding R1's button aims where the mouse / right stick points with the game's own
  circle, shots go through it, Vic moves as he does locked on with that gun; fists / melee / thrown / scoped weapons keep
  the lock-on. It is the game's own free aim (a lock-on to a dummy object), entered in one pad report, the dummy turned by
  the host, the world run with the free-aim bit cleared and the camera / HUD with it set, the player's shot line taken
  from the camera (src/game/vcs_aim.cpp; game-internals.md 12c, DECISIONS.md). Also: the scopes and first-person weapon
  views turn 1:1 with the mouse; Keyboard > Mouse wheel (on): next / previous weapon on foot, zoom in the sniper scope,
  camera, binoculars; the camera keeps the aim's heading when the aim ends. Test commands `gamepad ...` (an injected
  gamepad) and `aimstate`. `tools/controls_check.py`: the free-aim checks (right button enters it, the mouse turns it
  1.000 rad per 400 counts, a man on the aim line hit and one 2 m beside it not, 3.6 m strafed while aiming with the aim
  unchanged, the camera kept on release, fists / free aim off still lock on, the gamepad's RB and right stick, the sniper
  scope 1:1 and zoomed 70 -> 45 by two notches, the wheel's weapon steps); 46 checks, 0 failed. SMG, assault rifle and
  M249 shots checked down the camera line too. Not done: aim assist on the gamepad; the shotguns fire through another
  path (not FireInstantHit's line test), so their pellets still fly from the gun to the aim point - right at range, a
  little off up close; the flame-thrower not checked.
- **Free aim in a car** (2026-11-01, asked for by the user): Jive Drive's passenger-seat shooting (the game's own free
  aim in its drive-by mode, SET_PLAYER_DRIVEBY_MODE - the only mission that uses it) is turned by the mouse / right
  stick when free aim is on, the left stick centred; the game's range, height limits and car-following kept, shots down
  the camera line (src/game/vcs_aim.cpp; game-internals.md 12c). `tools/controls_check.py`, in a car standing with an
  AI driver and the mission's two commands: free aim off - the left stick turns it (-1.8 rad a second), the mouse not;
  on - the mouse 1.000 rad per 400 counts (the camera -1.008), the right stick -1.05 in 0.5 s, the left stick 0, 21 deg
  at the top, a man under the circle hit and one 2 m beside it not, leaving the car ends it. In the mission itself
  (hidden run, the user's save, god mode): the aim kept its world heading for 4 s of Lance's driving (0.02 rad), 62
  shots in 3 s of fire, all down the camera line, tracers to the circle. Then (the user's ask) the gamepad's R1 fires
  in that car aim only (sent as Circle): controls_check "the gamepad's R1 fires" (Circle, 30 shots in 1 s) and, free
  aim off, "the gamepad's R1 is R1".
- **Free aim in Phil's shooting range** (2026-11-01, the user's report: the old aim, and broken): the range forces
  the game's free aim on foot (SET_PLAYER_FORCE_FREE_AIMING); the right button's R1 + L3 and its release ended it for
  the round. Now taken over like the car's (game-internals.md 12c). In the range itself (hidden run, entered through
  Phil's door as a player does): the mouse 0.75 rad per 300 counts with the camera, the right button pressed and let
  go keeps it, the left stick nothing; aimed at the pop-up targets with the mouse, 400 points (the targets' own
  HAS_OBJECT_BEEN_DAMAGED). controls_check `forced_free_aim` (the command on foot with a pistol): free aim off the left
  stick turns it and the mouse not; on - the mouse 1.000 rad per 400 counts, the right button kept, the right stick
  aims and the gamepad's R1 fires (Circle; the user's choice, as in the car), 107.6 deg to the side at most (110 less the line test's pull), 40.0 up, a man under the
  circle hit and one beside him not, the script's end ends it. (Starting the range from the trainer's mission list
  drops Vic through the floor: the range is an interior the door's script loads - the game's, with or without free
  aim.)
- **Music / sound effects volume** (2026-10-28, asked for by the user): Audio > Music and Sound effects are the game's
  own levels, in step with its pause menu both ways and kept across save loads (game-internals.md 9c). controls_check
  checks that the levels reach the game and its prefs, a save load keeps them, and the pause menu's slider changes the
  setting; saves.py ok. A Voices volume (speech scaled in the native driver) was removed again the same day.
- [x] **Load Game hang** (found 2026-10-28 by the controls check, fixed the same day): after the check's binding /
  preset / menu / vibration steps in the city, Load Game from the pause menu sat on "Loading..." for ~45 s and then the
  game thread returned to 0x4548435F from the script VM (ra 0x256164). The trainer's god mode (on for the vibration
  step) kept the address of its last tick script's slot; the load fills that slot with one of the save's scripts, and
  the trainer, seeing it active while the clock stood still, took it out of the active list mid-load. A slot is now
  ours only while its IP is in the slot's own code (`vcs::rig::scriptActive`, game-internals.md 11d). The controls
  check's flow with the load at its old place: stuck on "Loading..." 2 of 2 before, fine after (the trainer's slot
  was taken by the load: logged). `tools/saves.py` (regress `saves`) now loads with god mode on: before the fix its
  load failed (stuck on "Loading...", the player never back at the save), after it passes with the slot taken logged.

- **Full screen** (2026-10-27, asked for by the user): Display > Window mode (Windowed / Full screen, borderless at
  the desktop's resolution) and Alt+Enter; the cursor hides after 2 s still in full screen; hidden test runs no longer
  write vcs.ini (SETTINGS_MENU_WIDESCREEN.md "Full screen").

- **Ultrawide** (2026-10-27, asked for by the user): Aspect ratio 21:9, 32:9 and Match window (the window's own
  shape, 4:3 .. 32:9, followed live). Checked with the reference scenes and city drives at 21:9 / 32:9 (picture, HUD,
  cutscene bars, culling and streaming at the edges); regress step "widecull" covers 21:9 and 32:9. At 32:9 3840x1080
  the render worker costs about twice 16:9 1920x1080 (busiest bench spot 97 fps, drive 103) - details in
  SETTINGS_MENU_WIDESCREEN.md "Ultrawide".
  Same day: the pause menu and loading screens sit inside the game's frame at 4:3 (between the bars of a 32:9 game on
  a 16:9 monitor), the pause menu switches to 4:3 at once, and no frame is squeezed for one shape and shown in the
  other (SETTINGS_MENU_WIDESCREEN.md). Then the shifted frame at the switch (VCS shows the previous frame, so the shape
  now follows the displayed picture; checked with every presented frame captured).
  Also: the integer Internal resolutions render wider in widescreen (1x at 32:9 is 1707x448; `PS2X_HW_WIDE_SCALE=0`
  = before), and the radar's streak of map beside its circle at 32:9 is gone (the HUD layer squeezed before rounding to
  PS2 pixels).

- **Widescreen culling at the screen edges** (2026-10-27, reported by the user): at 16:9 / 16:10 buildings, peds and
  cars in the outer part of the picture were culled as if the screen were 4:3 - `CCamera::IsSphereVisible` tests
  side planes built from the FOV alone, not from the widened view window (game-internals.md 5). The planes are now
  widened to the Hor+ edge before each test (`src/game/vcs_display.cpp`, `VCS_WIDE_CULL=0` = the game's planes).
  Sector culling was already right. Checked: plane edge = view window x at 16:9 (0.9336) and 16:10 (0.8402),
  regress step "widecull" (fails with `VCS_WIDE_CULL=0`: 0.7002); spheres passing the test at 16:9 +8-25% over the
  three bench spots; 4:3 untouched (the wrapper returns before reading memory).

- **Trainer** (2026-10-26, docs/TRAINER.md): an RDR2 Native Trainer style menu (F5 / Back + RB) - god mode, never
  wanted, wanted level, money, every weapon and loadouts, unlimited ammo, no reload, spawning all 110 vehicles,
  vehicle invincibility / repair / flip / boost / paint, teleports to 13 districts and a saved position, time,
  freeze time, weather, game speed. `tools/trainer_check.py`: 30 checks, 0 failed (with all 110 vehicles).
  Mission launcher added the same day: 59 story missions by title, five side jobs (in their vehicle); 33 checks, 0
  failed.
- **Free camera and Hide HUD** in the trainer (2026-10-30, docs/TRAINER.md "Camera", F6 / Back + LB): the active
  CCam written after the game's update of it (game-internals.md 12b), real-time speed (20 m/s: 50.7 m in 2.5 s at 30 fps,
  40.3 m in 2 s at 60 fps), the world streamed in 640 m from the player, works driving and in cutscenes, "Teleport
  player here"; Hide HUD skips CHud::Draw. `VCS_FREECAM=0`: the game's camera untouched (checked). `trainer_check.py`
  40 checks (the one failure the check's own flight path, fixed); `--freecam`: 7 checks, 0 failed.
  Same day: a mission launched under it hung on "Loading..." (the first version replaced the camera's update, which
  drives a cutscene's end); fixed, and a launch turns it off. `--freecam` 8 checks (Soldier's cutscene played out
  under it), 0 failed; the full check 41, 0 failed.

## Milestone 9 - Linux (started 2026-10-30)

Native Linux as a second platform (the user's request; the Steam Deck postponed): docs/LINUX.md has the build, the
differences and the work list. Written and syntax-checked on the Linux laptop (GCC 13 for checking; the build itself
needs the packages `build.py --check-deps` lists, clang 20 among them):
- toolchain: clang 19+ found by `tools/toolchain.py`, clang-cl's flags for clang (`ReleaseMode.cmake`, incl.
  `-fno-strict-aliasing`), compile / ThinLTO jobs capped by memory, `--check-deps`;
- host / runtime: exe folder, XDG data folder, the LP64 `long` spots (`fseek`, IOP `strtol`), `_BitScanForward`,
  control socket SIGPIPE / reuse, card birth time, rig kill and copy;
- renderer: RHI backend choice (`VCS_RHI`, Graphics > Graphics API), a null backend, the **Vulkan backend**
  (`src/rhi/vulkan/rhi_vulkan.cpp`), HLSL -> SPIR-V through glslang (`tools/hlsl_spirv.py`; all 15 shaders pass
  `spirv-val`), Dear ImGui through the RHI (`render/ui_renderer.cpp`);
- debugging: crash handler on signals (tested), SIGPROF profiler.
Not run in the game yet: first build, then the checks in docs/LINUX.md (generated code equal to Windows', bit-exact
HLE / VU0, scenes against the Windows reference, the rig).

## Milestone 9 - distribution: the launcher and releases (2026-10-30)

The user's request: a launcher in releases that finds the game, builds and launches it (Windows first), with the
Python scripts still usable on their own. docs/LAUNCHER.md has the whole of it; DECISIONS.md 2026-10-30 the choices.
- [x] Tools: `@vcs {json}` events with failure codes (toolchain), `build.py --recompile-if-needed / --progress json /
      --launcher`, `generated/recompile_stamp.json` + `recompile.py --check`, `build/build_info.json`, `doctor.py`,
      `vcs_disc.py check`, `setup_vs.py`, `backup.py`, `report.py`, `rip_disc.py`, `deps.py`, `release.py`,
      `update.py`; `find_iso` reads `build/vcs.ini` too (vcs.exe's); vcs.exe's exit codes per failure
      (`src/disc/vcs_disc_id.h`, shared with the launcher).
- [x] `launcher/`: VCSLauncher.exe (SDL3 + Dear ImGui, static, 4 MB): the four steps, the next-step button, tasks with
      stages / progress / cancel / errors and fixes, play with the log, saves backup / restore, shortcut, report,
      updates, options; command line `--check / --build / --play / --self-test / --screenshot`.
- [x] Releases: `tools/release.py` (82 MB zip: source, launcher, embeddable Python, `deps/`, licences; the game-file
      guard), offline configure from `deps/` (`FETCHCONTENT_FULLY_DISCONNECTED`, the FFmpeg URL - a `[gta-recomp]`
      CMake change), CI workflow `.github/workflows/launcher.yml` (written, not run yet).
- [x] Checked: from the release zip in an empty folder, the hidden launcher built the game from scratch (bundled Python,
      no downloads) in 8 min 42 s, and `--play` started it (the intro movie, 0 dropped); update 0.1.0 -> 0.1.1 in place; backups round-trip on a card copy;
      setup_vs.py dry runs and the Microsoft-signature check; a disc copy from an image file.
- [ ] A PC with nothing installed (Windows Sandbox): setup_vs.py's real install. A real DVD drive. CI's first run and
      the first tagged release. Code signing.
- [x] **The launcher draws itself** (2026-11-01, the user's request; DECISIONS.md, docs/LAUNCHER.md): SDL3 + Dear ImGui
      replaced by its own Win32 window with custom chrome, a D3D11 renderer (one instanced draw of SDF primitives),
      DirectWrite text and immediate-mode widgets with springs, after uppies; a new flow - Setup (a rail and one step at
      a time), Home (Play over a striped sunset), Task (rolling percentage, timeline, log drawer), sheets, dialogs,
      toasts. 1.0 MB (was 4), 17 s from scratch (was 81), 0.67 ms a frame, nothing drawn when idle. Checked offscreen
      (every screen and state, at 1x / 1.5x, transitions frame by frame), with the real disc, and a real build through
      it (LAUNCHER.md "Checked"). Not checked: the visible window by hand, Windows 11.
- [x] **Older CPUs** (a player's report on 0.0.2, 2026-10-30: Xeon E5-2687W, Sandy Bridge - "recompiling fails in 1
      second", `recomp.log` empty): every build was `/arch:AVX2`, so `ps2_recomp.exe` died on its first AVX2 / BMI
      instruction (0xC000001D) before printing. **Fixed 2026-10-31**: the build follows the building PC's CPU,
      `PS2X_CPU=AVX2|AVX|SSE4` (DECISIONS.md 2026-10-31; `VCS_CPU` forces one); doctor / the launcher show the processor
      and refuse one without SSE4.2 (`cpu_unsupported`); a tool that fails ends its log with how it ended
      (`toolchain.describe_exit`, launcher codes `illegal_instruction`, `missing_dll`). `vu1v` moved to `__AVX__` (its
      per-lane SSE4 version had never compiled). Checked on the i7-7700K with `VCS_CPU`-forced builds (quick, no LTO):
      - instructions: the AVX vcs.exe / ps2_recomp.exe have no AVX2, FMA, BMI, F16C or MOVBE instruction, the SSE4 ones
        no AVX either (objdump; the 375 `tzcnt` in every build, AVX2 too, are LLVM's `rep bsf` on inputs known to be
        non-zero, which pre-BMI CPUs run as `bsf`);
      - the AVX and SSE4 recompilers write the same 10,386 files as the AVX2 one, byte for byte (= generated/ee);
      - `VCS_SELFTEST=1`: 0 failures at each level;
      - scenes: the static ones (loading, pause map) identical at all three levels; AVX and SSE4 identical in the
        flyover and the cutscene too; the moving scenes differ between levels no more than two runs of the same AVX2
        exe do (timing);
      - game thread: AVX2 4.7-4.9 ms, AVX 4.6-4.9 ms (no measurable cost), SSE4 5.5-6.1 ms (~20%: the VU helpers per
        lane), all at 30 fps;
      - a tree configured for another CPU is configured again once (`[build] configuring ... PS2X_CPU=AVX`), not again
        on the next build; a simulated 0xC000001D through `run_logged` gives the named exit code in the log and the
        `illegal_instruction` event.
      Not run on a real pre-Haswell CPU (none here): the reporter's PC is the check.
- [x] **The launcher on Linux** (2026-11-01, the user's request; DECISIONS.md, docs/LAUNCHER.md "Linux"): native X11
      and Wayland, Vulkan (opened at run time), FreeType + fontconfig, the portal's file dialogs, `pkexec` package
      installs (`build.py --install-deps`), our own icons on both OSes. Checked on the dev laptop (Mint 22, XFCE, HD 520):
      self-test, every screen offscreen with Vulkan validation clean; the window on X11 (move, resize, maximise,
      minimise, close through xfwm4) and on Wayland (nested weston: the same, plus un-maximise back to the floating size);
      idle: 7 frames in 20 s (the entrance), then none - 5 ms of CPU. Not checked: GNOME / KDE Wayland by hand, a drop from a file manager, the
      portal dialog with a real click, `--install-deps` for real, the Windows build (CI compiles it).

- [x] **Linux releases: an AppImage, with Windows** (2026-11-03, the user's choices; DECISIONS.md, LAUNCHER.md "The
      Linux release"): `release.py --platform linux` (glibc 2.35 floor, CI on Ubuntu 22.04, pinned appimagetool /
      runtime / wayland-protocols), the install folder filled from the image (`update.py install`, changed files only),
      AppImage updates, `deps.py --platform linux`, apt.llvm.org for old Debian / Ubuntu (`linux_packages.py`), one
      `publish` job for both platforms. Checked end to end on the laptop: AppImage -> files -> game built in 18 min ->
      played; an update's rebuild 1.7 s. Not checked: CI's 22.04 build, an older distribution, apt.llvm.org for real.
## Useful switches
| Env var | Effect |
|---|---|
| `PS2X_GS=cpu\|hw\|gpu\|null` | GS renderer (vcs.exe: default hw, set by src/main.cpp; the runtime's own default cpu) |
| `PS2X_HW_SCALE=n`, `PS2X_HW_DEBUG=1` | hardware renderer scale / stats |
| `VCS_CPU=AVX2\|AVX\|SSE4` | build tools: the CPU the build is for (default: this PC's, `toolchain.cpu_level`); an older one's build runs here (tests) |
| `VCS_FPS_LOG=1` | VBlanks and flips per second (was `VCS_FPS=1`; `VCS_FPS` is the frame rate cap) |
| `VCS_TIMING=0\|1`, `VCS_FPS=30\|60\|120\|144\|-1\|0` | game timing original / frame-rate independent; frame rate cap (-1 display refresh, 0 unlimited) |
| `VCS_TIMESTEP=N` | independent timing: exactly 1/N s per game frame, unpaced (frame-dependence tests) |
| `VCS_SKIP_MOVIES=0\|1` | play / skip the intro movies (Display > Skip intro movies); test tools default to 1 |
| `PS2X_MC_BACKUP=0` | no automatic card backup (`<data>/backups/auto-*.zip`) before the game deletes or overwrites a card file |
| `VCS_MOVIE_LOG=1` | movie playback: sound pushed / queued / played vs the pictures, every second |
| `VCS_NATIVE_STREAM=0`, `VCS_STREAM_LOG=1` | disc streaming through the IOP (the reference); native read stats every 5 s |
| `VCS_NATIVE_SOUND=0` | sound through the emulated IOP's CDSTREAM / LIBSD (the reference) |
| `VCS_SKID_LOG=1` | the skid-sound function's inputs for the player's vehicle (type, drive type, speed, wheel states, surface) |
| `PS2X_SPU2_VOICE_LOG=1` | SPU2 key-ons (address, pitch, ADSR, volume) and every sounding voice's address / envelope / output peak each 0.1 s |
| `PS2X_IOP=0\|1` | emulated IOP off / on (default: off when streaming and sound are native) |
| `PS2X_SPU2_TRACE=<file>` | every SPU2 register write and DMA to `<file>.<n>` per SPU2 instance (0 = the emulated IOP's) |
| `PS2X_RPC_RECORD=<file>`, `PS2X_RPC_RECORD_SIDS=<hex,...>` | record RPC calls with their IOP time (tools/soundcheck.py) |
| `VCS_SOUND_REPLAY=<record>`, `VCS_SOUND_REPLAY_OUT=<wav>` | replay recorded sound calls into the native driver, write its output, exit |
| `VCS_MIN_STEP=x` | independent timing's step floor (default 50/240: the 240 fps ceiling; lower makes an unlimited cap unlimited) |
| `VCS_TIMING_LOG=1` | independent timing: fps, host wait, EE clock vs real time, WaitVBlank callers every 2 s |
| `VCS_GAME_VOLUME=0` | Audio > Music / Sound effects leave the game's own levels alone (no setter hooks, no sync; the reference) |
| `VCS_FOG_DISTANCE=10..100`, `VCS_FOG_START=0..300` | Graphics > Fog for one run: the game's fog range in percent (100 = original; game/vcs_fog.cpp) |
| `VCS_OBJECT_DISTANCE=100..400` | Graphics > Draw distance > Object distance for one run: CRenderer::ms_lodDistScale (0x4887F8) in percent, the entities' LOD / stream distances (100 = original; game/vcs_distance.cpp, game-internals.md 9h); `distancestate` reports the scale and the camera's LOD / generation multipliers |
| `VCS_FOG_HOOK=0`, `VCS_TRACE_FOG=1` | the fog-range setter left alone (the reference); the game's (start, end) pairs every 5 s |
| `VCS_FREEAIM=0` | free aim's hooks not installed (CGame::Process, CCamera::Process, the weapons' line test): the game's aiming untouched, the setting does nothing |
| `VCS_INPUT_MAP=0` | the runtime's fixed built-in key / gamepad mapping instead of the player's bindings (`ps2_pad.cpp`; the reference) |
| `VCS_VSYNC=0\|1\|2` | Vertical sync for one run: off, on, off with tearing allowed |
| `VCS_SCRIPT_TIMESTEP=0` | mission meters that count per frame stay on the frame rate (game-internals.md 11f; the reference) |
| `VCS_PHYSICS_TIMESTEP=0` | the push of a body under a wheel and the rolling resistance stay per frame (game-internals.md 4; the reference) |
| `VCS_COLL_LOG=1` | each ApplyCollisionAlt (FUN_0034D670) of the player's vehicle: the other entity, contact point, normal, speeds |
| `VCS_SCRIPT_TIMESTEP_LOG=1`, `VCS_SCRIPT_TIMESTEP_CHECK=1` | per scaled instruction every 2 s game time: runs, the game's amount vs applied; log each listed instruction found in a newly loaded mission |
| `VCS_PERF_OVERLAY=0\|1` | the perf overlay setting for one run |
| `VCS_PROFILE=s` | sampling profiler (guest functions + host symbols) |
| `VCS_PROFILE_LINES=name` | with `VCS_PROFILE`: source lines of the host functions whose name contains `name` |
| `VCS_PROFILE_RAW=<file>` | with `VCS_PROFILE` (Linux): the samples dladdr cannot name, appended as `module offset count` lines per report - for `llvm-symbolizer --obj=<module>` (anonymous namespaces, static libraries: gs_hw, the RHI; PERFORMANCE.md 27) |
| `PS2X_VIF_UNPACK_FAST=0\|verify` | VIF1 UNPACK general loop only / both, compared |
| `PS2X_GS_RECT_VERIFY=1` | every 64th rect upload also written pixel by pixel and compared |
| `PS2X_HW_LAZY_INDEX=0` | lazy uploads to write found by rescanning the queue (the old way) |
| `VCS_SKIN_VERIFY=1` | native ped skinning (SSE) checked against the scalar reference, a line every 16k packets |
| `VCS_EE_CHARGE_SHIFT=<n>` | frame-rate independent timing charges guest work 1/2^n of its estimated EE cycles (default 2; 0 = as original timing) |
| `VCS_CSR_FENCE=1` | the flip's GS CSR read (0x370714) waits for the render worker as other CSR reads do |
| `PS2X_RECOMP_FULL_PC=1`, `PS2X_RECOMP_NO_RESTRICT=1` | when running the recompiler: a `ctx->pc` store before every load / store and branch as before; generated functions without `__restrict ctx` |
| `PS2X_GS_THREAD=0\|1` | the render worker's GS stage (GS front end + frame building on a second thread; default on with the worker and the HW renderer) |
| `PS2X_RENDER_THREAD=0\|1` | render submission on the game thread / on its own thread (default with `PS2X_GS=hw`) |
| `PS2X_RENDER_THREAD_LOG=1`, `PS2X_RENDER_THREAD_AHEAD=n` | worker items / fences every 5 s; frames the game may run ahead (1) |
| `VCS_PROFILE_THREAD=RenderWorker` | `VCS_PROFILE` samples the render worker instead of the game thread |
| `PS2X_FAST_DISPATCH=0` | guest calls / loop checkpoints always through the scheduler (no inline fast path) |
| `VCS_EXE=path` | tools/run.py (and the rig, bench, scenes on top of it) run that build |
| `VCS_WATCHDOG=s` | EE + IOP thread states (pc, ra and sp of the running thread: a hang's frame can be read) |
| `VCS_PROFILE_INCLUSIVE=1` | with `VCS_PROFILE`: every guest function on the sampled stack counts once per sample (a subsystem's whole cost) |
| `VCS_VIEW_DISTANCE`, `VCS_DETAIL_DISTANCE`, `VCS_POPULATION_DISTANCE`, `VCS_VEHICLE_DENSITY`, `VCS_PEDESTRIAN_DENSITY` | Graphics > Distance and population for one run (0..100) |
| `VCS_POOLS=peds,vehicles` | the ped / vehicle pool sizes for a test (default 280 / 200 with Streaming memory above PS2, else 70 / 40) |
| `VCS_CROWD_LOD=0` | no crowd LOD (far random peds processed every frame; A / B measures) |
| `VCS_DISTANT_TRAFFIC=0..100` | Graphics > Distant traffic for one run (DISTANT_LIFE.md: decorative cars beyond the simulated traffic) |
| `VCS_DISTANT_BAND=lo,hi` | debug: the distant traffic's band in metres (`40,200` brings the proxies near, to look at them) |
| `VCS_DISTANT_TINT=1` | debug: the proxies lit red alone (told apart from the real cars in pictures) |
| `VCS_NODE_OVERFLOW_TEST=1` | every new sector entry from the overflow pool (its path checked without a crowd) |
| `VCS_WATCH=lo,hi[,val[,mask]]` | guest write watchpoint (rig `watch lo,hi[,val[,mask]]` arms it mid-run) |
| `VCS_WATCH_STACK=1`, `VCS_WATCH_STRIDE=stride,offset` | watch hits also list s2-s7 and the code addresses on the guest stack (callers); only writes to one field of every pool entry (e.g. `d20,1c8`: the peds' flags) |
| `VCS_TRACE_FILES=1` | game file API / inflate / GTAG relocation trace |
| `PS2X_TRACE_RPC=n` | first n SIF RPC events |
| `PS2X_LOG_MPG=1`, `PS2X_DUMP_MPG_DIR=d` | VU1 microcode uploads |
| `VCS_GS_STATS=s` | GS state histogram |
| `VCS_SELFTEST=1` | recompiled libgcc helpers vs native; RenderWare's clip flags (`VCLIPw`) on known vertices |
| `VCS_TRACE_IMMEDIATE=1` | callers of the Im2D / Im3D draw entries with their first arguments, counted every 5 s (which game code draws a primitive) |
| `VCS_TRACE_DELAY=1`, `VCS_TRACE_SOUND=1` | DelayThread calls; EE sound driver commands |
| `PS2X_TRACE_SPU=1`, `PS2X_TRACE_IOP_DMA=1` | IOP SPU2/DMA/interrupt tracing |
| `PS2X_TRACE_SPU2=1` | SPU2 key-ons, DMA bytes, IRQs, peak level per 5 s; per core: wet input, reverb output, ESA/EEA, EVOL, PMON / NON use |
| `PS2X_SPU2_REVERB=0` | mute the SPU2 reverb (A/B listening) |
| `PS2X_AUDIO=0`, `PS2X_AUDIO_DUMP=f.wav`, `PS2X_AUDIO_STATS=1` | no audio output / record the SPU2 mix / underrun and fill stats |
| `PS2X_IOP_REALTIME=0` | IOP time purely from EE cycles (audio slows with the game) |
| `VCS_TEST_SAVE=s[:slot]` | save to slot 1 (or 1..8) through the game's code s seconds after boot (mid-script: prefer `tools/saves.py`) |
| `VCS_TRACE_SECTORS=1`, `VCS_TRACE_CELL=1` | world sector draw / current hex cell vs camera |
| `VCS_DUMP_RAM=s,s,...` | write EE RAM to ram_<s>.bin at those seconds |
| `PS2X_DMA_HIST=1` | VIF1 payload per MB of RAM every 5 s |
| `VCS_TRACE_PACKETS=1` | peak use of the game's DMA packet buffer |
| F3 / F4, `PS2X_PERF_LOG=f.csv` | perf overlay / CSV recording (see CONTROLS.md); `python tools/perf_report.py f.csv`; since 2026-11-02 also `tex_create_us` (main thread making / releasing textures), `tex_kb`, `tex_decode_us` (the GS stage's decodes) and `host_max_ms` (the longest host frame within the game frame) |
| `PS2X_HLE_CLIPPED=1` | draw world packets that cross the clip planes with GPU clipping (wrong in places, game-internals.md 6h) |
| `PS2X_HLE_CLIPPER=0` | clipped world packets on the VU1 instead of the C++ clipper |
| `PS2X_HLE_ENV=0` | the environment-map pass (stage table 0x13) on the VU1 |
| `PS2X_HLE_REDRAW=0` | 0x02c0's re-draws / shine passes and the float-image prelit packets on the VU1 |
| `VCS_HUD_RW=0` | no HUD context at the RenderWare level (the 16:9 weapon icon goes back to the screen edge; no native HUD, no anchoring) |
| `VCS_HUD=0/2/1` | HUD in widescreen: keep 4:3 in the middle / screen edges / stretch (the menu's Display > HUD) |
| `VCS_WIDE_CULL=0` | widescreen: entities culled by the game's 4:3 camera planes (models vanish at the screen edges, as before 2026-10-27) |
| `VCS_TRACE_VISIBLE=1` | CCamera::IsSphereVisible: visible ratio every 5 s, samples with the camera-space point and the planes |
| `VCS_HUD_NATIVE=0` | the HUD drawn into the PS2 frame buffer instead of the native layer at the window's size |
| `PS2X_HW_PIXEL_CENTRE=0` | draws without the GS pixel-centre offset (softer copies, as before 2026-10-23) |
| `PS2X_HW_RT_FULL=0` | render targets read once per PS2 texel everywhere (higher internal resolutions lost in the copies) |
| `PS2X_HW_POSTFX=0` or letters `btc` | effects drawn as recorded / only these native: b bloom, t trails, c frame copy + motion blur |
| `PS2X_HW_SCALE=0` | native frame: the picture's targets at the window's size (menu: Internal resolution > Match window) |
| `python tools/rig.py burst DIR N` | the next N presented frames as DIR/burst_NNNN.png + DIR/burst.txt (shape, the renderer's batch) |
| `PS2X_HW_MENU_SWITCH=0` | widescreen: the pause menu goes 4:3 only after 32 frames without 3D (as before 2026-10-27) |
| `PS2X_HW_WIDE_SCALE=0` | integer scales in widescreen: the picture 640 x scale wide, stretched (as before 2026-10-27) |
| `PS2X_HW_MSAA=2/4/8` | MSAA for the picture's targets (menu: Anti-aliasing) |
| `PS2X_HW_ANISO=2..16`, `PS2X_HW_ANISO_LOG=1` | anisotropic filtering of the game's textures (menu: Texture filtering); count the draws per sampling path |
| `VCS_TEXTURE_REPLACEMENTS=0|1`, `VCS_TEXTURE_PACK=DIR` | optional PCSX2 DDS / PNG pack; path to SLUS-21590 or its replacements directory (`graphics.texture_replacements`, `graphics.texture_pack` in vcs.ini) |
| `VCS_TEXTURE_CACHE_MB=64..4096`, `VCS_TEXTURE_TRACE=1` | replacement GPU cache budget (512 MiB default; active textures protected for two frames); log computed PCSX2 identities |
| `VCS_MOD_FILES=0` | the mods' game files (GTA3PS2.IMG entries) not used - the disc's own |
| `VCS_STREAMING_MB=0|12|20|30` | Game > Memory > Streaming memory for one run: VCS's streaming heap (0: the PS2's 5.4 MB where it was; else that size at 0x2000000, past the PS2's 32 MB, and CStreaming's cap of 51 resources off; game/vcs_memory.cpp, game-internals.md 9e); `rig.py` command `memstate` reports both heaps |
| `VCS_TRACE_AREAS=1` | the world's area streaming: every area request, piece read / done and swap with its time (game/vcs_world.cpp, game-internals.md 9f) |
| `VCS_MODLOADER_DIR=DIR`, `VCS_MODS=0|1`, `VCS_TEST_MODS=1` | the mod loader's folder (default `modloader/` beside vcs.exe or in the checkout); Load mods for one run (fixes the switch); a test run with mods and their switches free (test runs load none otherwise) |
| `VCS_TEXTURE_DUMP=1`, `VCS_TEXTURE_DUMP_DIR=DIR` | every decoded texture once as an uncompressed DDS under its PCSX2 name (default `textures/SLUS-21590/dumps`): what a pack is made from (docs/TEXTURE_PACKS.md) |
| `PS2X_HW_GATHER=0` / `verify` | bilinear game textures through four loads with the wrap per tap (before 2026-10-30) / both paths, a pixel whose texels differ drawn magenta |
| `PS2X_GPU_TIMING=1` | GPU time per frame by marker (D3D11 timestamp queries; a marker per draw, named by kind, target and texture), every 5 s |
| `PS2X_RHI_LOG_CREATES=1` | each texture of 256x256 or more as it is created (a target re-created every frame shows as a stream) |
| `PS2X_HW_RT_REUSE=0` | render targets re-created as before 2026-10-30: a new texture at every drop and growth, FBP 420 scaled by each draw's width |
| `VCS_FPS=60` | 60 fps: the game's limiter waits one VBlank a frame instead of two (menu: Frame rate) |
| `PS2X_HW_FRAME=WxH` | the native frame's size whatever the window (tests) |
| `PS2X_HW_DUMP_ALLVERTS=1` | the F12 draw log prints every vertex of every draw |
| `VCS_MENU_NATIVE=0` | menus drawn into the PS2 frame buffer instead of the native layer |
| `VCS_LOADING_NATIVE=0` | loading screens drawn into the PS2 frame buffer instead of the native layer |
| `PS2X_HW_PATH2_PACKED=0` | PATH2 draw packets (Im2D, effect strips) through the GS front end vertex by vertex instead of recorded whole |
| `PS2X_TRACE_PATH2_TAGS=1` | PATH2 (VIF DIRECT) packets by the shape of their first GIFtag, and the register sequence of A+D packets that draw, every 5 s (`[path2]`) |
| `PS2X_WRITE_WATCH=0`, `PS2X_WRITE_WATCH_LOG=1` | guest RAM without the OS's write watch (no block can then be told unchanged); pages written per fold and the fold's cost, every 5 s |
| `VCS_RSL_ITEMS=0`, `VCS_RSL_ITEMS_VERIFY=1` | world / model draws as packets (no draw items); items built but the original writes the packets, compared with what each item stands for, and each block the walker copies with what an item would copy (FULLY_NATIVE.md step 4c) |
| `VCS_RSL_SLOW_LOG=1` | kicks whose item resolve takes > 3 ms (items, references, bytes) and chains the render worker takes > 15 ms on |
| `VCS_RSL_PLUGINS=0`, `VCS_RSL_PLUGIN_LOG=1` | the native flush leaves flushes with plug-in callbacks to the original (as before step 5f); log which callbacks the flushes left to the original would call |
| `PS2X_HW_PATH2_CONTINUED=0`, `PS2X_HW_PATH2_LEAD=0`, `PS2X_HW_PATH2_LINES=0` | A+D runs without their own PRIM / A+D packets with state writes before PRIM / Im2D lines through the GS front end (as before 2026-10-26) |
| `PS2X_HLE_IMAGE2=0`, `PS2X_HLE_DUMP_CODE=1` | MAR_A1's second 0x0030 code image on the VU1 (as before 2026-10-26); write the VU1 code memory the first times the world-image check passes / fails (cwd) |
| `PS2X_HW_FRONT_DRAW_LOG=<n>` | the first n draws the GS front end makes from GS primitives (PATH1 aside), with the GIF packet each came from |
| `PS2X_HW_PACKED_LOG=<n>` | the first n PATH2 packets the GS front end takes instead of the packed path (`submitPackedKick`), by their GIF tags; A+D packets only when they carry a vertex (FULLY_NATIVE.md 5e) |
| `VCS_RSL_RAW=0` | recordings refuse blocks with BASE / OFFSET / ITOP / MPG as before step 5e (instead of replaying them through the VIF interpreter) |
| `VCS_RSL_GAPS=0`, `VCS_RSL_RUNS=0`, `VCS_RSL_GAP_LOG=1`, `VCS_RSL_GAP_FNS=,<hex>,` | no gap captures; regions stop at their first kept tag (both as before step 5h); log frame 400's gaps; gaps only at the listed hook points (debug bisection, 0 = the kick) |
| `VCS_RSL_CENSUS=2` | the census names each tag left in the chain after the last hook point before it (hooked function and its caller) |
| `VCS_RSL_DRAW_CAPTURE=0`, `VCS_RSL_CAPTURE_CALLS=0`, `VCS_RSL_KEPT_LOG=1` | draws left to the original keep their packets; other CALLs end a captured region (both as before step 5f's end); log where each captured region not taken whole stops |
| `VCS_RSL_IM2D=0` | no captured emitters: Im2D (`FUN_002FB690`) and the effect / immediate-geometry writers keep their packets in the chain (FULLY_NATIVE.md steps 5c, 5d); `VCS_RSL_CAPTURE_MASK=<bits>`: which of them are captured (a bit per entry of `kCaptured` in `src/game/vcs_rsl_world.cpp`, default all) |
| `VCS_RSL_UPLOADS=0` | texture uploads (`FUN_003298D8`'s two REFs) as REFs in the chain instead of reference ops (FULLY_NATIVE.md step 5b) |
| `VCS_RSL_OPS=0` | the state setters (GS queue senders, texture matrix, `FUN_001CEF18`, `FUN_002F9CE8`, all-CNT flushes) write their packets instead of item ops (FULLY_NATIVE.md step 5a) |
| `VCS_RSL_CENSUS=1` | every 5 s: what the VIF1 chain still carries outside marked blocks and items - top-level tags by DMA id, first VIFcode and data region, per chain (FULLY_NATIVE.md step 5) |
| `VCS_RSL_SKIP=0`, `VCS_RSL_SKIP_VERIFY=1`, `VCS_RSL_SKIP_KINDS=<mask>` | copy every marked block; decide skips but copy anyway and check each against the recording it would have replayed; which kinds may be skipped (bit 0 world, 1 model, 2 model-b, 3 other call; default all) |
| `VCS_RSL_MEASURE=1` | with `VCS_RSL_WORLD_LOG`: per chain the bytes copied, the marked blocks and the distinct ones |
| `PS2X_HW_RT_LOG=1` | the render targets with their names (gsHwSetTargetRole), sizes and scales, every 5 s |
| `PS2X_HW_REFLECTION_SCALE=0` | the reflection map stays 1x with "Match window" |
| `VCS_RSL_RASTERS=0` | textures not named by raster (no upload REFs recorded) |
| `PS2X_HW_DECODE_FROM_UPLOAD=0`, `PS2X_HW_DECODE_VERIFY=1` | textures decoded from emulated VRAM (writing the queued uploads there first) instead of the queued upload's data; both, compared (`[gs-hw] textures from upload`) |
| `PS2X_HW_CLUT_FROM_UPLOAD=0`, `PS2X_HW_CLUT_VERIFY=1` | CLUT loads read emulated VRAM (writing the queued uploads there first) instead of the queued upload's data; both, compared (`[gs-hw] clut from upload`) |
| `VCS_TRACE_IM2D_DELAY=s`, `VCS_TRACE_IM2D_STACK=1` | start the Im2D trace after s seconds (default 30); print the guest stack's return addresses per call |
| `VCS_TRACE_HUDKEY=n` | log n 2D primitives the HUD classifier did not find (positions) |
| `VCS_TRACE_HUDANCHOR=n` | log n frames of HUD elements (box, part: 1 radar / 2 centred text, anchor) and each text line's justify and rect, from `VCS_TRACE_IM2D_DELAY` on (`tools/hud_check.py`) |
| `PS2X_HW_OUTPUT=WxH` | the renderer's output size whatever the window (full-screen sizes in test runs; with `PS2X_CAPTURE_SIZE`) |
| `PS2X_HW_DUMP_TEX=<key hex>` | write that decoded texture (colour + alpha PPM, keys from the F12 draw log) - analysis only, never commit |
| `PS2X_HW_HUD_LOG=1` | the renderer's HUD layers (per frame buffer, stale or not) and the output size, every 2 s |
| `VCS_TRACE_IM2D=n`, `VCS_TRACE_IM2D_MARKERS=1..5` | log n Im2D calls (caller, primitive, vertices) / the 2D pass's calls, from 30 s on |
| `PS2X_HLE_STATS=1` | native draw counters; 0x0030 stage tables still on the VU1 every 10 s (+ one code / data sample each) |
| `VCS_TRACE_RSL=1` | which functions build the frame's packets: calls and bytes per frame per function, every 5 s (`[rsl]`; game-internals.md 6i) |
| `VCS_RSL_WORLD=0`, `VCS_RSL_MODELS=0`, `VCS_RSL_WORLD_VERIFY=1`, `VCS_RSL_WORLD_LOG=1` | world blocks / model (and other CALLed) packets through the packet path instead of record / replay at the RSL draw; check every replay of either against the packet path; counts per kind every 10 s (`[rsl-world]`) |
| `VCS_RSL_SHADOW=1` | RSL shadow draw list: each draw call checked against its tag in the frame's chain, 3D tags outside draws listed, each draw program's VU state checked against the draw's RSL state (`[rsl-shadow]`, `state (1b)`; not with `VCS_TRACE_RSL`) |
| `VCS_RSL_SHADOW_DUMP=n`, `VCS_RSL_SHADOW_FIND=1` | with `VCS_RSL_SHADOW`: n samples of draw state + VU memory (`rsl_shadow_NNN.bin`); where in guest RAM mismatching VU rows are |
| `PS2X_CHAIN_CENSUS=1` | the VIF1 chain by tag location / id / data source: tags and quadwords per chain, every 5 s (`[chain]`) |
| `VCS_PROFILE_GUEST_FILE=<path>` | with `VCS_PROFILE`: every guest function's samples per report window, appended (for sums by subsystem) |
| `VCS_PROFILE_SPIKES=<ms>` | with `VCS_PROFILE`: a frame longer than <ms> gets its own report (`[spike] t=... ms frame`: its samples by guest function / host symbol, with call chains when `VCS_PROFILE_CALLERS` matches); `VCS_PROFILE_THREAD` may list several threads (`GameThread,RenderWorker,GsStage`) |
| `PS2X_WAIT_LOG=<ms>` | the EE scheduler logs waits longer than <ms> (pacing the EE clock to a deadline, or idle) with every guest thread's state, wait reason and pc / ra |
| `VCS_NATIVE_INFLATE=0`, `VCS_NATIVE_INFLATE_VERIFY=1`, `VCS_NATIVE_INFLATE_LOG=1` | zlib's inflate_fast recompiled as before; native decode compared with the original's output; counts, time and slowest call every 10 s |
| `VCS_NATIVE_MODEL_INDEX=0` | MAIN.SCM's model-id search (`FUN_00255188`) recompiled as before |
| `PS2X_VAG_DECODE=1` | the upstream VAG-to-PCM decode of sound files read through fio (for an unused playback path) as before |
| `PS2X_HW_DECODE_FAST=0` | textures decoded from their upload through the GS swizzle per texel even when the upload has the texture's own layout |
| `PS2X_HW_PALETTE_TABLE=0` | paletted textures decoded with the CLUT lookup per texel instead of a 256-entry table (PERFORMANCE.md 21) |
| `PS2X_HW_CPU_MIPS=0` | anisotropic filtering's mip chains by a render-target texture and `GenerateMips` on the main thread, as before 2026-11-02 |
| `PS2X_HW_EARLY_TEXTURES=0` / `=1` | new textures made on the main thread at their `TexCreate` instead of by the helper threads (the default with D3D11) / by the helper threads with Vulkan too (off there by default; PERFORMANCE.md 25) |
| `VCS_NATIVE_INFLATE_FAST=0`, `VCS_NATIVE_INFLATE_FAST_VERIFY=1` | the native inflate's byte-wise bit reader (before 2026-11-02); both readers on every call, output and state compared (PERFORMANCE.md 24) |
| `VCS_NATIVE_PARAMS=0`, `VCS_NATIVE_PARAMS_VERIFY=1` | the script VM's CollectParameters (FUN_00254B90) recompiled as before; the original runs and its words 1.. are compared with the native decode (PERFORMANCE.md 23) |
| `VCS_MUSIC_LOG=1` | the native music driver's commands (stream open / stop / preload ...) with the state it found |
| `PS2X_HW_LOG_PIPELINES=1` | each GS draw pipeline as it is made, with the time it took |
| `VCS_MUSIC_STOP_WAIT=1` | a playing music stream's stop waits for the next refill IRQ, as on the IOP (the level swap stood up to 448 ms for it; game-internals.md 9a) |
| `PS2X_WAIT_LOG_MAX=<n>` | with `PS2X_WAIT_LOG`: log up to n waits (300) |
| `PS2X_FRAME_STATS=0` | the brightness probe off even with the control channel (its GPU read-back each host frame hides GPU-side hitches; tools/cruise.py) |
| `python tools/cruise.py run OUT [ENV=VAL ...]` | the stutter rig: the car along a fixed route at 144 km/h, the per-frame CSV and a hitch report (PERFORMANCE.md 21) |
| `VCS_PROFILE_CALLERS=name[,name]` | with `VCS_PROFILE`: for samples in host functions whose name contains one of these, the unwound call chain up to the first guest function (`called from <sampled function> < <caller> ...`; an unwind fault ends the chain, not the process) |
| `VCS_RSL_NATIVE=0`, `VCS_RSL_NATIVE_VERIFY=1`, `VCS_RSL_NATIVE_LOG=1` | the original RSL state flush instead of the native one; the original runs and the native bytes are compared with it; counts every 10 s (`[rsl-native]`) |
| `PS2X_HLE_PACKED=0`, `PS2X_HW_PACKED_VERIFY=1` | the C++ reproductions' strips / fans through the GS front end; check the packed recording against it per packet |
| `PS2X_HLE_SETUP=0` | VU1 setup programs (matrices, slot routines, block copy) on the VU1 instead of natively |
| `PS2X_HLE_DUMP_IMAGES=1` | write the code and data of the first 0x02c0 shine packet of each code image (`shine_<hash>_*.bin`) |
| `PS2X_HW_DRAW_SOURCES=1`, rig `gsdraws [reset]` | draws still made from GS primitives by kind and source, per frame (logged every 5 s / on request); totals always in the perf line (`gs_draws` ...) |
| `PS2X_CLIP_DUMP=dir[,n]` | with `PS2X_HLE_VERIFY=1`: write n clipped packets (VU memory + the VU's kicks) for offline checks |
| `PS2X_HW_LAZY_UPLOADS=0`, `PS2X_HW_UPSTATS=1` | write texture uploads immediately / upload stats |
| `PS2X_SLOW_VBLANK_MS=n` | stall the EE n ms per VBlank (simulate a slow PC) |
| `PS2X_VU0_VERIFY=1`, `PS2X_VU0_NATIVE=0` | check / disable native VU0 programs |
| `PS2X_VU1_VERIFY=1`, `PS2X_VU1_COMPILED=0` | check / disable compiled VU1 code |
| `PS2X_TRACE_VU0=1`, `PS2X_TRACE_VU1=1`, `PS2X_VU1_STAT=1` | VU program statistics |
| `VCS_COUNT_CALLS=addr,...` | calls per 5 s of guest functions |
| `VCS_RWATCH=lo,hi` | which code reads a guest range |
| `PS2X_HW_DUMP=<frame>` | dump HW render targets + display as PPM |
| `python tools/run.py N name --hidden` + `python tools/gamectl.py ...` | hidden test run driven over the control channel |
| `python tools/gamectl.py gameinfo` | the F3 game-state panel's lines: player (position, heading, state, health), vehicle, camera, clock / area / pauses, pools, scripts |
| `python tools/scenes.py capture OUT` / `compare REF NEW` | reference scenes (screenshots + perf) from an empty card; diff two captures |
| `VCS_DATA_DIR=dir` | save / cache folder instead of %LOCALAPPDATA%/VCSRecomp (Linux: $XDG_DATA_HOME/VCSRecomp) |
| `PS2X_D3D_DEBUG=1` | D3D11 debug layer (needs Windows' Graphics Tools feature) |
| `VCS_RHI=d3d11\|vulkan\|null` | the renderer for one run (Graphics > Graphics API when the build has two; docs/LINUX.md) |
| `PS2X_RHI_DEBUG=1` | the API's validation layer: D3D11 debug layer / Vulkan's Khronos validation |
| `PS2X_RHI_STATS=1` | Vulkan: draws, descriptor writes, passes, suspended passes, barriers, uploads, CPU waits per frame, and input to GPU done (avg / p50 / p95) every 5 s; gs_hw: the input delay by stage |
| `PS2X_RHI_GPU_TIME=1` | Vulkan: GPU time of the passes and the whole frame (`PS2X_GPU_TIMING=1`: per draw, breaking passes) |
| `PS2X_HOST_SPIKES=<ms>` | main-loop frames whose work went over it, by phase (events, begin, frames, ui, window, present) with the main thread's CPU time; Vulkan: the frame's waits, draws with their CPU time in `prepareDraw` / `vkCmdDraw`, pipeline binds, descriptor pushes, passes, uploads, and the fence wait after it (PERFORMANCE.md 26) |
| `PS2X_RHI_LOG_MEMORY=1` | Vulkan: every device memory allocation / free VMA makes, with its size and time |
| `PS2X_RHI_STATIC_DEPTH=1` | Vulkan: depth test / write / compare in each pipeline, as before 2026-11-02 (now dynamic state, pipelines differing only there shared: PERFORMANCE.md 27) |
| `PS2X_RHI_FULL_COPY=1` | Vulkan: a dynamic vertex / index buffer's whole capacity copied into the frame's ring, as before 2026-11-02 (now the bytes last written) |
| `PS2X_RHI_REDUNDANT=1` | Vulkan: viewport, scissor, blend constants and vertex / index buffers recorded at every draw that set them, as before 2026-11-02 (now only when they change) |
| `PS2X_HW_CB_DEDUPE=0`, `PS2X_HW_CB_LOG=1` | the GS draw / native constants written at every draw, as before 2026-11-02 (now only when they change); writes and skips every 5 s |
| `PS2X_HW_NATIVE_MERGE=0` | one native draw per VU1 batch, not joined (PERFORMANCE.md 32) |
| `PS2X_HW_SHADER_DEPTH=1` / `=0` | the GS pixel shader writes the depth / uses the rasterised depth, early depth rejection on (the default with D3D11 and Vulkan since 2026-11-03: with the shader depth D3D11 lost the game's fire, game-internals.md 6m; PERFORMANCE.md 26) |
| `PS2X_RHI_FULL_BARRIERS=1` | Vulkan: whole-pipeline barriers, as before the layout-based ones |
| `PS2X_HOST_PACED=0`, `PS2X_FRAME_LEAD=0`, `PS2X_FRAME_LEAD_MARGIN_US=<us>` | frame-rate independent timing: the game's own deadline instead of the host's frames; no frame lead; the lead's margin (1500) - PERFORMANCE.md 19 |
| `PS2X_WRITE_WATCH=1` (Linux) / `=0` (Windows) | the guest RAM write watch on Linux (opt-in there) / off on Windows - PERFORMANCE.md 20 |
| `PS2X_HW_SPECIALISE=0` | the GS pixel shader's full version for every draw (not one per texture read; PERFORMANCE.md 18) |
| `PS2X_HW_QUEUE=<n>`, `PS2X_HW_BATCH_MAX=<n>` | frames the game may queue ahead / frames rendered per host frame (0: all); defaults 1 / 1 with frame-rate independent timing, 3 / all with the original (PERFORMANCE.md 17) |
| `VCS_VK_DEVICE=<n or name>` | Vulkan: which GPU |
| `PS2X_HLE=0` | draw the world through the VU1 program instead of natively (src/game/vcs_hle_world.cpp) |
| `PS2X_HLE_VERIFY=1` | compare the C++ world pipeline with the VU1's output (bit-exact expected) |
| `PS2X_HLE_SURVEY=1`, `PS2X_HLE_SURVEY_DUMP=n`, `PS2X_HLE_SURVEY_DUMP_EVERY=n` | VU1 program / layout survey, sample dumps |
| `PS2X_HLE_STATS=1` | stage tables the native handler passes over |
| `PS2X_HLE_LIT=0` | draw skinned / lit models through the VU1 program (world stays native) |
| **F2** (or gamepad Back + Start) | settings menu (docs/SETTINGS_MENU_WIDESCREEN.md); its values live in vcs.ini next to vcs.exe |
| `display.window=0\|1` in vcs.ini, `VCS_FULLSCREEN=0\|1` for one run, Alt+Enter | windowed / full screen (borderless) |
| **F8** (`controls.screenshot_key`), `VCS_SCREENSHOT_DIR=<dir>` | screenshot of the window as shown (its resolution, menus / overlays included) to `screenshots/` next to vcs.exe, or that folder (docs/CONTROLS.md) |
| `PS2X_HIDDEN_WINDOW=WxH` | a hidden test run's window at that size instead of 640x448 (a player's window: the screenshot key at 2560x1440) |
| `display.aspect=43\|1610\|169\|219\|329\|0` in vcs.ini, `VCS_ASPECT=...` for one run | aspect ratio (Hor+ widescreen; 0 = match the window) |
| `display.hud=0\|1` | widescreen HUD: keep 4:3 in the middle / stretch |
| `window.*` in vcs.ini | where the window was last (monitor, position, size, maximized); written by the game, delete the lines to reset (docs/SETTINGS_MENU_WIDESCREEN.md) |
| `python tools/rig.py start --save DIR`, `python tools/rig.py <command>` | automation rig: start from a card copy; peek / poke / findvec / dump / scm / scripts / player / pause |
| `python tools/tour.py [OUT] [--step 250] [--every K]` | world tour over the map (classes, map.png, screenshots, perf) |
| `python tools/missions.py [OUT] [--only 45,46]`, `python tools/drive.py [OUT]` | mission smoke test; drive loop |
| `python tools/regress.py [OUT] [--quick] [--set-baseline]` | regression suite against build/regress/baseline |
| `PS2X_FRAME_STATS=1` | brightness probe without the control channel (black / dip frames in the perf line and log) |
| `VCS_TRACE_TELEPORT=1` | log every ped teleport (FUN_0020b3d8) with its caller |
| `VCS_TRACE_PED_REMOVE=1` | log every ped flagged for removal (FlagToDestroy, FUN_00210780) and deleted (FUN_00198408): handle, position, model, created-by, flags, last script, callers |
| `display.scale=<1..8>` (old `scale=` still read) | internal render resolution, as `PS2X_HW_SCALE` (the environment wins) |
| `python tools/vudis.py code.bin --start 0x30 --to-ebit` | VU microcode disassembler |
| `python tools/sendkey.py Return`, `python tools/screenshot.py f.png` | key press / screenshot (control channel if open, else the window) |
| `python tools/eedis.py <elf> <addr or name>` | EE disassembly with function names |
| `python tools/minidbg.py <exe> <iso>` | catch fail-fast crashes with a stack |

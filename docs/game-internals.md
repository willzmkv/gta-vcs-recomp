# Game internals - GTA: Vice City Stories (PS2, SLUS-21590)

Everything known about how the game works inside, gathered while recompiling it. Addresses are EE
RAM addresses in SLUS_215.90 (identical to ELF virtual addresses; see [DISC.md](DISC.md)). Names in
`CamelCase::Like()` are our labels, modelled on the GTA III-era naming (re3 / reVC) where the code is
clearly the same lineage; they are guesses until marked **confirmed**.

Each fact says how it was established, so it can be re-checked. Tools: `tools/eedis.py` (EE
disassembly with names from `game/vcs/functions.csv`), `tools/vudis.py` (VU microcode), Ghidra
(`ghidra/`), the runtime's traces (`PS2X_*`, `VCS_*` switches in PROGRESS.md), the control channel
(`tools/gamectl.py`) and frame dumps (`PS2X_HW_DUMP`). Listings derived from game code go to
`build/analysis/` and are never committed.

**When you learn something about the game, add it here in the same change.**

---

## 1. Background (outside sources)

- Rockstar Leeds engine, shared with the PSP original (2006). The PS2 port (2007) reuses PSP-side
  data formats (`.PSP` frontend files).
- Descends from the GTA III / VC codebase but renders through Leeds' own "RenderWare Stories"-style
  layer (RSL), not stock RenderWare.
- Runs at 30 fps with a frame limiter. GTA III-era code scales movement by `CTimer::ms_fTimeStep`
  (1.0 = 1/50 s), so 60 fps mostly works, but earlier games had known 60 fps bugs (physics, some
  animations, script timers, water cannon). The PSP recomp (OverkillLabs3) and ThirteenAG's PPSSPP
  fix both patch these for 60 fps - references for milestone 4 of NATIVE_PORT_ROADMAP.md.

## 2. Code map

- 9,550 functions (Ghidra 12.1.4 + EE extension) plus 833 indirectly referenced ones Ghidra missed
  (`tools/find_missing_functions.py`). 8,521 below `0x443000` are game code; 1,029 from `0x443000`
  up are Sony SDK / libc / libgcc.
- SDK libraries, identified by ps2_analyzer's SCE symbol DB (`game/vcs/vcs.toml`):
  - `0x4431F8..` libgcc (`__divdi3`, `__muldi3`, soft-double `dpadd`...)
  - `0x447670..` libgraph (`sceGsResetGraph`, `sceGsSwapDBuff`, `sceGsSyncV`)
  - `0x448688..` libdma (`sceDmaSend`...)
  - `0x448D98..` libpad (`scePadRead`... - libpad, not libpad2)
  - `0x44E5F8..` libcdvd incl. **streaming** `sceCdStInit/StStart/StRead/StStop`
  - `0x44F668..` libmc (`sceMcOpen/Read/Write/GetDir`...; stubs: `0x450120` = `sceMcGetDir`,
    `0x44fe80` = `sceMcSync`)
  - `0x452320..` libmpeg (`sceMpegCreate`, `sceMpegDemuxPssRing`) + libipu
  - `0x45C1D8..` libsdr (`sceSdRemoteInit`, `sceSdRemote`) - audio goes over SIF RPC to SDRDRV.IRX
  - `0x467xxx` kernel syscall wrappers, `0x469E00..` libsif / RPC, `0x46B980..` libfileio
  - `0x46EA98..` Deci2 (dev-kit debug channel; dead on retail)
- `fiprintf` at `0x1A9EF0` lives inside game code (newlib linked early). `FUN_0045f378` = `sprintf`
  (seen building memory card paths).
- Largest game functions (state machines / script interpreter candidates): `0x141FE8` (41.7 KB),
  `0x13A0A0` (25 KB), `0x337208` (25 KB), `0x33F010` (20 KB).
- The analyzer found only 2 jump tables; the recompiler reports ~1,150 unresolved `jr` sites (switch
  tables) and falls back to dispatch through the function table. The memory card state machine
  `FUN_002788e0` is one (`jr` through the table at `0x4B0400`, indexed by the state at `0x487730`).

- Small hot leaf functions seen in game-thread profiles (2026-10-25): **FUN_003F9E50** = 4x4 identity matrix at $a0
  (zeroes 16 words in a loop, then 1.0 at +0, +0x14, +0x28, +0x3C; native since 2026-10-25); **FUN_00408058** = rand()
  (Park-Miller: seed at 0x48A5B8, q 127773, a 16807, r 2836; `div` then two `mult`, + 0x7FFFFFFF when <= 0);
  **FUN_002560D8** = the script VM's command dispatch (reads the 16-bit opcode at the script's ip, bit 15 = negate the
  result, handler table at 0x4BEAF0, 8 bytes per entry).

## 3. Boot

- Entry `0x001DD6A0` is standard crt0: zero GPRs / FPRs, clear .bss (`0x4CD100..0x74DF54`), set
  gp / sp, SDK init, `main`.
- `sceSifInitRpc`, then IOP modules from the disc in this order: `SIO2MAN, LIBSD, SDRDRV, CDVDSTM,
  CDSTREAM, PADMAN, SIO2MAN (again), MCMAN, MCSERV`. All nine run as real IRX code on the emulated IOP
  (SDRDRV prints "SDR driver version 4.0.1"); `SDRDRV`, `CDVDSTM`, `MCMAN`, `MCSERV` stay resident.
- `_lf_bind` (`0x46CD10`) polls for the ROM LOADFILE server - absent in the recomp, stubbed.
- **Disc indexing**: `FUN_002209F0(path)` walks `cdrom0:` recursively with
  `sceDopen/sceDread/sceDclose`, and for each file (`st_mode & 0xF000 == 0x2000`) calls the
  `sceCdSearchFile`-like `0x44E480` and appends (name ptr, lsn, size) to a table at `0x5038A8`
  (count at `0x486E8C`, string pool cursor at `0x486E88`). Every file is resolved to an LSN up
  front; reads are by sector afterwards.
- **Movies** (milestone 5, 2026-10-24): `FUN_0021CA70` (from `main`) initialises the player (FUN_0040D7D8), calls
  **CMoviePlayer::Play(path, 0, n) at 0x40D8B8** for `cdrom0:\MOVIES\STINGS.PSS;1` (0x4AC5F8, the Rockstar logos,
  n = 1,000,000) and `cdrom0:\MOVIES\INTRO.PSS;1` (0x4AC618, n = 10), shuts it down (FUN_0040D898) and clears
  the scratchpad. Play -> FUN_0040D930 (libmpeg + IPU, sound through the IOP) compares a frame count with n before
  it reads the pad: the logos can't be skipped, the intro can after 10 frames. `INTROP` / `STINGSP` are the PAL
  versions (unused by the USA game).
- **PSS layout** (read from the files): an MPEG-2 program stream; video in stream 0xE0 (MPEG-2, 512x448, 29.97 fps,
  shown across a 4:3 TV; STINGS 10.6 s, INTRO 84 s); sound in private stream 1 (0xBD), sub-stream 0xFF, each
  packet's payload behind a 4-byte header (`ff a0 00 00`) and split at odd byte counts. The joined bytes are a Sony
  "SShd" header (u32 size 0x18, type 1 = **16-bit little-endian PCM**, 48000 Hz, 2 channels, interleave 512 bytes),
  "SSbd" + u32 data size, then blocks of 256 left samples, then 256 right (byte order and interleave told apart by
  how smooth the samples are). First PTS: sound 0.043 s, video 0.110 s. FFmpeg's demuxer finds the video only.
- **Played natively** (`src/game/vcs_movie.cpp`): Play is replaced; the file read from the disc, demuxed, the video
  decoded with FFmpeg and shown by the presenter at 4:3 (`ps2ShowMovieFrame`), the sound mixed into the audio
  output (`ps2AudioMoviePush`), whose played-sample count is the clock (the host clock without audio output, and
  after the sound ends - the last pictures are due 30 ms after it). Start / Cross skip once the game's count
  allows. Display > Skip intro movies skips both. The game thread waits in Play, as in the game's player.

## 4. Frame and timing

- The game updates and draws at 30 fps on the 60 Hz VBlank: in a frame dump the 3D is in every
  other VBlank's GS frame; the in-between VBlanks carry no draws (found when `PS2X_HW_DUMP` kept
  landing on empty frames, 2026-10-22; the dump now waits for a frame with draws).
- **Main loop** (2026-10-23, from the guest-stack return addresses of the 2D work and the callers in the generated
  code): `entry` (0x1dd6a0) -> FUN_0021c928 -> FUN_0021c5e8 (start-up: init calls, then the loop) -> per frame
  FUN_0021ed88: CTimer / game updates (FUN_002f4858 / FUN_002f4960 / FUN_0021e720 ...), FUN_0021e950 (render: 3D,
  then the 2D pass FUN_0021f348, the frontend FUN_0021f7d8), then the frame's end.
- **VBlank**: FUN_00370480 creates a semaphore (id at 0x488E9C) and installs **FUN_00370300** as the INTC handler for
  VBlank start (cause 2) and end (3), through FUN_002f46f8 (AddIntcHandler + EnableIntc with a use count). On
  VBlank start it sets a flag (0x6F4268 = 1), counts VBlanks (0x488EA0) and calls up to 8 callbacks registered with
  FUN_003703f0(fn, arg) (table 0x6F4248, args 0x7490B0); on VBlank end it clears the flag. FUN_003703a8 returns the
  count, **FUN_003703b8 = WaitVBlank** (spins on the flag, clears it).
- **Frame limiter**: the graphics object at 0x520F90 counts VBlanks at +0x4 (callback FUN_00317aa8 ->
  FUN_00317ac8: +1, registered by FUN_00317788). At the frame's end **FUN_003179a0** (from FUN_0018f450) waits for
  the previous frame's work, kicks the next display list (FUN_00272268), then `do { WaitVBlank(); } while (vblanks
  < 2);` (0x317a48..0x317a58) and resets the count: **30 fps**, two VBlanks a frame.
- **CTimer** (read from the generated code of Update and Initialise, 2026-10-23; names as in reVC, whose
  CTimer::Update has the same shape): FUN_002f4858 = Initialise (registers VBlank callback 0x2f4840, which only
  counts VBlanks at 0x488290; step = 1.0, time scale 0x4CD168 = 1.0, pauses / times / frame counter = 0).
  **FUN_002f4960 = Update**:
  - Ticks: **FUN_00451C00(timer id 0x488288)** = a kernel timer's 64-bit count at the bus clock (147.456 MHz),
    only ever called from here; previous count at 0x488298, this frame's ticks at 0x4CD564. FUN_002f47c0 creates
    the timer when missing (called twice per Update); FUN_0030c984 = `cvt.w.s` (float -> int, truncating).
  - Clocks: 0x488280 (ms, also while paused) += ticks / 147456; `m_snPreviousTimeInMilliseconds` 0x4CD16C;
    **`m_snTimeInMilliseconds` 0x4CD104** and 0x4CD448 += (int)(ticks * scale / 147456) - truncated per frame.
  - Paused (`m_UserPause` 0x4CD2AC or `m_CodePause` 0x4CD11C): the step goes to 0x4CD174 (pause-mode step) and the
    frame's ticks are zeroed, so the game clocks stand still.
  - **`ms_fTimeStep` 0x4CD110** = ticks * scale * 50 / 147456000 (1.0 = 1/50 s; 0x34B60B61 = 3.3908e-7), then
    **at least 0.5** (1/100 s; VC had 0.01) unless paused or a chase recording (0x481CF8) is on;
    `ms_fTimeStepNonClipped` 0x4CD300 = that; then unless replaying (0x4CD444 == 2) **at most 3.333** (1/15 s) and
    the ms clock at most +60 per frame. Chase recording mode (0x4CD138 == 1): step 1.0, clock +16 ms.
  - **`m_FrameCounter` 0x4CD124** += 1.
  - (The earlier note "time step at 0x4CD174" was wrong: that is the pause-mode step.)
- **Frame-rate dependence found in CTimer itself**: the ms clock adds each frame's milliseconds truncated, losing up to
  1 ms a frame - game time (script waits, timers, the in-game clock) ran 1.5% slow at 30 fps and ~4% at 80 fps
  (measured: game clock vs wall clock, `m_snTimeInMilliseconds`). And the 0.5 minimum step makes the game run fast
  above 100 fps. Independent timing feeds whole milliseconds of ticks and lifts the minimum (below).
- **Display flip FUN_003706c8** (right after the limiter's wait): toggles the buffer index (+0x44 of the struct at
  *0x488E88), reads CSR, writes PMODE, SMODE2, DISPLAY1/2, DISPFB1/2. The limiter's first loop waits VBlanks until
  the previous display list is done (+0xC of 0x520F90 back to -1; set by the DMA end); with our DMA completing at
  once it does not loop. In gameplay the limiter is the only caller of WaitVBlank (counted by return address).
- **Independent timing** (`src/game/vcs_framerate.cpp`, Display > Game timing): FUN_00451C00 returns the host clock
  in whole milliseconds of bus ticks (`VCS_TIMESTEP=<fps>`: exactly 1/N s per frame, unpaced); the second
  FUN_002f47c0 call (ra 0x2F4C04) puts back a step the 0.5 minimum raised (floor 0.05); the limiter's WaitVBlank
  (ra 0x317A50) presents the frame, paces to the cap on the host clock, brings the EE clock up to real time on the
  VBlank chain (`EeScheduler::syncToRealTime`) and sets the VBlank count to 2. Measured: game clock = wall clock
  (x1.001) at 30 and 144; steps 1.65-1.70 at 30, 0.55-0.60 at ~85 fps.
- **Steps below 0.5 break the game** (frame-rate audit `tools/framerate_audit.py`, 2026-10-24): with the minimum lifted
  the player runs at 4.7 m/s up to 1/100 s steps, then 1.4 m/s at 1/110, 1.0 at 1/120, 0.5 at 1/240 (per-frame
  movement shrinks with the step squared); with the game's own 0.5 floor at 1/110 he runs 5.15 m/s (= 4.7 x 0.5 /
  0.4545, the step just too long). Code was written for steps >= 0.5 (VC's minimum was 0.01; VCS raised it) - not
  found yet. Independent timing therefore kept the 0.5 floor and capped at 100 fps until 2026-10-25 (below).
- **Found (2026-10-25): FUN_001f1d38 (ped animation movement)** turns the frame's animation root displacement into the
  ped's target velocity at +0x794 / +0x798 (FUN_001f25f0 then eases the move speed +0x140 towards it):
  `if (ms_fTimeStep < 0.5) v *= 0.5; else v *= 1.0 / ms_fTimeStep;` (0x1f2154 - 0x1f21c0, the 0.5 loaded once by
  `lui $at, 0x3F00` at 0x1f2158). The `< 0.5` branch never runs on the PS2 and is wrong - `* 0.5` where `/ step` is
  2 or more - so below the floor the ped got ~a quarter of its velocity. At 1/110 s (`VCS_TIMESTEP` feeds 9 / 10 ms
  frames, steps 0.45 / 0.5) the speed alternated 0.021 / 0.083 units - found with a write watch on the player's move
  speed (`VCS_WATCH`, writers by return address), then on +0x794. Patched (game/vcs/overrides.toml: the constant
  becomes 0.001, so every real step takes the `/ step` path); steps >= 0.5 are unchanged. Audit afterwards: run /
  drive / fall at 1/144 - 1/240 as at 1/30 - 1/100, sprint -5% / -11% at 1/200 / 1/240. Independent timing's ceiling
  is now 240 fps (step floor 50/240, `vcs_framerate.cpp` kMaxFps).
- **Found (2026-10-25): FUN_0031ae38 = CCutsceneObject::ProcessControl** (as in reVC) has the same dead branch:
  `if (ms_fTimeStep < 0.5) speed *= 2 (add.s x, x, x); else speed *= 1 / ms_fTimeStep;` (0x31b19c - 0x31b234, the
  0.5 from `lui $at, 0x3F00` at 0x31b19c, used only for the compare), then the object's offset +0x210 += step * speed
  and its position = +0x200 + +0x210. Below the floor each frame kept only 2 * step of the animation's root movement,
  so cutscene characters drifted off their animation: at 144 fps (step 0.35) Vic floated over the trailer and Marty
  sat sunk in his truck in MAR_A1 "Shakedown"'s opening cutscene (user report). Found by checking that the cutscene was
  right with original timing and with `VCS_MIN_STEP=0.5` at 144 fps, then scanning the generated code for
  `lwc1 -0x2EF0(...)` (the step) next to a `lui 0x3F00` and a compare. Patched like the ped one
  (game/vcs/overrides.toml: 0.001). The scan's other hits are not this pattern: FUN_001f4bf0 / FUN_0033bb2c use
  `max(step, 0.5)` (below), FUN_00348128 compares `step * 0.5` with 0.05, FUN_002f4960 is CTimer itself.
- **Audit results in the supported range** (`tools/framerate_audit.py`, steps 1/15 .. 1/100, per simulated second,
  2026-10-24): game clock exact (1/15 gets 60 ms a frame: the +60 ms cap, below ~17 fps the game slows, as on the
  PS2); car acceleration the same within 3% (speed at 2 / 3 / 4 s, distance in 4 s); running / sprinting the same
  from 1/30 up (1/15 runs ~10% faster); **falling and jumping depend on the frame rate**: 15 m of fall takes 0.90 /
  1.11 / 1.19 / 1.20 s at 1/15 / 30 / 60 / 100, a jump rises 0.73 / 0.81 / 0.86 / 0.88 m. **This is the
  engine's integration, not a per-frame bug**: the jump heights follow explicit Euler's error exactly - a peak
  v0^2/2g - v0*dt/2 with a continuous peak of 0.893 m predicts 0.726 / 0.81 / 0.85 / 0.868 (measured 0.725 / 0.814 /
  0.864 / 0.884) - and the ped's vertical speed agrees within 1-3% between 1/30 and 1/60 through a whole fall,
  quadratic air drag included (traced at ped+0x148). Shorter frames are closer to the continuous motion, as on the
  PC GTAs; matching 1/30 exactly would need a correction term in the integration (not done: <= 6%).
- **A `max(step, 0.5)` in the bike's lean**: FUN_00337208 is **CBike::ProcessControl** (not ped code, as first noted:
  it calls ProcessWheel FUN_003E7618, the springs FUN_0034E0E8 / FUN_0034E1D8 / FUN_0034E348, and keeps the bike
  fields of reVC's CBike - lean +0x680 / +0x684, wheel ratios +0x540..). At 0x33bb2c it divides the frame's sideways
  change of speed by `max(ms_fTimeStep, 0.5) * 0.008` (GRAVITY; VC: max(step, 0.01)) for the lean angle (+0x684,
  radians; the physics matrix stays upright, the lean is drawn and steers the wheels). Above 100 fps the bike leant
  less: a gentle turn peaked at 22 degrees at 144 fps against 30 at 30 (yaw rate the same; hard turns hit the lean
  limit, ~42 degrees, at both). **Patched 2026-10-28** like the ped one (0.5 -> 0.001, `lui $at` at 0x33BB10; the
  0.5 is only the max.s operand).
- **The same `max(step, 0.5)` elsewhere** (scan for `lwc1 -0x2EF0` then max.s / c.lt.s with a `lui 0x3F00` nearby,
  2026-10-28). **Patched 2026-10-28** (0.5 -> 0.001 as the others, game/vcs/overrides.toml; above 100 fps only):
  - FUN_00138b68 (cars in water; the 0.5 loaded on two paths, 0x138c48 / 0x138c6c, max.s at 0x138c88) and FUN_00336cb8
    (bikes in water, 0x336d60 / 0x336d78) divide the buoyancy impulse's z by `mass * max(step, 0.5) * GRAVITY` for the
    share under water, which sets the water's drag (`pow(1 - share * 0.05, step)`) and the sinking thresholds. Measured
    (a vehicle dropped from 8 m into the sea at (-675, -75), water at z ~5.9, with 15 m/s forwards): a Sentinel kept
    1.6 m/s one second into the water at 144 fps, 0.6 at 30; a PCJ-600 sank to the bottom (z 1.2 on average over 3-8 s)
    where it settles at 3.2. After: 0.75 / 0.69 / 0.76 m/s and depth 2.75 / 2.9 / 2.95 m at 1/30 / 60 / 144
    (`framerate_audit.py` carwater, bikewater).
  - FUN_0019fda0 (CBoat::ProcessControl, 0x1a2468 / 0x1a2480) multiplies one rudder turn force by `max(step, 0.5)` where
    the others use the step. A Speeder steered for 1.5 s: turn rate the same (42 degrees/s), speed after the turn 19.7
    m/s at 144 fps against 21.4 at 30, 21.1 after the patch - within the waves' noise, patched on the code.
  - Left: FUN_001f4bf0 (CPed::ProcessControl, 0x1f7378 / 0x1f7474 / 0x1f7bac).
- **The buoyancy's lift cut-off is per frame on paper, but no effect measured**: FUN_00369950 (from FUN_00369450, the
  buoyancy for cars, bikes and boats) makes the lift impulse `share * buoyancy * step` and drops it to
  `max(lift - momentum, 0)` when the vehicle rises with more than 4 times that impulse (`lui $at, 0x4080` at
  0x369994) - 4 frames of lift. Patched to scale the 4 with the step and measured: a Sentinel floats at z 6.02 on
  average at 30 fps and 5.96 at 144 with or without it (bobbing +-0.3 m). Not kept.
- **Vehicle physics that acts once a frame without the step** (found 2026-10-28 from a user report: a car running
  over a body jumped, a stolen bike flipped forward, both at high frame rates only). Fixed in
  `src/game/vcs_physics_timing.cpp` (scaled by step / (50/30) when the frame is shorter than the PS2's;
  `VCS_PHYSICS_TIMESTEP=0` off):
  - **FUN_003F34F0, the bump of a body under a wheel**: called after each wheel's ProcessWheel, for cars from
    FUN_00152E18 (a pair of wheels, from CAutomobile::ProcessControl FUN_0013A0A0 at 0x13e478 / 0x13e4cc) and for bikes
    from CBike::ProcessControl. If the vehicle has a driver (+0x384), the player drives (FUN_0020C310), its type
    (+0x354; 0 cars, 6 / 8 bikes) isn't 1 / 2, up.z >= 0.1 and it moves (|v|^2 >= 0.0001), it walks the driver's
    nearby peds (driver +0x6F8, count +0x77A): one that is dead (state +0x8C4 == 0x3A) or knocked down (FUN_002192E0)
    within 1 m horizontally of the wheel's contact point gets the vehicle `ApplyTurnForce((0, 0, mass * 0.01, * 1.5
    for cars), contact point)` - per frame. Measured (`tools/framerate_audit.py` scenarios runover / bikebody; a dead
    ped 14 m ahead of a Sentinel at full throttle): the car's front wheels on the body pitched it at 0.042 rad / 0.02 s
    at 30 fps but 0.072 at 144; its rear bumper then hit the ground (CPhysical::ProcessCollisionSectorList FUN_0034FD60
    -> ApplyCollisionAlt FUN_0034D670 against the road) and threw the car up at 3.3 m/s (1.4 at 30 fps; rise 0.55 m
    against 0.17). A PCJ-600 rolling slowly past a body at its rear wheel ended on its nose (forward vector z -0.995)
    at 144 fps; at 30 it pitches to -0.29 and settles. Found with the rig's `trace` (per-frame vehicle state) and write
    watches on the car's move speed z (+0x148) and turn speed x (+0x70), summing each writer's changes (return address
    and the callers on the stack): at 144 fps most of the extra pitch came from ApplyTurnForce called at 0x3F36CC.
  - **ProcessWheel's rolling resistance** (FUN_003E6C18 cars, FUN_003E7618 bikes; CVehicle::ProcessWheel, args f12
    thrust, f13 brake, f14 adhesion (multiplied by the step inside), a0 vehicle): with brake 0, thrust 0 and the gas
    pedal (+0x25C) under 0.01, the brake becomes 0.9 / handling mass (handling +0xBC; 0.09 under 500 kg, 0.18 for the
    RC Bandit (model 0xFC51), 0.54 / (mass + 200) for vehicle types 6 / 8, 0 for type 5; the bike version has 0.18
    under 500 kg) and clamps the wheel's forward correction - per frame, never times the step (reVC fixes the same line
    with `brake *= CTimer::GetTimeStepFix()` under FIX_BUGS). Coasting from 10 m/s a Sentinel slowed at 3.4 m/s^2 at
    30 fps and 16 at 144; a PCJ-600 at 2.4 and 11, which pitched it forward. Patched (game/vcs/overrides.toml): the
    four constants load from guest RAM 0x7FF00 / 04 / 08 / 0C (0.54, 0.09, 0.9, 0.18), which the ProcessWheel hooks
    fill before every call - unscaled at 30 fps and below, so the PS2's values bit for bit. **Guest RAM 0x7FF00..0x7FF0F
    is ours**: below the runtime's async callback stacks (0x80000..0x100000); a write watch on it through eight runs
    (run-overs and coasting at 30 / 60 / 144 fps) saw no game write.
  - **Audit** (`tools/framerate_audit.py --only coast,runover,bikebody,bikecoast,bikelean --steps 30,60,144`,
    2026-10-28; in brackets 144 fps with `VCS_PHYSICS_TIMESTEP=0`, the lean from the build before the patch):

    | | 1/30 | 1/60 | 1/144 |
    |---|---|---|---|
    | car coasting, m/s^2 | 3.44 | 3.45 | 3.45 (16.2) |
    | run-over: rise m / thrown up m/s / pitch rate | 0.13 / 1.37 / 0.042 | 0.10 / 0.97 / 0.043 | 0.09 / 0.70 / 0.041 (0.59 / 3.9 / 0.072) |
    | bike past a body: forward z at most / up z at the end | 0.53 / 1.0 | 0.39 / 1.0 | 0.31 / 1.0 (1.0 / 0.42: on its side) |
    | bike coasting, m/s^2 | 2.38 | 2.38 | 2.37 (11) |
    | bike lean per g of sideways acceleration | 0.56 | 0.60 | 0.64 (0.49) |

    The bumps are smaller at higher frame rates: a car's collision with a body or the road is sampled in finer steps
    (at 30 fps and 16 m/s a frame moves the car 0.55 m into it). The per-frame part is what made them grow.
  - **Still per frame** (read in ProcessWheel, not measured or fixed): a burst tyre's wobble - a random sideways amount
    added every frame when the wheel status (last argument) is 1.
- **Moving platforms: `1 / max(step, 1.0)`** (found 2026-10-31 from GitHub issue 10: Vic fell out of the ferris wheel
  at 60 fps, not at 30). Objects that the game places itself each frame get the move speed `(new position - old) * 1 /
  max(ms_fTimeStep, 1.0)`, so that a ped standing on them moves with them. On the PS2 the step is 1.67 and the 1.0 never
  matters; above 50 fps (step < 1) the speed came out step times too small. Patched (game/vcs/overrides.toml: the max.s
  becomes max(step, 0.001) through the two nops behind it, as the 1.0 register is also the division's numerator; steps
  >= 1 unchanged):
  - **FUN_00175620, the ferris wheel** (Funland; no reVC counterpart). Globals: 0x486020 the hub object (its matrix
    turns), 0x486024 the angular speed (0.05; * step * 0.02 a frame = 0.05 rad/s, ~1.15 m/s at the carriages), 0x486028
    the angle, 0x48602C the angle left to a stop, 0x486030 the state (0 starting: speed += step * 0.0002 up to 0.05; 1
    running; 2 stopping, when 0x486031 is set; 3 stopped), 0x48603C the radius (23.0), 0x486040 / 0x486044 the swing's
    spring and damping (0.1 / 0.95), 0x486034 / 0x486038 the swing's push while starting / stopping (0.001). The 12
    carriages (model 7069, created within 275 m of the camera by FUN_001763E0, removed past 300 m by FUN_00176680) are
    at 0x72D258; each is put at angle + i * 30 degrees in the plane x = 342.19 around (y -243.2, z 37.1), its move speed
    set (FUN_003562C0 at 0x1760A8) and its matrix tilted by its own swing (angle 0x72D228, speed 0x72D1F8: `w = (w + push
    - angle * 0.1) * 0.95; angle += w * step` - per frame, but the wheel runs steadily, the angles read 0 and the push
    only settles them at 0.01 rad; left as it is). Measured (rig, a ped dropped into the lowest carriage): at 1/30 he
    keeps his place in it (offset within 0.01 m for 15 s) and his move speed equals the carriage's (0.023 units = 1.15
    m/s); at 1/144 the carriage's speed read 0.0079 (0.40 m/s), he took that, slid 0.75 m/s towards its edge and fell
    out after 1.8 s. Found with the object pool (below), a write watch on a carriage's move speed +0x140 (writer FUN_003562C0,
    ra 0x1760B0) and the function's decompile.
  - **FUN_002469A0, the escalators** (CEscalator::Update as in reVC: N step objects moved along three segments on a
    16.384 s cycle of the game clock, `m_snTimeInMilliseconds & 0x3FFF`; created within 25 m of the camera). Same
    division at 0x246EB4 - riders slid against the steps the same way. Found by scanning the generated code for a
    max.s of the step against 1.0 followed by a div.s (`framerate_audit.py` covers the wheel only: `ferris`). The list:
    count 0x4CD2E8, records from `*(0x4CD2EC)`, 0x150 bytes each: path points +0x00 / +0x10 / +0x20 / +0x30, a matrix
    +0x40, active +0x90, reversed +0x94, step count +0x98 (31), segment ends +0x9C / +0xA0 (0.113 / 0.85), centre +0xB0,
    radius +0xC0, step objects from +0xC4. All 8 are in the mall interior around (406-422, 1052-1200, -198). After the
    patch a step's move speed at 1/144 is 1/30's (0, 0.0147, -0.0095 against 0, 0.0148, -0.0095).
  - **The follow cameras** (patched 2026-10-31, the same way; 12b): FUN_0029DAB0 (mode 4, on foot) and FUN_0029FAF0
    (mode 18 behind a vehicle, 22 behind a boat; CCam::Process's cases 3 / 0x11 / 0x15) turn beta toward where they
    want it by a per-frame amount (a share `min(speed x rate x step, 1)` of the gap, capped at rate x step), make that a
    speed with `/ max(step, 1.0)` (0x29EA50 / 0x2A0790), ease the heading speed +0x140 toward it (pow(k, step)) and add
    speed x step to beta. Above 50 fps the automatic turn ran step times too slow; the stick's term is added after the
    division and was right. Measured (`framerate_audit.py` carcam: the car's lag behind it after a left turn halving;
    footcam: the camera's turn rate while the player runs to its right): before 0.47 / 0.58 / 1.86 s and 96 / 82 / 35
    degrees a second at 1/30 / 60 / 144, after 0.50 / 0.48 / 0.48 s and 97 / 99 / 100. $f3 (the 1.0) is read again by
    the `min.s` two instructions on, so the 0.001 goes to $f5. On foot the controls follow the camera: after a turn the
    player lines up with it, so its own turn shows mainly while he runs sideways.
- **Physics read for the audit**: ped -> its vehicle at ped+0x450 (also +0x480 / +0x484; found by scanning the ped for
  the address of the car it was warped into); **CPhysical move speed (x, y, z) at +0x140** (units per 1/50 s, x 50 =
  m/s; z found at +0x148 mid-fall, y at +0x144 in a car driving north); written through a vector setter
  FUN_00356288 / FUN_003562c0 (write watch on +0x148: callers FUN_001f93b0, 00232de8, 001f25f0 (peds),
  00348128 / 00348538 / 00348628); FUN_00348098 = ApplyMoveSpeed (position += move speed * step). Terminal fall
  speed ~1.0 units (50 m/s) after 3.5 s. A new car (`CREATE_CAR`) faces north; its matrix can be poked (right
  +0x00, forward +0x10, up +0x20, position +0x30).
- **Frame counter readers** (`m_FrameCounter` 0x4CD124; the generated code has constant addresses folded, so a text
  search of generated/ee lists them - 52 functions; the time step 0x4CD110 has 93, the ms clock 0x4CD104 198):
  - work spread over frames, `(entity seed (+0x48 >> 54) + frame) & mask` or `frame & mask` with masks 1-0xFF:
    FUN_0011c958, 00138b68, 00140b28, 00186aa8, 001effb0, 0020db30, 00203240, 00238bb0, 00243730, 0025bba8,
    00267928, 00268048, 002c3e48, 0033d5a0, 00378dd8, 00421320 ... - run more often per second at higher rates;
    harmless where the work itself scales by the step;
  - blinking, `frame & 8 / 4 / 2` toggles: FUN_0031e3f8, 0031fce8, 00321500 (& 8), 003209d0, 00320f28 (& 4) -
    right after CHud::Draw (0x31c628), reVC's CHud flashing (a flashed item on `& 8`, health under 10 on `& 4`);
    001419d0, 0033dca0, 003f29d8 (& 2), 002af6e0 (& 8) elsewhere. They flashed twice as fast at 60 fps. **Fixed**
    for the 2D pass (radar, HUD, text: FUN_0021f348): above 30 fps it runs on a counter whose low 6 bits advance
    30 times a game second (from the game clock), put back as CTimer::Update starts (`vcs_framerate.cpp`
    twoDPass / timerUpdate) - the value stays within 63 of the real counter for anything else reading it then.
    The four outside the 2D pass are not covered yet;
  - timestamps (stored, compared with frame - 1: "seen last frame"): FUN_00383028 .. 003868c0, 003ac9c8 - harmless.
- **Mission scripts that count per frame**: a meter changed by a fixed amount each `WAIT 0` pass (PHI_A2's heat bar,
  eleven more missions) moved with the frame rate - section 11f.
- **Emulated time and the VBlank chain** (found when independent timing first ran at 5 fps): VBlank events are chained
  in EE cycles (+1/60 s each) and each is also held to its host-clock moment. An EE clock moved past the chain makes
  every following VBlank due at once by cycles, and the EE is held at each until its real-time moment - the game
  crawls. Topping the clock up must therefore aim at the chain's real-time position, not at elapsed host time.
- **60 fps, original timing** (`src/game/vcs_framerate.cpp`): the VBlank-counting callback adds 2 instead of 1, so the limiter lets a
  frame through after one VBlank. Measured outside the barracks (2026-10-23, i7-7700K): 31-37 fps - a gameplay
  frame costs ~27 ms on the game thread (game code 19-24 ms, render submission ~8 ms), more than 1/60 s; the
  emulated VBlank keeps real-time pace only while the game thread keeps up (events need both the EE cycle and the
  host deadline). A steady 60 needs ~40% less game-thread time per frame (milestone 7).
- **What the game watches of the GS / VIF1** (counted by the render worker's fences, `PS2X_RENDER_THREAD_LOG=1`,
  2026-10-23): in gameplay the GS privileged CSR / SIGLBLID ~5 reads a frame (300/s at 60 fps); GIF and VIF1
  registers only a few times at boot; never VU1 memory or the VIF1 / GIF FIFOs. The work it hands over is ~3 items a
  frame: the frame's VIF1 display list (one chain, FUN_00272268 above), one or two GIF chains (texture uploads), then
  the VBlank. It never enables the VU1 D / T stop bits (FBRST). So the GS and VU1 can run behind the game thread
  as long as the CSR reads see the finished work (runtime/ps2_render_worker.h).

## 5. Camera, projection and widescreen

Found in the widescreen spike (SETTINGS_MENU_WIDESCREEN.md), by forcing the preference and logging
the globals each frame:

| What | Where | Notes |
|---|---|---|
| FOV global | `0x487484` (float, degrees) | written by `FUN_002653e0` (a `CDraw::SetFOV`); 45 in the frontend, 120 in the flyover, 70 in game |
| Aspect global | `0x487488` (float) | written only by `FUN_002653f0` (`CDraw::CalculateAspectRatio`) |
| Aspect choice | `FUN_002653f0` | widescreen preference on: 16/9, or 5/3 when `[0x6f4d14]` is set; off: 4/3, or 1.25 when set. `[0x6f4d14]` is 0 in the frontend and 1 in game (not a PAL flag) |
| Widescreen preference | `*(int *)(*(0x48F3D0) + 0x30)` | getter `FUN_002af658` (`lw v0, 0x30(a0)`); the object at `[0x48F3D0]` (prefs / menu manager) is created by `FUN_00470fa8` |
| View window | `FUN_001ab9e0(camera a0, raster rect a1, tan f12, aspect f13)` | x = tan(FOV/2), y = x / aspect, raster 640x448 (null rect), then `FUN_0018f2d8` = RwCameraSetViewWindow. Called at `0x21cdd4` with the aspect global, at `0x21f9c8` with a fixed 16/9 (same camera `[0x51E490]`; probably the letterboxed cutscene view) |

- The game's own widescreen is **Vert-**: same horizontal view, vertical view cut, for a TV that
  stretches the anamorphic picture. The HUD does not change with it (the radar keeps its shape); the
  preference's 106 readers are mostly 3D-projected sprites (e.g. `0x1147e0`: a size without the 4/3
  factor).
- RenderWare derives the frustum (EE culling) and the VU matrices from the view window, so widening
  it (our Hor+, `vcs_display.cpp`) widens 3D, sector culling and both renderers at once.
- **Entity culling does not follow the view window** (found 2026-10-27, the user's "models at the screen edges are
  culled as if the screen were 4:3"). `CCamera::IsSphereVisible` = `FUN_003816F8(cam, centre, camera matrix, radius)`
  transforms the centre to camera space (x right, y forward, z up) and tests it against near / far (`0x4CD130` /
  `0x4CD12C`) and four camera-space plane normals in the camera object: right `cam+0xB60` = (cos h, -sin h, 0),
  left `+0xB70` = (-cos h, -sin h, 0), bottom `+0xB80` / top `+0xB90` = (0, -sin v, -+cos v); culled when
  dot > radius. They are built in `FUN_0037b2c8` (called from `FUN_00378dd8`, the camera update) from the FOV global
  alone: h = FOV x pi / 360 (`0x3C0EF921`), v = 0.7 x h (`0x3F333333`) - never from the aspect or the view window.
  Found by grepping the generated code for stores to `0xB60(` (one writer) and loads of the FOV global `0x487484`
  (13 readers; the others are a FOV < 60 branch in the sector draw `0x240ff4`, LOD / sprite scales 70 / FOV). With
  the view window widened the planes still cut at the 4:3 edge (tan h = 0.700 vs 0.934 at 16:9): buildings, peds and
  cars in the outer eighths of a 16:9 picture were not drawn, and the car / ped spawners (which also ask
  IsSphereVisible) treated those parts as off screen. Fix: `vcs_display.cpp` wraps FUN_003816F8 and, before it runs,
  widens side planes the game has rebuilt to the Hor+ edge (normal (a, r b) normalised); the vertical planes stay
  (Hor+ keeps the vertical view). No other reader of those planes (the writes are only in FUN_0037b2c8, the reads
  only in FUN_003816F8). Regression check: `tools/regress.py` step "widecull" (the planes' edge equals the view
  window's x at `0x742AC0`, which holds (tan(FOV / 2) x r, aspect x r, near 0.3, far 2000)).
- The sector test (6b) builds its clip planes in `FUN_002F7620` from that `0x742AC0` view window, so it was already
  wide: its side planes at 16:9 are (-+0.73095, -0.68243), the same 43.0 degrees as the widened entity planes.
- `FUN_0039fc48` (3D point to screen for sprites: coronas, light glows) only rejects by near / far; it sizes the
  sprite with the aspect global (`0x39fd2c`: 448 / z x 1.4286 / aspect), which Hor+ leaves at the game's 4:3 value.

## 6. Renderer

### 6a. Frame structure

- One VIF1 DMA chain per frame, kicked at `0x27245C` (D1_CHCR = 0x145, chain + TTE); TADR alternates
  between static root lists `0x480D20` / `0x480DE0`. The kick: the flip `FUN_003706c8` -> `FUN_002721f8` (the chain
  pointer at `0x510438`, cleared after) -> `FUN_002723E8(channel, tadr, chcr regs)`, the DMA library's start (clears
  the channel's D_STAT bit, writes TADR / QWC, then CHCR). There is no wait for VIF1 to finish before the next frame
  rewrites what the chain reads: `FUN_00272480(channel, timeout)` (the DMA sync) is called only from `FUN_003177e0`,
  `FUN_00317930`, `FUN_00317b50`. (2026-10-25, from the recompiled code's disassembly comments: the callers of
  `func_2723E8` / `func_272480`; Ghidra was not available.) Full frames are 7-14k tags (CALL / RET per
  model) - more than the runtime's original 4096-tag guard.
- Models are prebuilt packets in the model heap, entered with CALL tags and ended by RET; CALL tags
  are written by the RSL draw functions (6i; `FUN_003298D8`, named here before, builds texture uploads). VU1 microcode sits inside
  `.text` and is uploaded each frame by REF tags with `MPG` VIFcodes (`0x2AC130`, `0x2F13D0`,
  `0x368730`, ...).
- VIF1 payloads come from packet buffers (`0x500000-0x6FFFFF`) and the model heap
  (`0x1200000-0x17FFFFF`). DMA packet buffer `0x520F90`: two halves of 0xE6670 bytes;
  `FUN_00476770` returns the bytes used; draw code skips work when < 0x12C00 are left (peak ~400 KB).
- Z buffer: Z24 640x448 at ZBP 280, cleared each frame by 40 column strips (640x224 CT32 in total)
  drawn at FBP 280, whose own Z write goes to ZBP 350 (the second half).
- Display: PMODE 0x8027 = both circuits on the same buffer one line apart, blended (anti-flicker);
  DISPFB FBP 0, FBW 10 (640), DBY 2 / 1; 640x445 shown.
- **The flip (FUN_003706c8) reads GS CSR** at `0x370714` (`ld $v1, 0($a0)`, $a0 = 0x12001000) and keeps only the
  FIELD bit: `dsrl $v1, 13` then `andi $s4, $v1, 1` in the next branch's delay slot; $v1 is overwritten right after.
  It is the game's only CSR read per frame (2026-10-25, a game-thread profile with call stacks: the runtime's CSR
  fence - waiting for the render worker to finish all queued GS work, for SIGNAL / FINISH - was ~25% of the game
  thread uncapped). FIELD is set at VBlank by the EE scheduler, so that read skips the fence
  (`PS2Runtime::addCsrReadWithoutFence`, src/game/vcs_display.cpp; `VCS_CSR_FENCE=1` = as before).
- **Scratchpad**: the per-frame render state lives at `0x70002020+` (e.g. FUN_002b8ed0, the busiest draw function,
  stores ~10 words there per call through `$s0 = 0x70002020`). The recompiler resolves those addresses as constants;
  the runtime's Load / Store for them are inline since 2026-10-25 (they were out-of-line calls taking a mutex).

### 6b. World and culling

- World sectors: `FUN_0023E748(level, pass)` (3 passes per frame; takes a lock, scrolls water UVs at
  level+0x5D0/0x5D4) walks the sector slots at level+0x248.., and per entry calls `FUN_00240ED8`,
  which culls with `FUN_002F7528` = unit box transformed by the sector matrix, tested by a VU0
  microprogram (CMSAR0 0x19F, VI15 = 0xDC) via `FUN_0030C8D4`; 0 = culled, 1 = inside, 2 = partly.
  Visible sectors go to `FUN_002409C8`.
- Entities: `CCamera::IsSphereVisible` = `FUN_003816F8` (VU0 macro transform of the centre by the
  camera matrix, near / far at `0x4CD130` / `0x4CD12C`, planes at cam+0xB60).
- Level object `0x750340` (pointer at `0x48F4F8`). The world is a hex grid of cells (125 x 108.25
  units, odd rows offset by half a cell; origin = ((x + 0.5 - (y & 1) / 2) * 125 - 2400,
  (y + 0.5) * 108.25 - 2000), `FUN_00241F00`). Cell slots at `0x75036C` (+0xA0 each): grid x / y at
  +8 / +0xC, loaded flag +0x18, draw lists via +0x20. `level+0x248` = current cell, `+0x250` = next;
  `FUN_0023C820` (per frame) follows TheCamera (`0x6F44D0`, position at +0x30) and `FUN_0023D740`
  swaps them. Each cell streams ~0.3-0.5 MB of MAINLA.IMG in 6 pieces into `0xCB7A80..0x112A660`.
  Fort Baxter is around cells (5..7, 16..18).
- In-world draws are mostly MODULATE + alpha test GREATER 8, fogged, vertex colours from V4-5
  unpacks.
- **Levels**: the world is three levels, each a `WRLD` image (`.LVZ`) plus an archive (`.IMG`): 1 = BEACH (east
  island), 2 = MAINLA (mainland: Little Havana, Little Haiti, Downtown, the airport, Fort Baxter), 3 = MALL. The
  executable names them with the developers' paths (`cdrom0:/RUNDATA/BEACH.LVZ`... table at `0x4ADAE0`, resolved
  through the disc index). Only one level's WRLD is in memory. `FUN_0023baf0(level, newLevel, ...)` switches;
  `FUN_0023c188(level, camera position)` (per frame) calls it when the wanted level - global `0x4CD118`, set from
  the player's zone - differs from the loaded one (`level + 0x18`). Teleporting across islands switches levels by
  itself within half a second (rig `level_test`, 2026-10-23).
- The cell the streaming focuses on is computed from the camera position in `FUN_0023c188` with float maths and
  `cvt.w.s` (float -> int). The EE FPU's `cvt.w.s` truncates; the runtime rounded to nearest, so at places the
  game picked a neighbouring hex cell 100+ units away (camera (-675, 1172) -> cell (14, 30), centre
  (-587.5, 1301.6), instead of (13/14, 29)) and streamed that cell's geometry and collision: nothing drawn or solid
  under the player, who fell through the world (the user's falling-through report; `VCS_TRACE_CELL=1` shows the
  focus).

### 6c. VU microcode

- `.vutext` holds four small VIF `MPG` packets (DVP overlays: `0x48055C` -> VU 0x000, 245 pairs;
  `0x4810EC` -> 0x400, 4; `0x48146C` -> 0x000, 24; `0x48176C` -> 0x200, 20). They are **VU0**
  helpers: program 0 (0x000..0x7A8; identical in VU0 code memory during play) holds **0x020 = segment vs
  triangle** and **0x3A0 = sphere vs triangle** (with the nearest-point-on-segment subroutine at 0x228, called with
  `BAL vi15`); they were ~22% of the EE thread interpreted (2026-10-23, `PS2X_TRACE_VU0`) and are native now
  (`src/game/vcs_vu0_native.cpp`, checked bit-exact with `PS2X_VU0_VERIFY=1` over 4.7M runs while driving). Inputs:
  vf17 (12.4 / 7-bit fixed point: normal or segment), vf14..16 the triangle (7-bit fixed point), vf12 / vf13 the
  sphere or segment; out vi1 = hit, vf1 contact point, vf2 / vf3 direction and depth. Program 0 has E-bit exits at 0x000 / 0x010, converts fixed point with `ITOF0` /
  `MULi 1/128`, builds edge vectors and runs `CLIP` / `FMAND` / `FCAND` / `DIV` - most likely
  collision (line / sphere vs triangle). Found by disassembling with hctp-recomp's `vu1dis.py`.
- **The other VU0 programs while driving** (2026-10-24, `PS2X_TRACE_VU0=1` per 5 s; code dumped with
  `PS2X_VU0_DUMP=0c10`, disassembled with `tools/vudis.py`; all native now, `PS2X_VU0_VERIFY=1` 0 differ): 0x0CF8
  sector box vs three plane sets (~240k calls), 0x0EB8 three 4x4 matrix products (~220k), 0x03A0 / 0x0020 above,
  **0x0C10 sphere vs three plane sets** (19k calls, 190 ms interpreted - 10 us a call: vf1 = centre + radius in w,
  matrices at vi15+24, vf31 = (1,1,1,1); vi1 / vi2 = per set the MAC sign flags of radius - distance /
  distance + radius, read by FMAND 4 pairs after the FMAC), **0x0068** = the segment-vs-triangle program entered after
  its fixed-point conversion (5.8k, 60 ms), **0x0E98** = the 0x0EB8 loop on the matrices at vi15 into vi15+12 (2.9k,
  25 ms), **0x0E30** = vf1..vf12 stored to vi15+0..11, the last in the E bit's delay slot (2.9k, 8 ms). Together the
  four were ~5% of the game thread while driving. 0x0CF8 / 0x0C10 are called through `FUN_0030C8D4` / `FUN_0030C8A8`,
  the matrix programs through `FUN_0030C940` / `FUN_0030C900` (e.g. `FUN_002F7620`, which builds the camera's plane
  matrices from `0x742AC0` into `0x520AD0` / `0x5208A0` / `0x520860`).
- The **VU1** rendering microcode is not a static MPG packet: it is uploaded from `.text` each frame
  (6a). `game/vcs/vu1_uploads.txt` lists the uploads seen; `tools/vu1_recomp.py` compiles them.
- Setup programs that never XGKICK (normal): 0x0148 (clear state), 0x0418 (lighting dispatch via
  JALR table at VU mem 34..37), 0x0478 (matrices), 0x2B50 (matrix copy).

### 6d. VU1 pipeline (found with PS2X_HLE_SURVEY, NATIVE_RENDERER.md stage 4)

Program **0x0030** produces ~95% of all GS vertices (3.0-3.5M/s) and ~88% of VU1 time. 0x02c0
(V4-32 x4 per call: sprites / particles / text) ~3%. Disassemble samples with
`python tools/vudis.py <vucode.bin> --start 0x30 --to-ebit` (`PS2X_HLE_SURVEY_DUMP=2
PS2X_HLE_SURVEY_DUMP_EVERY=40000`).

**0x0030 is a dispatcher**: `JR` to instruction `[38].x` inside itself, then a chain of `JALR vi15`
calls whose targets / `vi01` parameters come from VU data qwords 38-41 (set per batch by the setup
programs). Stage tables (instruction indices):

| entry | stages (vi01) | used by |
|---|---|---|
| 0x19 | 0x0da, 0x4e0 (0x9c), 0x2b6 | prelit world: V3-16 pos @1, V2-8 uv @0x48, V4-5 colour @0x8f (+ V4-32 matrix @0x2c x3 in some) |
| 0x19 + 0x4ff | 0x0da, 0x4ff (0x9c), 0x2b6 | rigid lit models (vehicles, props): int8 normals V3-8 at `[47].y` |
| 0x16 | 0x5a5 (0xa8), 0x0da, 0x4ff (0x9c), 0x2b6 | lit, skinned models (peds): V3-16 pos, V2-8 uv @0x55, V3-8 normal @0x2b, V4-32 weights @0x7f |
| 0x13 | 0x0da, 0x4ff (0x9c), 0x2b6, 0x5b1 (0xa8), 0x2b2 | rigid lit models with an **environment map** (vehicles): native since 2026-10-23 |

Code at 0x2700.. (instr 0x4e0..) is swapped by per-pass MPG uploads, so programs are keyed by the
code at the start PC *and* 0x2700-0x2fff.

VU data layout of the world pipeline (qwords; "top" = XTOP double buffer):
- 28-31: combined world -> GS window matrix (rows; includes the viewport scale / 2048 offset).
  24-27: the clip matrix.
- 42: position scale (1/32768 for int16), 43: position offset (folded into the matrix).
- 44-46: 3x3 matrix for the V2-8 texture coordinates, applied with I = 1/255.
- 47: input offsets: .x positions (1), .y normals, .z uv (0x48), .w colours (0x8f).
- 48: .z = mask applied to TOP, .w = output buffer offset (0xd6).
- 49 / 50: GIF tags for the kick (PACKED, NREG 3: ST, RGBAQ, XYZF2; PRIM strip with IIP TME FGE ABE);
  NLOOP = vertex count from `[pos-1].w`. 51: default RGBA (255,255,255,128) / material colour.
- 52: .x transform routine (0x0e9 normal / 0x0da first call), .y flags (bit 1 fog, bit 0 extra pass
  at 0x1440), .z kick variant (698 / 711). 53: fog (.z bias 255.6, .w scale -0.128 per unit of w).
- 0x9c-0x9e (vi01 of stage 0x4e0): colour scale (time-of-day tint), offset, clamp (255).

Routines (byte addresses): 0x0748 normal path (`BAL 0x0c38` clip test; all inside -> 0x06f0 fast
path; some outside -> clipper 0x11f8 + 0x13d8); 0x07f0 transform (`p = M * (itof(pos) * scale +
offset)`, `Q = 1/p.w`, XYZ = FTOI4(p.xyz * Q), STQ = (uvMatrix * itof(uv) / 255) * Q); 0x1048 fog
(F = clamp(53.z + 53.w * w, 0, 255)); 0x2700 prelit colour (RGBA = MINI(itof(c) * [0x9c] + [0x9d],
[0x9e])); 0x15b0 GIF tag + `XGKICK`. Lit models: 0x2d28 (stage 0x5a5) 4-bone skinning (weights'
low bits are bone indices, bones 4 qwords each from 0xa9, result written back over the position /
normal slots); 0x27f8 (stage 0x4ff) 4 directional lights + ambient, scaled by `[51]` * 1/510, RGB
only. All reproduced bit-exact in `src/game/vcs_hle_world.cpp` (`PS2X_HLE_VERIFY=1`), except the
STQ.w lane and its fog in the lit pass, which the runtime's VU1 core leaves inconsistent.

**Environment map (stage table 0x13, 2026-10-23).** The rigid lit pass (0x0da, 0x4ff with 0x9c, kick 0x2b6 = tag 49)
followed by **0x5b1** (vi01 = 41.y = 0xa8) and kick **0x2b2** (tag 50). 0x5b1 first XGKICKs the GIF packet at vi01
(seen: one A+D write of TEXFLUSH), then for every vertex (at least 6): ST = (rows vi01+5..7, x / y lanes times
I = 1/256) . normal + 0.5, times the vertex's Q from the output, written over the output's S and T. Kick 0x2b2 sends
the same output (colours, XYZF2, Q unchanged) again with tag 50: PRIM 0x25c - strip, Gouraud, textured, blended,
no fog, **context 2** (the environment texture set by the game's own GS packets). The kick tag's x lane is the
packet header's .w via ILW + MFIR - sign-extended (0xffff80nn). Reproduced (`envKicks` in vcs_hle_world.cpp) and
checked against the VU's three kicks: 27,243 packets over the reference scenes, all exact; native drawing: pass 1
as a rigid lit model on the GPU, the TEXFLUSH and the second pass as the VU's kicks (PATH1). Clipped table-0x13
packets stay on the VU1.

**Code images.** The code below 0x2700 is not fixed either: VCS uploads different variants of the transform
(0x07f0) and clip test (0x0c38) per pass - every reproduction is tied to a hash of the exact code it was read from.
Seen so far:
- the prelit world image (integer positions folded with 42 / 43, integer UVs through the texture matrix 44-46 / 255,
  prelit colour stage at 0x2700);
- the **float images**: positions are floats, the matrices 24-27 / 28-31 are used as stored (no fold), RGBA is
  qword 51 from the transform, the fog stage FTOI4s into XYZF.w. Two flavours: the one 0x02c0 runs with (UVs as
  floats, used as they are) and the "variant C" prelit one (UVs ITOF0'd through the texture matrix, a colour stage
  at 0x2700 that copies the colour qwords - layout V4-32 @001 / @049 / @091 x72; a second copy differs only in
  padding after a JR). Variant C's transform epilogue leaves the *next* vertex's w in STQ.w for vertices N-4..N-2
  (N = max(count, 7) processed): a MOVE.w waits for the MULw right before it - and the fog reads w there.
- Clip tests run over at least 5 / 6 vertices, so stale data past the count reaches CLIP: the VU treats exponent-255
  bit patterns as huge finite numbers (no NaN / Inf), so such a vertex can flag the packet for the clipper -
  reproduced with the VU's operand / result clamping (vcs_vu_math.h, from the runtime's vu1fast helpers).

**0x02c0 - instanced re-draws** (setup slot 0xc8, entry 0x1c): copies the packet's object matrix (4 qwords at TOP)
to 0-3, 0x04d8 builds clip (view 16-19 x object) and window (viewport 12-15 x clip) matrices into 24-27 / 28-31,
0xc8 the camera in object space (33 = R^T (32 - T)), then transform 0x0da + kick only, over a mesh resident at
absolute addresses (48.z mask 0). **Entry 0x0d** draws a resident mesh twice: setup slot 35 = 0x4e0 normalises the
object's rotation rows (ERLENG) into a 3x2 environment matrix (P+5..7, P = 0x9c); 0x4ff / 0x509 write an A+D TEX0_1
/ TEX0_2 qword at P+1 from P+2 / P+3 and kick the packet at P; pass 1 = the float transform + tag 49; 0x546 sets
RGBA = Q.xyz (Q = 0xa4) with alpha = FTOI0(clamp(Q+1.x * w + Q+1.y)); 0x513 kicks P again and writes the
environment ST ((P+5..7 x / y * 0.5) . normal + 0.5) * Q; pass 2 = tag 50 (context 2). A shine over a texture.

**Shine through the clipper / culled (2026-10-24).** The kick routines load their tag into vf01 and jump through
52.z: 0x15b0 (stage 0x2b6) loads tag 49, **0x1590 (stage 0x2b2) tag 50**; 52.z is 698 (0x15d0, plain kick) or 711
(0x1638, the per-triangle clipper, which builds its fan tags from vf01 - 0x1650 / 0x1660). So a shine object that
crosses the guard band is clipped twice: pass 1 with tag 49, then 0x509 / 0x546 / 0x513 run as usual over the
clip test's output (ST undivided with w in .w, the clip position + fog) and pass 2 is the same clipper with tag 50.
A culled packet (all vertices outside one plane) leaves the stage table at once: 0x0748 -> `B 0x0178` = XGKICK of
qword 58 (the empty tag) and the program's end - only the TEX0_1 kick of 0x4ff was sent. Both reproduced
(`shineKicks`, `vcsclip::clipFromOutput(..., tagQword)`); PHI_A1 in verify mode: 421,694 0x02c0 packets exact,
among them 9,629 clipped and 26,518 culled shines.

**Setup programs** (VU1 starts other than 0x0030 / 0x02c0, no primitives; `PS2X_TRACE_VU1=1`, code dumps in
`build/analysis/vu1setup`; one flyover sample each, so other code images may differ):
- 0x01e0 (and 0x02c8, a copy): object matrix at TOP -> 0-3, 0x04d8 (vi01..04 = 0, 16, 12, 24: clip = view 16-19 x
  object into 24-27, window = viewport 12-15 x clip into 28-31), then the slot routines in qwords 34-37 (routine in
  .x, vi01 = .y: 0xc8 camera into object space, 0x3a nothing, 0x4e0 env matrix), then `B 0x0178` (kick qword 58, end).
- 0x0250: the slot part of 0x01e0 alone, then the kick. 0x03a8: the slots from the .w lanes, then the kick.
- 0x0418: the .w slots, then falls into 0x0478 and ends without a kick. 0x0478: 0x04d8 (4, 8, 12, 16), then
  0x04d8 (0, 16, 12, 24).
- 0x0000 (same image): 42 / 43 (position scale / offset) from TOP, then the end kick. 0x0148: qword 58 = the empty
  tag (0x8000, 0, 0, 0). 0x2b50 (lit image): a block copy - the header at TOP to header.z, then 4 x header.x qwords
  after it (the bone matrices), four loads then four stores per step.
- Slot routines of the lit image (code at 0x2700+, so per image): **0x4e0** (byte 0x2700, vi01 = P = 0x9c) the light
  directions P+0..2 into object space: rows 0-2 normalised (ERLENG, MUL.xyz), P+9+i = L0 r.x + L1 r.y + L2 r.z (all
  lanes); **0x57e** (byte 0x2bf0, vi01 = A = 0xa8) the env matrix: A+5+i .xy = rows A+2..4 . normalised row i, stored
  as whole qwords - A+7.z is the last ERLENG, the other z / w lanes are stale registers (the env pass reads x / y).
- **The Z-clear image** (a second low image, different code before 0x0178; about 2 runs a frame each, reproduced
  2026-10-24 in `zclearProgram`): **0x0200** kicks two 4-qword GIF packets at qwords 0 and 4 in turn, the strips' x
  edges stepped by I = 32 (FTOI4 into the x lanes of qwords 2 / 3 of the other buffer), [0].w pairs of kicks (10:
  20 strips over 640) - the Z clear into FBP 280 that the renderer turns into depth fills; **0x0000** the same with
  6-qword packets at 0 / 6, x lanes of 2-5 stepped by the floats in 4 / 5 (read once; the floats carry 2^23 so the
  sum's low bits are the coordinate) - an SQ paired with an ADD of the same register stores the value from before the
  ADD; **0x0400** XGKICKs the packet at qword 128. Their kicks go to the GS front end (depth fills, register packets).
- **Slot routines are found by their code**, not their number: the light and env routines are position independent
  and 0xf8 bytes long, and the lit images put them at 0x2700 or 0x2bf0 - in one image slot 0x4e0 is the env routine
  (that 0x0250 fell back until slots were named by the hash at their address).
- Table-0x13 packets through the clipper (reproduced 2026-10-24, `envClipKicks`): the clip test / fog write the output
  buffer as for the prelit world (ST undivided + clip w, qword 51, clip position + fog), the lighting stage 0x4ff
  writes RGBA over it, kick 0x2b6 = clipper with tag 49; 0x5b1 kicks the packet at 41.y and writes the env ST (lane z
  of the output as Q) over x / y; kick 0x2b2 = clipper with tag 50. Reference scenes in verify: 216 exact.
With these, **no VU1 program runs in a normal frame** (reference scenes, tour subset: `vu1_programs` 0).
About 330 runs a frame in the flyover and cutscenes before (`PS2X_TRACE_VU1=1`: 0x01e0 30k, 0x03a8 12.6k, 0x0478
9.3k, 0x0250 5.3k, 0x0418 1.5k per 5 s). **Reproduced (2026-10-24)**, each routine tied to a hash of its code
(`CodeCheck` in vcs_hle_world.cpp), checked with `PS2X_HLE_VERIFY=1` against the whole VU memory after the VU's run
(stale lanes excepted): reference scenes 434,692, PHI_A1 217,489, a city spot 80,557 - all exact. The matrix and
camera routines need the VU's float edge cases (0x0478 / 0x0418 reach denormal or huge inputs: `vuMul` / `vuMadd` /
`vuAdd` / `vuSub`). Left on the VU1: 3-4 programs a frame (the second low image). `PS2X_HLE_SETUP=0` = VU1.

**A second world image** (2026-10-26, found by the mission smoke test: MAR_A1 was the only mission with GS-path draws,
0.6 a frame, all from 0x0030 on the VU1). `PS2X_HLE_STATS=1` showed its packets passed over with `code 0` - the
handlers' hash of the 0x0030 image (bytes 0x30-0x118, 0x6d0-0x7e0, 0x7f0-0xc20, 0xc38-0x1030, 0x1048-0x11f8,
0x1360-0x13d8, 0x15b0-0x1638, 0x2700-0x28a8) did not match. `PS2X_HLE_DUMP_CODE=1` (writes the VU1 code memory the first
times the check passes / fails) showed MAR_A1 loading other code at bytes 0x7f8-0xbcf, 0xc58-0xfe7, 0x1460-0x156f (and
0x28b0-0x2f6f, outside the hash); the dispatcher and the routines its packets call (stage table entry 0x19: 0x6d0,
0x748, 0x15b0, the light slot at 0x2700) are unchanged. Hash 0xdffe680d06eb60d0, accepted after `PS2X_HLE_VERIFY=1` over
the mission: 1.55M fast-path packets, 133k C++-clipped, 264k setup programs, all exact. The clipper's own check
(0x13d8-0x15b0) fails under this image, so clipped packets would still go to the VU1 there; none did in the run.

**0x02c0 code images.** Three images with the shine / re-draw stage routines are seen (PS2X_HLE_DUMP_IMAGES=1 writes
the first shine packet of each: build/analysis/shine): hash (0x30..0x2b20) 1a8edffd (the one reproduced first),
da70874b (differs only at 0x1030 - dead code after a JR's delay slot) and 97eecb5d (also in 0x0a88-0x0c10 and
0x0ef0-0x1020, alternative transform / clip-test routines the stage table never calls: the shine and re-draw enter
through 52.x = 0xde / 0xe9 only). The second and third were seen in the city (e.g. (-675, -75)) and fell back to the
VU1 until accepted; verify there: 85,413 re-draws exact.

All of these are reproduced in `vcs_hle_world.cpp` (floatKicks / shineKicks) with the VU memory they write, and
checked against the VU's kicks and memory (174,110 packets over the reference scenes, MAR_A1 / GON_C3 / a drive
~50k / 51k / 120k - all exact). With them native, **nothing of the 3D runs on the VU1** in the reference scenes
except clipped environment-pass packets (63 in two minutes); clipped 0x02c0 / variant-C packets go through the C++
clipper read back from VU memory (`vcsclip::clipFromOutput`).

### 6e. GS usage (VCS_GS_STATS, title / intro)

- ~95% triangle strips; also triangles, fans, sprites, points. CT32 colour (some CT16 for effects);
  Z24 (some Z32), test GEQUAL.
- Blend: 84% `(Cs-Cd)*As+Cd`; also `(Cs-Cd)*FIX+Cd`, `Cs*FIX+Cd` (FIX up to 255), `Cs*FIX`,
  `Cd-Cs*FIX`. Alpha test GREATER (+KEEP) and GEQUAL with AFAIL=FB_ONLY; no DATE.
- Textures: 83% PSMT4 + CT32 CLUT (world), CT32, T8, CT16; modulate / decal; repeat / clamp /
  region clamp; bilinear. Fog on most world geometry. CT32 / CT16 textures inside render targets
  = post effects. COLCLAMP on, no dithering, no FBA, no PABE.

### 6f. 2D: HUD, text, radar, menus, effects

- **The render-target reads' packets** (2026-10-26, `PS2X_HW_FRONT_DRAW_LOG`): the reflection map's copies into FBP 420
  (0x1A4) and FBP 452 (0x1C4) and the self-copy into FBP 140 (0x8C) sample target 140 (TBP 0x1180) through PATH2 A+D
  packets: one tag (NLOOP 0x36 or 0x10, A+D) of UV / RGBAQ / XYZ2 writes - a triangle strip whose PRIM (0x31C:
  TRISTRIP, IIP, TME, FST, context 2) came in an earlier state packet - or, for the sky's copy, XYOFFSET_2 (0x19),
  TEST_2 (0x48), SCISSOR_2 (0x41), then PRIM, then the strip. The flyover also draws one Im2D packet a frame with
  PRIM LINESTRIP (two vertices).

Found from frame dumps (`PS2X_HW_DUMP`, draw lists with depth) during the widescreen work:
- RenderWare keeps **ZTE on for 2D** (ZTE=0 is prohibited on the GS) and uses ZTST ALWAYS or GEQUAL.
- The HUD, text (font texture 256x256), radar and menus (RwIm2D) are all drawn at the **largest Z**
  (0xFFFFFF; 0.99999994 after scaling by 2^-24, never exactly 1.0).
- 3D-projected sprites - clouds (64x64 textures at z ~0.0006), coronas, the mission marker glow -
  take the same RwIm2D path but at their real depth.
- The sky is a full-width gradient at z = 1 (n=18 strip, x -0..852); a horizon band full width too.
- The colour filter / trails effect is drawn as **strips that sample the frame** itself (textures
  inside render targets, `texIsRt`), plus an alpha-only quad (FBMSK 0xFFFFFF) over x 80..560 at
  z = 0.
- The radar is drawn straight onto the screen (corrected 2026-10-24 from a frame dump at Fort Baxter: there is no
  off-screen radar target - the 128x128 at FBP 420 is the reflection map, 6g): the round outline is a fan at far Z
  (GEQUAL, Z write on) and the map tiles (128x128 T4 textures, far Z, GEQUAL) are clipped by the **scissor** to the
  radar's rectangle (37,336 - 120,412 at 4:3). Menu labels are scissored too.
- Pause menus have no depth-tested geometry below the far plane (that is how "3D frame" vs "2D
  frame" is told apart for the widescreen presentation).

**At the RenderWare level (2026-10-23, `src/game/vcs_hud.cpp`).** Found from the HUD's format strings (`$%08d` at
0x4b73a0, `%02d:%02d` at 0x4b73a8, used at 0x31ec08 in the money / clock routine FUN_0031eb18) and a trace of the
calls (`VCS_TRACE_IM2D=<n>`, `VCS_TRACE_IM2D_MARKERS=5`):
- **Im2D**: FUN_002fbe08(prim, vertices, count) = RwIm2DRenderPrimitive, FUN_002fbe68(prim, vertices, count, indices,
  indexCount) = the indexed one. **RwRenderStateSet** = FUN_00175198(state, value) (a switch over states 1..12; 6 =
  ZTESTENABLE). Font setters around 0x3f4000-0x3f4050 write the font state at 0x711f30.
- **Vertex** (16 bytes; the PSP-style packed form): u, v as u16 fractions | RGBA bytes | x, y as s16 pixels in
  640x448 | z (u32; the HUD's is 0xfffffe).
- **The 2D pass** (FUN_0021f348, after the 3D): sky gradient (0x3e0c6c) and horizon band (0x3e0d4c), the radar map into
  its off-screen target (indexed, 0x39fec4), clouds (0x3a1260), then **CHud::Draw = FUN_0031c628**: text glyphs
  (0x3dffc0, quads), the weapon icon (0x3a07a8: a sprite routine, z 0x4dcc3, depth test off), ammo digits (0x3dfe24),
  the radar on screen (0x3e0018, 0x3e0094, 0x3dff64); then FUN_001676c8, FUN_00396670, FUN_003aee80, FUN_002a9708,
  FUN_0025cd98 (messages / text when shown); the frame's display list goes out after (FUN_002723e8).
- The radar map is masked to its circle by **depth**: its HUD draws are depth-tested and their z matters.

**Weapon icon and ammo count** (2026-10-29, FUN_0031f478, called from CHud::Draw; decompiled, Im2D vertices with
`VCS_TRACE_IM2D`). The layout is the PSP's, in 480x272 units scaled by 4/3 x 1.647: the icon is a 72x72 quad
(FUN_003a03a8: centre in f12/f13, half sizes in f15/f16) over the rect 416..470 x 16..60 (465 with FUN_002af658), at
x 554..626. The ammo is text in rects: a single number centred in 411..462; "total-clip" as the total right-aligned in
414..438 (justify 0x14), the separator centred in 438..444 (0x12; "." with x - 4 for weapon types 0x22 / 0x23) and the
clip left-aligned from 444 (0x11). The icon texture's dark box is not centred in its quad: it covers about u 0.06..0.70
(centre ~436.5 PSP units, where the single number is centred), so the separator sits at ~63% of the box's width and a
short total ("95-29") looks right of centre. **The PS2 original does the same** (a PS2 capture on Grand Theft Wiki's
HUD page, "9993-6": separator at ~65%); not a port bug.

The **16:9 weapon-icon bug** (the icon at the right edge, away from its ammo count): the renderer's widescreen placement
treats far-plane 2D as HUD, and the icon is not at the far plane. Fixed at the source: from CHud::Draw to the frame's
end, Im2D primitives drawn with the depth test off get the HUD's z (`VCS_HUD_RW=0` = off).

**From Im2D to the screen (2026-10-23, F12 draw dumps at 16:9, `PS2X_HW_DUMP`; `VCS_TRACE_HUDKEY=<n>`).**
- The GS sees an Im2D vertex at (x - 0.5, y - 0.5) (the 2D code's pixel-centre bias) for most HUD draws, within 1/16 of
  (x, y) for others (the zone name, fades): a vertex's whole-pixel position finds it again with either rounding.
- The game draws the frame into the buffer at FBP 140 and copies it into the displayed one (FBP 0, DISPFB DBY = 2)
  with a plain full-screen copy in 40 strips (decal, no blending) at the start of the **next** frame. So what is shown
  is the previous frame's FBP 140, HUD included.
- Just before the HUD, a full-screen draw writes Z = 0 everywhere (FBMSK 0xffffffff: Z only). HUD draws use ZTST
  ALWAYS (most) or GEQUAL with Z writes at z = 1.0 (help text, zone name): later depth-tested HUD draws do not overlap
  earlier ones. Blending is (Cs - Cd) * As + Cd or additive; no draw seen uses the destination alpha.
- (Corrected 2026-10-24: the 128x128 drawn into FBP 420 is the reflection map, not the radar's map - the radar's tiles
  are drawn onto the screen directly.)
- Indoors the radar is the ring alone. Help text hides the rest of the HUD while it is up; on leaving the barracks
  the radar flashes for a while (a tutorial).
- **Radar on screen** (dump outside the barracks door, 16:9): first an untextured, colourless draw (blend
  (0 - 0) * As + Cd, Z write) of 24 triangles fills the radar box's corners outside the circle with Z 0xFFFFFE; then
  the map as four 128x128 tiles (large quads around the box) at Z 0xFFFFFD with ZTST GEQUAL, scissored to the box:
  they fail in the corners. Then the ring (64x64), the north / blip icons (16x16), all with bilinear filtering.
- **Menus** (2026-10-23; `VCS_TRACE_IM2D_STACK=1` lists the return addresses on the guest stack at each 2D call):
  FUN_0021f7d8 is the frontend's draw, called every frame; it returns early unless a menu is up, else calls
  FUN_00471400, then **FUN_003b53f8 (the menu / pause-menu draw)** (through 0x3b568c / 0x3b56d8 and 0x3cdfbc to the
  page drawing), then FUN_003e0b10 (a full-screen overlay). In the pause menu no 3D is drawn and CHud::Draw is not
  called: the frame is the copy of FBP 140 into FBP 0, then 2D only into FBP 140 (backdrop, map, icons, text, the
  overlay). The map's player arrow blinks.
- **HUD textures**: the clock and money digits (orange and green, with outlines) and the wanted star are one
  128x64 atlas with 16-texel cells side by side; the ammo count and text use a 256x256 font atlas. All HUD textures
  are bilinear-filtered (TEX1 MMAG / MMIN linear) and drawn smaller than their texels (a clock digit: 15 x 21.5
  texels into about 5 x 9 pixels).

Milestone 3 step 2 builds on this (docs/NATIVE_RENDERER.md "HUD layer"): `vcs_hud.cpp` collects the HUD context's Im2D
primitives each frame, groups them into elements (boxes closer than 12 px across / 3 px down: a line of text, the
radar and its blips, the clock / weapon / money cluster) and gives each an anchor from where it sits on the 4:3
screen: left edge when its middle is left of 280 and it starts before 160, right edge when its middle is right of 360
and it ends after 480, the middle otherwise; full-width draws (fades, letterbox bars) stretch. At the frame kick the
whole-pixel positions of its vertices go to the renderer, which finds each GS primitive's element by them. The radar
and centred text are not grouped by position but known by who draws them (below).

**Radar and text** (2026-11-01, user report: with the HUD at the screen edges, subtitles / stunt texts at the bottom
jumped between left, middle and right, and the radar sometimes left its corner; `VCS_TRACE_IM2D_STACK=1` and
`tools/eedecomp.py`):
- **The radar** is `FUN_00321500` (called from CHud::Draw): the map through `FUN_00110eb0` (tiles from `FUN_001178f8`,
  Im2D returns 0x117620 / 0x1178a8), then the ring and blips (0x3e0018, 0x3e0094, 0x3dff64, 0x3e0ba0). The map tiles
  are **huge quads** scissored to the radar (x -210..410, y 141..716 at 4:3 for a radar at 37..122 x 336..415). That was
  the bug: text near the bottom left merged with the radar's element, and the pair's box - changing as the map turns
  and scrolls and with the text's length - flipped between "left edge" and "middle". CHud::Draw's next call after the
  radar is `FUN_00113d50`, `FUN_00321758` or `FUN_002dfe10` (whichever path it takes), so "the radar is drawing" =
  from `FUN_00321500`'s entry to one of theirs.
- **Text**: the font setters (`FUN_003f4000` style = 0x48a1dc, `FUN_003f4020` colour = 0x711f30, `FUN_003f4050`
  rect = 0x711f38, `FUN_003f41e0` justify = 0x48a1ec, `FUN_003f3c30` reset) and `FUN_003f3d10` print(string, x, y) ->
  `FUN_003f3d88`, which copies the state into the style's font object (`*(0x48a1d8 + style * 4)`) and calls
  **`FUN_00261be8` = render(font, string, x, y, colour, rect, ?, shadow)** twice (drop shadow with a copy of the rect,
  then the text with the global one). The glyphs go out synchronously, one Im2D batch per line from `FUN_003dfdd8`
  (return address **0x3dfe24**, nothing else draws through it). **Justify** = font + 0x28: bit 1 left, 2 centred, 4
  right in the rect, 8 / 0x10 / 0x20 top / middle / bottom; none = at (x, y). The **rect** is (left, bottom, right,
  top) floats in the PSP's 480-wide units (x 640 / 480 on screen) and the glyphs are scissored to it. Seen: subtitles
  and mission text 0xa in 24..456; the stunt results ("Distance: ... Height: ...", "INSANE STUNT BONUS") 0xa in
  **116..456** - centred right of the middle (x 381 of 640), clear of the 4:3 radar; help text 0x9 in 28..248; zone /
  vehicle names 0x14 in 0..456; the ammo count 0x14 / 0x12 / 0x11 in 414..438 / 438..444 / 444..462.
- So `vcs_hud.cpp` tags the radar's primitives (left edge, never grouped) and lines centred in a rect at least 240
  units wide (the middle, never grouped). At the screen edges a centred line's rect is moved to the middle before the
  game lays it out (the stunt results: the radar they avoided is at the edge now), the rect rather than the glyphs
  because of the scissor. `VCS_TRACE_HUDANCHOR=<frames>` logs the elements and the text lines; `tools/hud_check.py`
  checks it.

**What reaches the GS as PATH2** (2026-10-24, `PS2X_TRACE_PATH2_TAGS=1`, decompiled with `tools/eedecomp.py`):
- **Im2D's packet** (`FUN_002FB690`, called through `FUN_002FB9D0`): `CNT` (3n + 1 qwords) with `DIRECT 3n + 1`, one
  PACKED GIFtag - NLOOP n, EOP, PRE with the RenderWare primitive as PRIM (`0x5D` fan / `0x5B` triangles / `0x4D`
  untextured fan / `0x25C` context-2 strip ...), NREG 3, REGS `0x512` = ST, RGBAQ, XYZ2 - then per vertex ST (u, v
  from the u16 fractions, Q 1.0), RGBAQ (the bytes, halved when `*0x488370`) and XYZ2 (x + 0x6C0 / y + 0x720 in
  1/16 pixel, minus 8: the half-pixel bias; z 32-bit). Every PATH2 packet that draws in normal play has this shape
  except a few effect strips.
- **Effect strips** in A+D packets: `PRIM`, then per vertex `UV` / `RGBAQ` / `XYZ2` register writes (the full-screen
  flat quads, 20 strips of 6 vertices; the reflection map's copies of FBP 140 into FBP 420, context 2, `PRIM 0x35C`
  with UV). Packets that set other registers before their `PRIM` (TEX0, ALPHA, TEST, ...) are state.
- The rest of PATH2 is A+D register setting and texture uploads (IMAGE).

**Frames outside gameplay** (2026-10-24): `FUN_0021ED88` is the main loop around `FUN_0021E950`, the per-frame
function (`FUN_0021CE80` begins the 3D frame - camera, clear, sky via `FUN_0018DDC0`; then the world, the 2D pass
`FUN_0021F348`, the menu `FUN_0021F7D8` -> `FUN_003B53F8`, and `FUN_0021DAD8(1)` ends the frame). **`FUN_0021E170`
is `LoadingScreen`**: its own frame - `FUN_0021CD20` begins it (clear), the splash full-screen through the sprite
routine `FUN_003DFF20` (raster from `FUN_0021DBF0`), the loading texts (`*0x486E64` 1..3), the progress bar
(`FUN_0021DFB0`, steps of 1/18 or 1/11) - and `FUN_0021DAD8(0)` ends it (kicked through `FUN_002723E8` like every
frame). It is called from many load paths (`0x346BAC`, `0x3C817C`, `0x45F3E4`, `0x46FE98`, `FUN_0021E950` when a game
is being loaded). **There is no title menu**: with an empty card the game goes from the loading screens straight into
the opening sequence (the reference scenes' "flyover"), where Start does nothing; the frontend menu is the pause menu.

### 6g. How a frame is put together (draw-list dumps, 2026-10-23)

From `PS2X_HW_DUMP` / F12 with `PS2X_HW_DUMP_BATCHES=4` at the hospital (-884, -470), noon and 20:00; the draw
log names the render target each copy reads (`src rt fbp=...`). The game draws a frame in one VBlank and nothing in
the next (30 fps). In order:

1. Opaque full-screen copy of buffer **140 -> 0** (240 vertices: strips) - the previous picture kept in FBP 0.
2. Sky gradient (24 vertices, `Cs*FIX`, z = far), cloud sprites (64x64, additive), horizon band (full width).
3. Alpha-only full-screen quad into 140 (FBMSK 0xFFFFFF), then the **native 3D** (~900-1300 world / lit draws,
   all into FBP 140).
4. Opaque full-screen **self-copy 140 -> 140** (120 vertices) - a shifted / filtered copy of the scene onto itself.
5. The reflection map: a 128x128 sample of 140 into FBP 420 plus sun glow sprites (not the radar - corrected
   2026-10-24); more 3D (particles, peds' second pass) into 140, the vehicles sampling 420.
6. Bloom: 256x256 ping-pong between FBP 420 and 452 (4 passes: clear `Cs*FIX` black, add the other at FIX 128 with
   tint 0x13), then added into 140 (`Cs*FIX + Cd`, FIX ~100).
7. Trails / colour: 512x512 downsample 140 -> FBP 384 (opaque), blended back into 140 at FIX 8 (`(Cs-Cd)*FIX+Cd`,
   624 vertices of strips) and added with a tint (0x10, 0x03, 0x09 at noon; 0x10, 0x01, 0x04 at 20:00).
8. Blend with the previous picture: FBP 0 -> 140 at FIX 16 (motion blur), alpha-only clear.
9. HUD / text at the far plane; the cinematic bars (zero height when closed) and text boxes (alpha 0x40).

**The post effects' exact draws** (2026-10-23, noon outside the barracks, 640x448 4:3, `PS2X_HW_DUMP_ALLVERTS=1`;
UV mapping per primitive fitted from the vertices, u = du/dx * x + offset in texels, x the GS position):
- Copy 140 -> 0 (frame start) and motion blur 0 -> 140 (`(Cs-Cd)*16/128 + Cd`): bilinear, **u = x + 0.5,
  v = y + 0.5** - on the GS (which samples pixel X at X) exactly texel X's centre: a plain copy.
- Self-copy 140 -> 140 after the 3D: point sampling, REGION_CLAMP, colour 0x7f, alpha 0, ZTST GEQUAL at z = 0 (only
  where nothing was drawn: the sky); strips with u = x + 0.94..1.88, v = 1.004 y + 0.94 - the sky shifted by about
  a pixel.
- Bloom: 640x448 -> 256x256 at FBP 420 (bilinear, 2.5 x 1.75 texels per pixel, +0.5), threshold `Cd - 255 * 35/128`,
  four blur passes (420 <-> 452: clear, then 35 quads at FIX 128 with colour 0x13 = a ~15% weight each, point
  sampling, tap offsets of a texel), added into 140 at FIX 100 (bilinear upsample, 0.4 x 0.57 texels per pixel).
- Trails: 640x448 -> 512x512 at FBP 384 (bilinear, 1.25 x 0.875), blended back at FIX 7 from 103 strips with offsets
  of up to 11 texels (a smear), then added at FIX 128 with a tint colour (0x10 0x03 0x09 at noon).
- A 128x128 sample of the picture (5 x 3.5 texels per pixel, no offset) goes into the corner of FBP 420, blended
  with what is there at FIX ~6-19 (a running average), plus an additive sun / glow sprite (64x64); right after,
  3D draws (vehicles, z ~0.001, blend FIX 6) are textured from that 128x128 corner - a reflection map refreshed
  every frame. After the bloom (which overwrites FBP 420) the sample is added again for the next frame.

**Pixel centres** (found from the copies above, fixed 2026-10-23): the GS samples a pixel at its integer position,
D3D at +0.5. Our renderer had drawn without the offset, so the copies (u = x + 0.5) read halfway between two texels:
the shown picture and the motion-blur feedback were averaged with their neighbours every frame - visibly softer than
the PS2 (barbed wire and fences smeared at 640x448). NATIVE_RENDERER.md "Post effects".

**Evening black frames (fixed 2026-10-23)**: between about 20:00 and 22:15 most frames came out black -
everything but the last text - with normal draw counts, native and VU path alike. Found by dumping the render target
after every full-screen draw (`PS2X_HW_DUMP_STEPS`): the scene in FBP 140 was fine until the bloom was added back
(step 6) and then 0 - the GPU copy of FBP 140 had been thrown away mid-frame. Cause, in our HW renderer, not the
game:
- In the evening the game uploads a 512x512 8-bit texture to block 8960 (page 280 = the Z-buffer area; Z is no
  longer needed by then). It also uploads a 16x16 CLUT at block 13588 (page 424, inside the bloom buffer FBP 420)
  and streams textures into the VRAM around 420 all the time (~6000 small uploads a second). VCS reuses VRAM freely.
- The renderer drops a render target (forgets its GPU copy) when an upload touches its pages. FBP 140 was tracked
  24 page rows tall (pages 140-380) instead of 14 (140-280), because the sky gradient's vertices reach y 738 and
  the height was taken from vertices, not from what the scissor (0..447) lets the GS write. So the Z-area texture
  "hit" the picture and dropped it. FBP 0 likewise stayed 32 rows tall after the boot-time clear of all VRAM.
- Fix (gs_hw.cpp `noteTarget`, `shrinkTargets`): a target's height is the last scissored line it can be drawn to;
  heights shrink back to what was drawn in the last 300 frames. Now 19:30-22:45 show a picture with the night
  bloom, and no target is dropped by the Z-area texture. A regression check (`tools/regress.py`, step "evening")
  keeps it fixed.
- Still dropped (as before): the bloom buffers FBP 420 / 384 when textures are uploaded around them, and the Z-clear
  view FBP 280. Copying uploads into targets block by block (`PS2X_HW_PATCH_TARGETS=1`) is exact but costs ~200 ms
  a frame with VCS's upload rate; it needs lazy patching (resolve only when the target is read) - milestone 3.

### 6h. The VU1 clipper and native drawing (GON_C3, 2026-10-23)

Found by the full mission smoke test: in mission 94 (`GON_C3`) the poolside cutscene showed the ground as black /
fog-blue on the native path, dim brown on the VU1 path (`PS2X_HLE=0`). `PS2X_HLE_VERIFY=1` was clean (26.5M
fast-path vertices exact, every cull decision right), and drawing only the *clipper* packets on the VU1
(`PS2X_HLE_CLIPPED`, now the default) reproduced the reference - so the difference is the GPU clipping that native
drawing used for packets the VU sends through its clipper. Not the cause (each tried and reverted): the GIF tag
(the kick's tag 49 is right - tag 50 is the second lit pass's, routine 0x2b2 = byte 0x1590, PRIM 0x20c: context 2,
untextured; stage table 0x19 always ends with 0x2b6 = 0x15b0, tag 49), perspective-correct ST, perspective-correct
depth, and the near / far clip planes.

The VU's clipper path, read from vu1_code_full.txt and reproduced bit-exactly in `src/game/vcs_vu_clipper.cpp`
(milestone 3, 2026-10-23):
- **Clip test 0x0c38** (all vertices, at least 6) writes per vertex three qwords to the output buffer: ST from the
  texture matrix *not* divided by w, with the clip-space w in lane w; the default colour (qword 51); the clip-space
  position (folded matrix 24-27). The 12 clip flags (plain in bits 0-5, guard band x 65 in bits 6-11) go into the
  input position's .w.
- **0x11f8** fog (when 52.y bit 1): F = clamp(53.w * w + 53.z, 0, 255) into the position's .w (w stays in the ST
  qword). **0x13d8** runs the backface pass 0x1440 when 52.y bit 0 (not reproduced: such packets stay on the VU1;
  none seen). The prelit colour stage then writes integer RGBA as on the fast path.
- **Kick 0x2b6 -> 52.z = 711 = 0x1638**, the per-triangle clipper. Tag 49's **w lane holds a second upper half with
  PRIM 0x7d** (triangle fan, IIP TME FGE ABE); the clipper builds its tags from it (y lane = 49.w). Qword 58 is an
  empty GIF tag (NLOOP 0, EOP) it kicks before every triangle - a sync, draws nothing.
- Per strip triangle (i, i+1, i+2): skip when the third vertex has ADC (0x8000) or all three share a plain flag;
  when no vertex crosses the guard band (flags & 0xfc0 = 0), emit the three as they are (0x23c0); otherwise copy
  them (colour as float) into a ping-pong buffer (66+1 / 111+1, four qwords per vertex) and clip:
  - planes (a x + b y + c w + d, from qwords 64..59): w > 0.3 (near, flag 32, routine 0x1b10), w < 2000 (far, flag
    16), y = -w, y = w, x = -w, x = w (flags 8, 4, 2, 1; 45-degree planes, routine 0x1f68) - each only when the
    polygon's OR of flags has its bit; inside / outside by the flag bit, intersection t = d0 / (d0 - d1), every
    lane of ST+w, colour and position lerped (a * (1 - t) + b * t, the inside vertex first), flags recomputed;
  - after the near plane, colours are multiplied by w (perspective-correct interpolation) and divided again when
    emitted (0x2608; after the near plane alone 0x2520 emits them undivided);
  - rejected when the AND of the output flags is non-zero; emitted as soon as no guard-band bit of a later plane is
    set (masks 1984, 960, 448, 192, 64, 0) - the GS scissor handles the rest.
- Emit: viewport matrix 12-15 applied to (x, y, z, w), / w, FTOI4 with the fog in F; STQ = ST / w; colours FTOI0
  after + 0.001. One GIF packet per polygon (fan, NLOOP = vertex count, EOP).

Checked with `PS2X_HLE_VERIFY=1` against the VU1's own XGKICKs (`runtime/ps2_vu1_tap.h`): GON_C3 98,906 clipped
packets / 1,199,117 GIF packets, the reference scenes 143,963 / 2,837,560 - all exact (ST, Q, RGBA, XYZF2; STQ.w is
not read by the GS). Native drawing hands those packets to the GS as PATH1 (`PS2X_HLE_CLIPPER=0` = the VU1; the
GPU clipping that drew the ground black is `PS2X_HLE_CLIPPED=1`). Cost over the reference scenes, means per
section: flyover render 14.6 ms (GPU clipping 14.1, VU1 17.7), cutscene 11.8 (11.3, 14.5), gameplay 5.9 (5.6, 7.6);
frame time at the 30 fps cap again. The intro office, which the stage-5 notes called black on the VU clipper, draws
correctly on it.

### 6i. RSL submission: how a frame's packets are built (2026-10-24, docs/RSL_SUBMISSION.md phase 0)

Found by decompiling the candidates (`tools/eedecomp.py`, headless Ghidra on the analysed EE project; output in
`build/analysis/decomp/`), a scan of the code for the packet buffer object, and two runtime measurements:
`VCS_TRACE_RSL=1` (before-hooks on every function that moves the cursor; bytes per function per frame,
`src/debug/rsl_trace.cpp`) and `PS2X_CHAIN_CENSUS=1` (every VIF1 chain tag by where it sits, its id and where its data
comes from).

**The packet buffer** is an object at `0x520F90`: +0x14 the half in use (two halves of 0xE6670 bytes from +0x20),
+0x18 the DMA cursor growing up from the half's start, +0x1C a data pointer growing down from its end.
`FUN_00476770` = bytes used (both ends), `FUN_00478628` = bytes free (checked before each write). About 33 functions
write it (the scan: `0x2F0360..0x2FB690`, `0x1CEF18`, `0x266868`, `0x2A92C0`, `0x2ABF68`, `0x32CD60..0x32D880`,
`0x40C430`, `0x40F6C8`).

**State, then draw.** RSL keeps render state in memory and writes it into the buffer only when a draw needs it:
- Matrices: blocks at `0x520860..0x520B8C` (view / projection / object, with dirty flags); `FUN_002F7620` computes the
  camera's. `FUN_002F8710` is the **state flush**: every dirty block as CNT + UNPACK into VU1 memory. Small setters
  upload single constants directly: `FUN_002F7090` (3 qwords at VU 0x2c = 44-46, the texture matrix),
  `FUN_002F9FA0` (one vector), `FUN_001CEF18` (3 qwords at VU 0).
- GS registers: queued as A+D pairs at `0x729510`, count at `0x488308`; each draw (and `FUN_002F7B30` on its own)
  sends the queue as one DIRECT GIF packet (CNT, `0x51` DIRECT, tag `0x1000000000008000 | n`).
- Textures: the texture bind `FUN_00408F30` -> `FUN_00317FB8` (material) -> `FUN_002F62B8` / `FUN_002F6A78` queue
  TEX0 / CLUT state and, when the raster is not resident, its upload: `FUN_003298D8` keeps a binary tree of transfer
  headers (nodes of 0x70 bytes from `0x6EE3F0`, pool at `*0x488760`, used / max `0x4CD58C` / `0x4CD588`: a cached
  BITBLTBUF / TRXPOS / TRXREG / TRXDIR packet per destination) and writes two REF tags - the header packet and the
  image - both sent as VIF DIRECT (PATH2).
- **Texture streaming** (2026-10-24, `FUN_002F62B8` decompiled; `PS2X_HW_UPSTATS=1`): `FUN_002F62B8(slot, psm,
  log2 w, log2 h, mip count, ..., source address, flags)` uploads a raster into a fixed VRAM slot - base
  `*(0x742B30 + slot * 8)`, each further mip at the next 64-block boundary - and skips everything only when the slot's
  key `0x742B50[slot]` (psm, sizes, mips, source) is unchanged. Flag bit 0 per mip: T8 uploaded as CT32 (and T4 as CT16)
  at half width / height - the RAM data is laid out for that. It then queues TEX0 (with the CLUT) / TEX1 / MIPTBP1 /
  MIPTBP2 for the draw. The texture's CLUT is uploaded with it, so while driving every bind uploads texture + CLUT
  (~390k uploads / 5 s, nearly all the same slots again). The header packet ends with the IMAGE GIFtag; the pixels
  follow in the next DIRECT: a REF straight to the raster in RAM whose VIFcodes are `NOP; DIRECTHL n` with n = its
  qwc (how docs/FULLY_NATIVE.md step 3d names textures by raster). T4 textures (the radar tiles among them) are sent
  as CT16, and their CLUT goes into the same GS page right after (other blocks).
- **Draws**, each starting with the state flush and the GS-register queue:
  - `FUN_002F7D58(packet)`: a **model instance** - CALL to the model's prebuilt packet in the model heap (444 calls a
    frame while driving); `FUN_002F80A0(packet)` the same shape (a variant);
  - `FUN_002F7EB0(addr, qwc)`: **world geometry** - REF to a sector's prebuilt packet in the streamed cells (called by
    the sector draw `FUN_002409C8`, whose texture / material setup is itself cached: the first frame copies the
    setup packet into a block on the heap at `0x6F3DB8`, later frames CALL it);
  - `FUN_002F8250(...)`: REF with its own UNPACK / MSCAL (immediate geometry);
  - `FUN_002FB9D0` -> `FUN_002FB690`: **Im2D** (the 2D primitives of 6f: CNT + DIRECT, vertices converted from the
    16-byte Im2D format; 515 calls a frame while driving).

**Measured** (one bench run, i7-7700K, 60 fps cap; per frame):

| | standing in the city | driving (AI car) |
|---|---|---|
| bytes built in the packet buffer | 90-150 KB | ~470 KB |
| VIF1 chain | 720-1,300 tags, ~33-48k payload qwords | ~9,600 tags, ~294k qwords (~4.7 MB) |

Driving, by function (bytes are charged to the hooked function entered last, so a draw's own CALL / REF lands on
`FUN_002F8710`, which every draw enters first): `2F8710` 848 calls / 135 KB, `2FB690` 515 / 125 KB (2D),
`2F62B8` 326 / 64 KB (textures), `2F7090` 304 / 33 KB, `2F7908` 238 / 23 KB, the rest < 20 KB each. The chain while
driving: REFs from cell packets to data in the `0x6EDC70..0xCB7A80` range 78k qwords, the per-frame REFs into the
model heap 55k, cell geometry 34k, CALLed model packets 31k, per-frame CNT data (2,250 tags) 16k, VU1 code (REFs to
`.text`) 8k. **About 10% of what VIF1 receives is built each frame; the rest is prebuilt** (cells, model heap,
cached blocks).

Game thread while driving (`VCS_PROFILE=5`, `VCS_PROFILE_GUEST_FILE`, the two drive windows: 70% guest code, no
waiting): the RSL range `0x2F0000..0x2FD000` 12.5% of samples (the packet emitters 10.1%), the world sector walk 1.5%,
plus `memmove` 3.8% (the runtime flattening the ~4.7 MB chain when it is kicked) - **about 17% of the game thread,
~3 ms of a ~19 ms frame**, goes into building and handing over packets. The rest is spread thin (no other function
above 2.3%).

**Coverage of the draw functions** (docs/RSL_SUBMISSION.md phase 1a, `VCS_RSL_SHADOW=1`, `src/debug/rsl_shadow.cpp`):
before-hooks record every call of the draw functions with its argument and the cursor at entry; as the runtime copies
the frame's chain (`runtime/ps2_chain_observer.h`) each packet-buffer tag is placed in the record whose range holds
it. Every draw wrote exactly its own tag - a model instance a CALL to its packet, a world draw a REF to its geometry
(address and qwc): city spot (1325, -325) 394 model / 42-47 world draws a frame, driving 495 / 79 plus 19.5 immediate
(`FUN_002F8250`) - **all matched**. The only VU1-bound tags outside a draw's range (7 a frame standing, 14 driving)
are state: at the frame's start `FUN_002F53D8` and `FUN_002F9FA0` CALL the camera object's packet (`0x16E7B90`,
object at `*0x4882DC` + 0x10), the sector draw `FUN_002409C8` CALLs its cached setup blocks, one CALL follows a
`FUN_001CEF18` upload. The blocks allocated from the top of the half (+0x1C) hold only RET tags (the ends of CALLed
per-frame blocks). All 32 functions that store the cursor (a scan of the code following register copies) are hooked.

**What a draw carries in state** (`FUN_002F8710`, the flush every draw runs first; dirty bits in `0x4882A0`):
bit 0 CALL the current object's packet (object `*0x4882A4` + 0x10) and its plug-ins' callbacks (object + 0x114 list,
+0x48); bit 1 CALL `*0x4882A8`; bit 2 three qwords from `FUN_001CFAE0(object + 0x134, ...)`; bits 0 / 4 with 0x200 /
0x400 MSCAL 0x4A / 0x75 (the setup programs 0x0250 / 0x03A8, 6d); bit 0x2000 CALLs from the table at `0x742AB0`;
bit 8 UNPACK 5 qwords to VU 49-53 (the GIF tags, colour, flags, fog) from `0x742AE0`; bit 0x1000 UNPACK the viewport
(VU 12-15) from `0x520920`, MSCAL 0x8F (setup 0x0478), the guard band (VU 65) from `0x520AC0`.

**Where a draw program's VU state comes from** (phase 1b, `VCS_RSL_SHADOW=1`: each MSCAL of 0x0030 / 0x02C0 is tied to
the draw whose stream started it by its offset in the copied chain, and the VU memory it starts with is compared with
the draw's RSL state snapshotted at the draw call; the map below found from `VCS_RSL_SHADOW_DUMP` samples):

| VU qwords | what | RSL source | same as the RSL source |
|---|---|---|---|
| 0-3 | object matrix | `0x520860` | 85-97%; the rest whole matrices the chain itself uploads (object packets, cell data) |
| 4-11 | camera matrices | `0x5208A0`, `0x5208E0` | 100% |
| 12-15 | viewport | `0x520920` | 63-98%; the rest a depth range (rows 14-15) some model packets upload, which stays for the draws after them |
| 32 | camera position | `0x520990` | 100% |
| 34-37 | setup slot table | render context (`*0x4882A4`) + 0x60 | 100% |
| 49-51, 53 | GIF tags, colour, fog | `0x742AE0` | 100% |
| 52.y | flags | `0x742AE0` + 0x34 | 100% (52.x / .z are written per packet / by the programs) |
| 59-64 | clip planes | `0x5209A0` | 100% |
| 65 | guard band | `0x520AC0` | 100% |
| 16-31 | view, clip and window matrices | computed by the setup programs (6d) from the rows above | - |

Every differing value is accounted for (bench run: city spots and the drive, 0.000% unexplained): uploaded by the
draw's own stream (its state flush, setters, its packet), carried over from an earlier draw (VU memory persists), or a
whole block uploaded earlier in the chain. World draws match the RSL state completely; model draws add their packets'
own state. The "render context" (`*0x4882A4`) is not a per-instance object: a few values recur (`0x16EEEC0`,
`0x16EF040`, `0x16EF1C0`); the per-instance matrix is the block at `0x520860`.

**Sectors cache their own packets** (2026-10-25, `FUN_00240ED8` read from its decompile; found when packet-buffer
tags turned up in the chain of later frames). `FUN_00240ED8(level, sector entry)` draws a sector through
`FUN_002409C8(level, sector, pass)`, and the first time a sector is drawn in the caching branch (the frustum test
`FUN_002F7528` not 2, `*0x487484 >= 60.0`, the entry's cache word `entry + 4` still 0) it notes the cursor, lets
`FUN_002409C8` write the sector's packets (its material setup, the GS registers and every world draw's flush, GS packet
and REF), puts a RET after them, copies the whole range into a block allocated from the pool at `0x6F3DB8`
(`FUN_002BC730`, copied by `FUN_0045CE10`), stores the block in `entry + 4` and draws it with `FUN_002F7D58(block)` -
a model draw of the copy. Later frames only make that model draw: the sector's packets as written on the first frame
are sent again for as long as the block lives. The caching call of `FUN_002409C8` returns to `0x241060`. (When the
pool has no room the sector is drawn a second time by a plain call and the first copy's packets stay in the buffer.)
`FUN_002409C8` itself caches each material's setup the same way (texture bind, `FUN_002F7B30`'s GS packet) into a
block at `entry + 4` of the material and CALLs it on later frames: the cursor noted, `FUN_00408F30` called from
`0x240D14` (returns to `0x240D1C`), `FUN_002F7B30` from `0x240D1C` (returns to `0x240D24`), the range copied by
`FUN_0045CE10` at `0x240D9C`. Code that writes something frame-specific into
the packet buffer while a sector is cached therefore has it sent again in later frames - the draw items of
docs/FULLY_NATIVE.md step 4c stay packets there.

**A world block** (the REF payload of `FUN_002F7EB0`, sector geometry in the streamed cells; listed from a RAM dump,
2026-10-24) is one shape throughout: per packet `UNPACK V4-32 x1 -> TOP+0` (the header: count, nloop), `STMASK`,
`STROW`, `UNPACK V3-16 x n -> TOP+1` (positions, masked: w from ROW), `STMASK`, `STROW`, `UNPACK V2-8 usn x n -> TOP+0x48`
(UVs, masked), `UNPACK V4-5 x n -> TOP+0x8F` (colours), `MSCAL 6` (program 0x0030) - no constants; everything else the
world program reads is VU-resident state from the draw's flush (6d table 0x19). Sizes seen: 208-2,340 qwords, packets
of up to 70 vertices. Phase 2 draws these from recordings (docs/RSL_SUBMISSION.md).

**A model packet** (the CALL of `FUN_002F7D58` / `FUN_002F80A0`; decompiled with `tools/eedecomp.py`, packets listed
from a RAM dump and checked by the phase 2 recorder, 2026-10-24):
- Both functions run the state flush, write the queued GS registers (`0x729510`, count `0x488308`) as a
  `CNT` + `DIRECTHL`, then `CALL a0` with qwc 0 and VIFcodes 0 (`a0 << 32 | 0x50000000`). `FUN_002F80A0` first CALLs
  the packet at `*0x48830C` when bit 31 of the dirty word `0x4882A0` is set (and clears it) - a state packet shared
  by the draws after it.
- The CALLed packet is a sub-chain, not one block: up to ~64 tags (`CNT` with inline data, `REF`s into the model
  heap - e.g. 6-qword constant blocks shared between packets - and a closing `RET`); single-packet models are one
  `RET` with its payload. VIFcodes seen: FLUSHE, UNPACK V4-32 (constants: matrices, the depth range of 6i's VU table
  rows 14-15), DIRECTHL (GS registers), STMASK / STROW, UNPACK V3-16 / V2-8 / V4-5 / V3-8, STCYCL, MSCAL 0x0030
  (most), 0x0478 (setup) and 0x02C0 (re-draw).
- **Fields an UNPACK leaves alone**: an unmasked V3 / V2 UNPACK writes only x y z (x y) - w keeps what VU memory held
  (the runtime's VIF interpreter; not checked against hardware) - and masked UNPACKs leave write-protected
  fields (mask 3). Skinned packets rely on it: two recordings of the same packet bytes differed only in such a w
  (`0x802A` / `0x801A`), left by the packet drawn before.
- **Other CALLs** in the packet buffer: packets at `0x1FDA020`-`0x1FDA120` (320-1,088 bytes) are rebuilt every frame or
  every other frame; a few state packets in the `0x16EExxx`-`0x16EFxxx` render-context blocks (6i above) use VIF BASE.
- Volume at city spot (-700, -500), per 10 s: 248k `FUN_002F7D58` CALLs, 106k `FUN_002F80A0` CALLs, 89k other CALLs,
  61k world REFs; ~1.29M draw program MSCALs among them.

**Texture uploads** (2026-10-25, from the decompiles of `FUN_003298D8`, `FUN_002F62B8`, `FUN_002F6A78`; native as
reference ops since FULLY_NATIVE.md step 5b):
- `FUN_003298D8(slot, psm, log2 w, log2 h, bytes, source, dest)` writes **two REFs into a 32-byte slot its caller
  reserved**: one to the destination's transfer header (qwc 6, VIFcodes FLUSHE, DIRECTHL 6) and one to the pixels
  (qwc `bytes / 16`, VIFcodes NOP, DIRECTHL `bytes / 16`). The header is a 0x70-byte node of a binary tree rooted at
  `0x6EE3F0`: key at +0x60 = `(dest >> 6) << 48 | (psm & 0xFFFF) << 32 | (h & 0xFFFF) << 16 | (w & 0xFFFF)` (compared
  unsigned; smaller keys left at +0x68, larger right at +0x6C), the packet at +0x00: GIFtag `0x1000000000000004` A+D,
  BITBLTBUF (0x50), TRXPOS (0x51), TRXREG (0x52) from the sizes, TRXDIR (0x53) and the IMAGE GIFtag
  (`bytes >> 4 | 1 << 59`). A missing node is made from the pool at `*0x488760` (used / max `0x4CD58C` / `0x4CD588`);
  with the pool full both REFs are written empty (qwc 0).
  On the found path (2026-11-02, from the recompiled code's disassembly) it writes exactly: +0 the header REF
  (`0x30000006`, node), +8 `0x10000000` (FLUSHE), +0xC `0x51000006`, +0x10 the pixel REF (`bytes >> 4 | 0x30000000`,
  source), +0x1C `bytes >> 4 | 0x51000000`; +0x18 is left alone. Besides the slot only its own stack frame (the key at
  sp - 0x20, the search direction at sp - 0x18, `$s0` saved at sp - 0x10); no calls; it leaves `$v0` = the pixel REF
  dword, `$v1` = `0x51000000`. The recompiled tree search can leave for the EE scheduler at its loop's back edge and be
  resumed later (a check right after the call saw a few hundred of 3.2 M calls unfinished). While driving it runs
  40-70k times a second, nearly all of them while a material cache is filled (the RSL item path's "a cache being
  filled", so the step 5b hook calls the original). Doing those natively was byte-exact (3.2 M calls compared) but made
  the cruise hitch more (PERFORMANCE.md 28) - not kept.
- `FUN_002F62B8` (the bind into a VRAM slot) reserves 32 bytes per mip level and calls it for each (128 bytes and
  `FUN_00266608` instead when `*0x488318 == 0` - not seen), then writes a CNT 5 with DIRECTHL 5 (A+D: TEXFLUSH, MIPTBP1,
  MIPTBP2 for the slot, register 0x42, TEXFLUSH) at the cursor, and queues TEX0 into `0x4882E0 + slot * 8` with the
  dirty bits. `FUN_002F6A78` (the CLUT) reserves 32 bytes for one call (CT32 16x16 or 8x2 by the palette size) and may
  write a CNT 2 (A+D: register 0x42, TEXFLUSH).

**Im2D's packet writer** `FUN_002FB690(prim, n, vertices, indices, context, screen)` (2026-10-25, from the recompiled
code's disassembly; its only caller is `FUN_002FB9D0`, which does not touch the packet after it): nothing while
`*0x488398` is set; else one CNT of 3n + 1 qwords whose VIFcodes are NOP, DIRECT 3n + 1. The GIFtag: NLOOP n, EOP,
PRE, PRIM = prim | IIP | TME (when the context's texture entry `0x6EDCBC + context * 4` is set) | ABE | CTXT (context << 9), NREG 3, REGS
`0x512` (ST, RGBAQ, XYZ2). Per vertex (16-byte Im2D vertices, through the index list when there is one): ST = the
16-bit u / v * 2^-15 with Q 1.0 (kept from the vertex before when untextured), RGBA from bytes 4-7 with alpha halved
(every channel halved while `*0x488370` is set), XYZ2 = ((x + 0x6C0) << 4) - 8, ((y + 0x720) << 4) - 8 from the
16-bit coordinates (or x << 4, y << 4 when `screen` is set) and Z = the 32-bit word at +0xC through int -> float ->
int. The HUD layer draws these (NATIVE_RENDERER.md "HUD layer"); since FULLY_NATIVE.md step 5c they reach it as ops.

**The frame's fixed structure** (2026-10-25, `PS2X_DUMP_CHAIN=<n>` - every tag of one kick - read with the census).
The chain starts in the static root list (`0x480D20` / `0x480DE0`, 6a): CALLs to fixed packets in `.data` around
`0x480EA0`-`0x481760` that send GS setup (CNT + DIRECT), VU1 setup (CNT STCYCL + UNPACK, NEXT, CNT MPG - VU1
microcode - and RET with an MSCAL) and finally a NEXT into the packet buffer's half. **Render-context packets**
(CALLed by the state flush, bits 0x1 / 0x2 / 0x2000; in the model heap and the heap) open with a REF whose VIFcodes
are **BASE / OFFSET** (VU1's double buffer), then CALL a block of **MPG REFs into `.text`** (the draw programs: e.g.
five of 128 / 112 qwords from `0x2AC130`, one of 27 from `0x25E890`, three from `0x368730`) and end with a REF of the
context's matrices (STCYCL, UNPACK) and an MSCAL. Every frame re-sends the same microcode (the runtime copies it only
when it differs). The Z clear (6a: 40 column strips) is a VU1 program in the chain, run natively (`zclearProgram`,
`src/game/vcs_hle_world.cpp`).

**The other packet writers** (2026-10-25, step 5d of FULLY_NATIVE.md; callers from the recompiled code's `jal`s, shapes
from decompiles and the census of what each writes): `FUN_002F8250` is **Im3D's submit** (called by `FUN_002FC2E8` /
`FUN_002FC838`, `FUN_002FC210` writing the vertices): it runs the state flush, then REFs the vertices with an UNPACK
and an MSCAL. `FUN_0040F6C8(vu, src)` (called by `FUN_0040F798`) writes one CNT with a 9-qword UNPACK and returns 9;
`FUN_002A92C0(program, block)` (called by `FUN_002A9538`) writes a CNT UNPACK, a REF to the block's data with an UNPACK
and an empty REF carrying an MSCAL - immediate geometry with its own program. `FUN_0040C430` and `FUN_00266868` have
no direct callers (reached through function pointers - plug-in or pipeline callbacks). `FUN_002F0360` and
`FUN_0032D880` (which calls `FUN_0032D480` / `FUN_0032D7C8`) are called from `FUN_001ACE40`; `FUN_002F39D8` from
`FUN_0035C9E0`, `FUN_002F3DD0` from `FUN_002F4080`, `FUN_002F5520` from `FUN_0018F1C8` / `FUN_0018F450`, `FUN_002ABF68`
from `FUN_002AC048`. None of them patches its packets after returning (checked: every captured region is unchanged
at the kick).

**The state setters** (2026-10-25, decompiled before / read from the recompiled code's disassembly comments; native as
state ops since FULLY_NATIVE.md step 5a):
- `FUN_002F7908` and `FUN_002F7B30` are the same code: queue FOG (6) / FOGCOL (7) / registers 0x14 / 0x15 by the
  dirty bits 0x80 / 0x100 / 0x20 / 0x40 (values at `0x4882E0` / `E8` / `F0` / `F8`), clear bits 0x1E0, and when the
  queue is not empty send it as the draws do (CNT n + 1, FLUSHE, DIRECTHL n + 1, GIFtag `0x1000000000008000 | n`, the
  pairs) and empty it.
- `FUN_002F7090(m)`: CNT 3, FLUSHE, UNPACK V4-32 x3 -> VU 0x2C, the three qwords at `m` (the texture matrix, VU
  44-46; with too little room it writes them through a null pointer).
- `FUN_001CEF18(vu, m)`: CNT 3, FLUSHE, UNPACK V4-32 x3 -> VU `vu` (the code word `vu | 0x6C030000`), the three
  qwords at `m`; returns 3. No room check.
- `FUN_002F9CE8()`: CNT 0 with NOP, FLUSHA.
- `FUN_002F9FA0(x, y, z)` runs the flush, then an UNPACK of a vector computed with float divides (`/ 65535.0`,
  `* -32768.0`); `FUN_002F53D8(...)` the frame's camera packet: CNT 0 (STCYCL 1 1, FLUSHA), CALL `*0x4882DC + 0x10`,
  dirty word `0x3E1F`.

**The state flush** `FUN_002F8710` (decompiled with `tools/eedecomp.py`, 2026-10-24; native in
`src/game/vcs_rsl_native.cpp`, byte-exact against the original). The dirty word `0x4882A0`, in the order written:
- 0x1 render context `*0x4882A4` changed: `CALL context+0x10`, then each plug-in on the context's list (head
  `context+0x114`, end `context+0xB0`, next `+0x64`) with a callback at `+0x48` is called (args at `+0x54` / `+0x58`);
  0x2: `CALL *0x4882A8`; 0x4: `FUN_001CFAE0` -> UNPACK of 3 qwords; then the plug-ins' `+0x4C` callbacks (bit 0x1) or
  `+0x50` (bit 0x10 alone); 0x11 unless 0x600 are both set: MSCAL 0x75 / 0x4A (the ones bits 0x400 / 0x200 do not
  send themselves).
- 0x2000: three CALLs from the table `0x742AB0[*0x4CD57C]`; 0x8: UNPACK V4-32 x5 -> VU 0x31 from `0x742AE0`;
  0x1000: viewport `0x520920` -> 0x0C (4 qw, MSCAL 0x8F) and `0x520AC0` -> 0x41; 0x800: `0x5208E0` -> 0x08 (MSCAL
  0x8F) and lights `0x5209A0` -> 0x3B (6 qw); 0x400: `0x520990` -> 0x20 and `0x5208A0` -> 0x04 (MSCAL 0x83); 0x200
  (when `*0x4882B0 == 0`): the instance matrix `0x520860` -> 0x00 (MSCAL 0x3C), or a REF to `*0x4882B8` instead.
- GS registers to the queue `0x729510` (`{value, register}` pairs, count `0x488308`; the draw sends them as a
  `CNT` + `DIRECTHL` right after the flush): 0x80 FOG (6) `0x4882E0`, 0x100 FOGCOL (7) `0x4882E8`, 0x20 register
  0x14 `0x4882F0`, 0x40 register 0x15 `0x4882F8`. The word is left with bit 31 only.
- **The plug-in callbacks** (2026-10-25, `VCS_RSL_PLUGIN_LOG=1`: which callbacks the flushes would call, while
  driving; the callers' code read from the recompiled functions): only `+0x50` callbacks (bit 0x10 without 0x1) occur,
  called as `cb(node[+0x34], node[+0x54], node[+0x58])` in the list's order, after the CALLs of bits 0x1 / 0x2 and before
  the MSCALs; their return values are not used. Four functions: `0x1CEF90` = `FUN_001CEF18(vu, src)` (CNT 3: 3 qwords
  -> VU `vu`), `0x40F798` = `FUN_0040F6C8(vu, src)` when the word at `src + 0x90` is set (CNT 9: 9 qwords, and the word
  cleared), `0x2A9538` -> `FUN_002A92C0` and `0x2F4080` -> `FUN_002F3DD0` (immediate geometry with REFs and their own
  program). The first two are ~85% of the callback flushes; the native flush writes them since FULLY_NATIVE.md 5f.
- **Inside a draw, by return address** (2026-10-25): a hook cannot tell "inside a draw" from "after it" by the guest
  stack pointer alone - a draw's caller often calls a helper next, whose own draws run deeper on the stack than the
  first draw did. What runs inside a draw is called from the draw function itself (`FUN_002F7EB0` 0x2F7EB0-0x2F8024,
  `FUN_002F7D58` 0x2F7D58-0x2F7EAC, `FUN_002F80A0` 0x2F80A0-0x2F8250), its flush (`FUN_002F8710` 0x2F8710-0x2F937C),
  the callback wrappers (`0x2A9538`, `0x2F4080`, `0x40F798`, `0x1CEF90`) or `FUN_001CFAE0`: a hook is inside a draw
  when it is deeper on the stack *and* its `$ra` lies in one of those (bounds from `game/vcs/functions.csv`). With
  stack depth alone every later draw of the caller's helper counted as "inside" (items fell from ~31k to ~12k per 10 s
  while driving).
- **Emitters draw models** (2026-10-25): some captured emitters call the RSL draw functions inside them (the draws run
  the original there: their packets belong to the emitter's region). The draws' tags then sit inside the emitter's
  region, in the order the draws were made. A region that takes such a tag into an item must also drop the draw from
  the frame's draw list - the walker matches the list against the chain's tags in order, and one missing tag puts it
  out of step for the rest of the frame: no model packet is ever learned (`not known`), and standing at Fort Baxter no
  model draw became an item at all.
- **The texture binds' own packet** (2026-10-26, `VCS_RSL_CENSUS=2` and a dump of the payloads): after its uploads
  (`FUN_003298D8`, called at 0x2F660C), `FUN_002F62B8` writes a `CNT 5` + `DIRECTHL 5` itself (0x2F66A4 on: reserves
  0x50 bytes after `FUN_00478628`'s free-space check) - an A+D GIF tag with TEXA (0x3B), MIPTBP1_1 (0x34), TEX2_1 (0x36,
  the CLUT load) and TEXFLUSH (0x3F); values from the bind's locals and the table at 0x742B30. ~80 a frame while
  driving. The binds reserve their upload slots first and fill them through `FUN_003298D8`: a capture must never read
  the buffer between the two (half-filled slots).
- **Callers hold the cursor across calls** (2026-10-26): `FUN_00240ED8` keeps the start of the range it copies into a
  sector's cache across its call of `FUN_002409C8`; moving the cursor back at a hook point inside that call shifted the
  copied range (the frame lost its alpha pass, effects, HUD and post passes). Packets may be taken at any hook point,
  but the cursor may only move back where no caller can hold it - in practice: only for an emitter / draw region
  settled at its own stack level, never for a gap (FULLY_NATIVE.md 5h).
- **The material cache's last packet** (2026-10-26): `FUN_002409C8` calls `FUN_002F7B30` at 0x240D1C+8 (return
  0x240D24) to write the material's GS packet, and copies the material's packets (bind and that GS packet) into its
  cache after it returns - the window covers that packet, so it ends at the next hook point, not at the call.
- The buffer's free space is `0xE6670 - FUN_00476770(0x520F90)` (`FUN_00478628`); with too little the flush writes
  through a null pointer (never seen). While driving ~9% of flushes have plug-in callbacks (skinned / effect
  plug-ins) and ~1% bit 0x4; those still run the original.

### 6j. The helicopter's dust: the heaviest fill in the game (2026-10-30)

Found from a user report (frame time spiking at 3x internal resolution while a helicopter flew low): a 4-star wanted
level brings the police helicopter, and while it hovers near the ground the game draws its rotor dust as **one GS-path
draw a frame of ~1,040 large triangles** (~3,100 vertices, quads as triangle pairs; texture 32x32, bilinear, REPEAT;
blend `Cs*FIX + Cd` with FIX 128 - additive - colour 0x03-0x13; ZTST GEQUAL, no Z write). Measured from an F12 draw log
with every vertex (`PS2X_HW_DUMP_ALLVERTS=1`, triangles clipped to the screen): **60-66 full screens of overdraw**,
~18 Mpixels at 640x448 - on the PS2's GS (~1.2 Gpixel/s textured) about 15 ms, so the effect is heavy on the console
too. A few smaller draws with other 32x32 textures (the same blend, up to ~6 screens) go with it. At 3x internal
resolution in 16:9 (2560x1344) it is ~206 Mpixels a frame through the GS-emulating pixel shader: that, not the CPU, was
the spike (NATIVE_RENDERER.md "The GS bilinear by Gather", PERFORMANCE.md section 15).

How it was found: the perf line showed the render worker and the GS stage mostly waiting (sampling profiles of both
threads: ~85% in a wait), the main thread too (~90% in `ZwWaitForSingleObject`: on the GPU); the overlay's "GPU" figure
is the main thread's submit time, not GPU time. `PS2X_GPU_TIMING=1` (timestamp queries per draw) then put 46 of 61 ms
of a presented frame on `gs draw fbp 140 tex blend`, and per texture key on this one 32x32 texture.

Seen on the way (not the helicopter's doing - the same before it arrives, every frame): the GPU targets of the effect
buffers FBP 420 and 384 are re-created ~4 times a frame. A 16x16 CLUT upload at block 13588 (page 424, inside both
buffers' pages) drops both targets every frame (`PS2X_HW_LOG_DROPS=1`); FBP 420 is then made at the window scale for
the reflection map's 128-wide draws (role windowScale, 4.0 x 3.0 at 3x 16:9), regrown at 3x for the bloom's write-back
(fbw 4), and back again - each a new texture plus a rescaling blit (`PS2X_RHI_LOG_CREATES=1` lists them). The CLUT is only
the first of many: with patching on (`PS2X_HW_PATCH_TARGETS=1`, every upload kept in the target) ~75 uploads a frame
land in the pages of FBP 420 / 384 - the game streams textures into its effect buffers' memory, which is why patching
them all costs so much and why they are dropped. Fixed 2026-10-30 without changing what a target holds (PERFORMANCE.md section 15): one scale per FBP (any
windowScale role there), a dropped or outgrown target's texture reused by the next target of its size (cleared as a new
one), a dropped target re-made at the size it had. The drop itself stays (the reflection map starts from cleared each
frame, as before).

### 6k. Plane trails and RenderWare's Im3D line clipper (2026-10-30)

Found from a user report: long thin white lines across the picture in the morning, starting at the white trails that
planes leave in the sky (seen outside the Little Havana Empire site, 08:28, 16:9 - and at 4:3 too).

- **The trails** (VC's `CPlaneTrails`): six of them at `0x50B5D0`, 0x140 bytes each - 16 points (`CVector` + pad, 16
  bytes) then 16 times (`u32` ms, at +0x100). Drawn by `FUN_00245A28`, which calls **`FUN_00245768` = `CPlaneTrail::
  Render(this, visibility)`**: `CCamera::IsSphereVisible(TheCamera 0x6F44D0, point 0, 1000)`, alpha = visibility * 110,
  a point older than 30 s (`now - time >= 30001`, the clock at `0x4CD104`) gets time 0 and is skipped, the rest go to
  a vertex buffer at `0x709650` (stride 0x18, white, alpha fading over the last 10 s), then `RwIm3DTransform`
  (`FUN_002FBFA8`) and `RwIm3DRenderIndexedPrimitive(2, 0x4873B0, 2 * (n - 1))` (`FUN_002FC2E8`; the static
  indices 0,1,1,2,...). The points are 500-600 units up and sane (read with the rig while the lines showed).
- **Lines go through the EE, not the VU1**: `FUN_002FC2E8` with type 2 loops over the index pairs and calls
  **`FUN_002F95E0`, the line clipper**: each end to clip space (`FUN_001AAA38`, the VU0-macro matrix multiply with
  the camera's matrix at `0x520A80`), clip flags from **`FUN_0032E508`** (`ctc2 $zero, vi18; vclipw.xyz vf1, vf1w;
  cfc2 $v0, vi18`), trivial reject on a shared outside bit, otherwise a cut per plane by bit (`FUN_002F97F0`, plane
  table `0x5209A0`, 16 bytes a plane), then emitted as a PATH2 GIF packet (ST / RGBAQ / XYZ2, `FUN_002F9AA0`).
  `FUN_0032E508` is the only VCLIP in the EE code.
- **The bug was the recompiler's VCLIP**: it set bit 0 for x < -w and bit 1 for x > +w (swapped; the VU sets bit 0
  for x > +|w|, bit 1 for x < -|w|, then y, z) and compared with w, not |w|. A trail segment leaving the view on the
  right was cut at the left plane; the side planes meet at the eye, so the cut point landed next to the camera (w
  0.11 - 0.12, Q ~8.9 in the F12 log) and its projection, far outside the GS's 16-bit coordinate range, wrapped to
  anywhere on screen: a line from the trail across the picture. Fixed in `ps2xRecomp/src/lib/vu_translator.cpp`
  (the VU1 interpreter's CLIP, `ps2_vu1_upper.cpp`, was right); `VCS_SELFTEST=1` checks the flags.
- How it was found: F12 with `PS2X_HW_DUMP_ALLVERTS=1` showed one line draw with one vertex at Q 8.9 and the same in
  every path (compiled / interpreted VU1, `PS2X_HLE=0`, the GS front end), so the game itself sent it; a trace of the
  kicks showed PATH2 and no ADC; `VCS_TRACE_IMMEDIATE=1` (callers of the Im2D / Im3D entries, `vcs_overrides.cpp`)
  named `0x245904` in the trail renderer.

### 6l. Distance fog: where its range and colour come from (2026-11-02)

Found while giving the fog settings (`src/game/vcs_fog.cpp`) a home: read from the recompiled code's disassembly
comments, then measured live with the rig (`fogstate`, `peekf 0x742B28 2`) and `VCS_TRACE_FOG=1`.

- The fog itself is the GS's (6d): per vertex `F = clamp(53.z + 53.w * w, 0, 255)` from the camera-space depth w,
  blended per pixel with FOGCOL. 53.z / 53.w live at **`0x742B28` / `0x742B2C`** (VU 49-53 are uploaded from
  `0x742AE0`, so qword n is at `0x742AE0 + (n - 49) * 16`).
- **`FUN_002F6008(start f12, end f13)`** - the RSL fog-range setter, a leaf: `d = end - start`, 53.w = `-255 / d`,
  53.z = `(start / d + 1) * 255`, i.e. `F = 255 (end - w) / (end - start)`; sets dirty bit 0x8 when either changed.
  Its only caller is the camera's begin-update **`FUN_0018EE48(camera)`**, which also stores the camera at
  `0x486450`, clamps its near plane (+0x78) to >= 0.1, builds the projection (`FUN_0018F4D0`), queues FOGCOL
  (`{colour, 0x3D}` into the GS register queue `0x729510`) from the word at **`0x4E1B30`** through `FUN_002F5FB8`
  (bytes 0 and 2 swapped on the way), then calls `FUN_002F6008(camera +0x80 (fog plane, `FUN_0018F4B8`),
  camera +0x7C (far plane))`.
- The other setters next to it, all on the 0x742AE0 block and dirty bit 0x8: `FUN_002F5D90(on)` 52.y bit 0 (the
  extra pass), `FUN_002F5F10(on)` 52.y bit 1 (**fog on / off**; it also calls `FUN_002F6268(.., 0x20, on)`),
  `FUN_002F5EA0(rgba)` qword 51 (the default colour, one byte a lane, cached at `0x4882B4`), `FUN_002F5E00(f)`
  52.w, `FUN_002F5E30(flag)` 52.x = the transform routine as a VU1 address, `(0x2AC878 or 0x2AC820 - 0x2AC130) / 8`
  = 0x0E9 / 0x0DE (the microprogram's image starts at `0x2AC130`).
- Measured (Vice Point, the user's card): the far plane is 2000 in every weather and hour tried, so the fog is total
  exactly where the draw distance ends. The start is the time cycle's: ~55 at noon (it drifts by ~0.14 a second as
  the hours blend), 22 at 22:00, **-28.6 in foggy weather** (fog already at the camera). Indoors (the Viceport
  safehouse) 53 read 100 .. 1000. One camera calls it (no second range in the trace).
- So scaling the setter's two arguments moves the fog in every renderer path at once (VU1, CPU HLE, the native
  vertex shaders' `nativeFog`, the clipper), with the time cycle's colour; at 100 % the game's own values go in.

### 6m. Fire: GS sprites lost to the pixel shader's depth on D3D11 (2026-11-03)

Reported by the player: no flames at all - the flame-thrower's, molotovs', explosions', burning cars'; their smoke was
there. Reproduced by `tools/fire_check.py` (a car set to health 200, the flame-thrower fired, a molotov thrown; fire
pixels counted) and found with F12 draw logs, a read-back of the depth buffer just before the flame draw, and switches.

- **The fire is GS-path sprites** drawn after the native 3D: the flame texture (64x64, `key 48cd3e4b597ff6e`, the
  decode correct: orange, alpha 0x80) as additive quads - `Cs*FIX + Cd` with FIX 128 (vertex colour ~0x1F, dozens of
  quads adding up) and `Cs*As + Cd` (colour 0xFF, alpha 0x78) - with ZTST GEQUAL, no Z write. The explosion is the same
  kind of sprite.
- **Their Z** is the game's: `FUN_0039FBF0(z) = FUN_002F9F58(z) >> 4` (as a float), and `FUN_002F9F58(z) =
  (A z + B) / (C z)` with A, C, B the GS window matrix's z terms at `0x520A68`, `0x520A6C`, `0x520A78` (the matrix at
  `0x520A40`: x / y scale 426.54 / -426.54, centre 320 / 224, A 166,850, C 1, B 1,849,800 at 4:3 in game) - the same
  mapping as the world's (a flame 3-9 m from the camera: 0.0014-0.0027 of Z24; the ground behind it 0.0016-0.0021,
  the player at ~2 m 0.0034-0.0044). CDraw's near / far (`0x4CD130` / `0x4CD12C`: 0.1125 / 2000) are the camera's.
  So the sprites' depth is right, and on a PS2 they pass in front of what is behind them.
- **Why they vanished**: the D3D11 GS pixel shader wrote its depth (SV_Depth = the interpolated vertex depth, a
  leftover of the GL renderer's precision fix, PERFORMANCE.md 26). With it, these draws failed wherever the shader
  took a bilinear path - the four loads, Gather, the anisotropic hardware sampler - and showed with the nearest and the
  full shader (`PS2X_HW_SPECIALISE=0`), all writing the same SV_Depth from the same interpolant; drawn opaque without
  depth they come out textured. Neither the depth values (forced 0.5 nearer: still gone) nor the post effects
  (`PS2X_HW_POSTFX=0`), MSAA, anisotropy, fog or the aspect changed it; `PS2X_HW_SHADER_DEPTH=0` (the rasterised
  depth, what Vulkan already used) drew the fire everywhere. Why only the bilinear variants lose it with SV_Depth is
  not understood (most likely the driver: an AMD Radeon RX 7700 XT here); the fix does not depend on it.
- **Fixed**: the rasterised depth on D3D11 too (`gs_hw.cpp` `shaderDepth`, PS2RECOMP_PATCHES.md) - the same value with
  a 0..1 depth range, and early depth rejection back on. `fire_check.py` passes: fire pixels in the best frame from
  0-12 to hundreds (flame-thrower) and thousands (molotov).

## 7. Textures: `.XTX`

**Host replacement identity (2026-10-31).** PCSX2 packs match the GS texture as it is sampled, rather than the RAM
raster pointer or the upload format. Tested against the user's SLUS-21590 pack: the pixel-block XXH3 hash plus the
expanded current-CLUT hash and TEX0 PSM/TW/TH identify 25 pack files over Fort Baxter and four city spots. This
includes the game's T4 data sent as CT16 by `FUN_002F62B8` / `FUN_003298D8` (section 6i): hashing the transfer's
linear bytes would not identify the sampled texture. The host implementation materializes queued uploads on a
candidate cache miss, then hashes the same GS blocks PCSX2 hashes; the CLUT is read from the CPU backend's loaded
palette. For nearest/bilinear sampling, the original GS dimensions and region limits still govern wrapping while
the host samples replacement pixels at their own resolution. See `docs/TEXTURE_PACKS.md`.

**Automation readiness (2026-10-31).** During the title flyover `CGame::currArea` (`0x489F7C`) is already 0 and
frames advance; that alone is not evidence of a playable game. Found when the first texture-pack city captures
showed the flyover without a HUD and no matching HUD/radar textures. `rig.Game.new_game()` now also requires the
`player` control command to return a non-null player ped (`CWorld::Players[0].m_pPed`, `0x4E4910`, as already read by
`src/debug/rig.cpp`) before reporting success, and keeps pressing Cross while
waiting. Earlier empty-card benchmarks using only the area check may have measured the title camera.

Confirmed from `LOADSC*.XTX` (263,372 bytes each, 512x512 8-bit):
- `0x00` magic `"xet\0"`; `0x08` file size; `0x0C` / `0x10` offset of the trailing relocation table
  (0x404B0); `0x60` name (`"loadsc0"`).
- Pixels at **0xB0**: 512x512 linear PSMT8 indices (no swizzle). CLUT at **0x400B0**: 256 x RGBA8888,
  alpha 0x80 = opaque, already in **CSM1 order** (index bits 3 and 4 swapped).
- The game uploads both unchanged: CLUT to CBP 13584 (16x16 CT32), pixels to TBP 8960 (TBW 8, T8),
  through a VIF1 DIRECT (BITBLTBUF / TRXPOS / TRXREG / TRXDIR + IMAGE GIF tag) and a REF tag whose
  upper half is `NOP; DIRECTHL n` (this split exposed the upstream VIF bug in PS2RECOMP_PATCHES.md).
- The legal screen (copyright text + logo) is also a 512x512 T8 upload.

## 8. Data loaded at boot

- `PS2/GAME.DTZ` = zlib -> 6.6 MB `GTAG` v1 memory image (header: size, then table offsets), loaded
  at `0x016FE800` and relocated by `FUN_00324A50`; main copies ~50 global pointers out of it.
- `MAINLA.LVZ` / `BEACH.LVZ` / `MALL.LVZ` are zlib streams of a `WRLD` image (header `"DLRW"`);
  MAINLA inflates to 0x54F358 bytes at `0x756680` (read into `0xA8FE00`, inflated by
  `FUN_0021FA38`, relocated by `FUN_00324A50`). Sectors then stream from the matching `.IMG`
  through CDSTREAM.
- **zlib 1.2.2** (version string at file offset 0x3A6250; message strings "invalid distance too far back" ...),
  built with the EE's 64-bit `long`: `z_stream` = next_in 0, avail_in 4, total_in 8 (8 bytes), next_out 0x10,
  avail_out 0x14, total_out 0x18, msg 0x20, state 0x24; `inflate_state` = mode 0, ..., wsize 0x2C, whave 0x30,
  write 0x34, window 0x38, hold 0x40 (8 bytes), bits 0x48, lencode 0x58, distcode 0x5C, lenbits 0x60, distbits
  0x64 (mode BAD = 27). **FUN_00184428 = inflate_fast** (OFF = 1, pre-increment pointers) - read from its loads,
  2026-10-25. It is the busiest guest function while streaming: ~12% of guest time in a standing profile window.
  Native since 2026-10-26 (PERFORMANCE.md 14). The game's whole-file inflates (GAME.DTZ, the LVZ images) pass
  `avail_in` ~16 MB - "enough" - so the input buffer is not known by its length; one stream decodes ~8 KB chunks
  into a ring at 0x4D3E80 / 0x4D6080 / 0x4D8280 and relocates pointers in each chunk in place right after.
  With the native inflate the boot's loading screen shows 0.7-1.2 s (1.2-1.6 s before; `tools/scenes.py` shoots it
  at 1.0).

## 9. Streaming and sound (CDSTREAM.IRX)

- CDSTREAM.IRX is Rockstar's IOP driver for **both** disc streaming and sound. Three SIF RPC servers
  (IOP threads):
  - `0x65686577` - stream reads. EE client `0x505B48`: fn 1 = read `{channel, EE dest, LSN, -bytes}`
    (size negated), fn 2 = poll (`0xFA` busy, `0` done). The first request is GAME.DTZ (0x217E70 bytes).
    EE side (2026-10-24): **FUN_00221818(channel, dest, lsn, size)** - size in sectors or -bytes - flushes the
    cache over the buffer, calls fn 1 (reply 1 = accepted), then **FUN_00221678(error word)** shows the disc error
    screens (codes 0xFB-0xFF, jump table 0x4ACFA0); **FUN_002218f0(channel)** = fn 2; **FUN_00221968()** = fn 5
    (no arguments, always 0 seen); fn 0 = init `{6}` from FUN_002215a0. Channel 5 carries most reads. Counted in
    boot + 35 s of play: fn 5 x3011, fn 2 x577, fn 1 x336. **Native since milestone 6 step 1**
    (`src/game/vcs_stream.cpp`): the three EE functions replaced, reads copied from the disc image by a host
    thread, a poll waits up to 20 ms for its read (the game polls at once and would otherwise lose a frame per
    read). 14 MB take ~5 ms; boot and LOAD_SCENE teleports as fast as through the IOP (game-bound).
  - `0x65686578` - EE client `0x4CE2C0`, bound by `FUN_0010DCE8`.
  - `0x65686579` - EE client `0x4D3D40`, bound by `FUN_0010E918`. Commands go through
    `FUN_0010E970(cmd, arg)`: buffer `0x4CE300`, the reply's first word is the result. Seen:
    `0xA0000` (count, 0x6D8 = 1752 sounds), `0xA0B00` / `0xA0C00` per-sound queries, `0xA0D00` load
    bank, `0xB0100` bank-load status (1 = loaded).
- `FUN_0010D868` = sound init (skipped if `*(0x481900) != 0`), called from `FUN_002A6330`, which then
  loads a bank and loops `while (FUN_0010DE78() != 1) DelayThread(1000)`.
- On the IOP the load handler (`0x22DC0`) builds `\AUDIO\SET%s_PS.SDT;1` with sysclib `sprintf` and
  spins `while (!sceCdSearchFile()) WaitVblankStart()`. Banks: `AUDIO/SETn/SFXnn_PS.RAW` (ADPCM) +
  `.SDT` (table).
- CDSTREAM's audio thread (entry `0x1FCE8`) waits on a semaphore signalled by libsd
  transfer-complete and SPU2 IRQ handlers (`sceSdSetTransCallback`, `sceSdSetIRQCallback`).
- The bank-load RPC handler (fn `0xA0D00`) signals the reader thread and then polls for completion
  *inside the RPC call* (the runtime's nested-RPC model needed `yieldFromCall` for this).
- Bank load: ~1.7 MB of ADPCM by IOP DMA channel 4/7 into SPU2 RAM; music and dialogue stream
  through voice loops refilled on SPU2 IRQ-address interrupts.
- `DelayThread` (`0x468318`) = `SetTimerAlarm` (EE T0, handler `0x451038`) + `WaitSema`; a fresh
  semaphore per call. libsd `sceSdInit` waits for `STATX & 0x7FF == 0` after reset; STATX bit 7
  (DMA busy) only while `ATTR & 0x30`.
- SPU2 features in use (measured 2026-10-23 with `PS2X_TRACE_SPU2=1` at Fort Baxter, on foot): no pitch
  modulation (PMON 0), no noise (NON 0). Reverb is configured *off* the way libsd does it: both cores route every
  voice to the wet path (VMIXEL/ER `0xFFFFFF`, MMIX `0x0FF0` / `0x0FFC` with the wet gates set), but the work area
  is an 8-halfword dummy at the top of sound RAM (core 0 ESA `0xEFFF8`..EEA `0xEFFFF`, core 1 `0xFFFF8`..`0xFFFFF`),
  ATTR bit 7 (effects enable) is clear (`0xC000` / `0xC041`), EVOL and vIIR are 0. Main volumes `0x3FFF` (direct).
  Gameplay peaks at Fort Baxter are low (~400-1700 of 32767); AutoDMA input is idle on foot.

- **CDSTREAM.IRX's structure** (48,540 bytes, ~6,400 MIPS I instructions; `tools/irxdis.py`, 2026-10-24): the three
  servers are registered with handlers at module offsets 0xDF4 (0x65686577, streaming), 0x36DC (0x65686578) and
  0x57C0 (0x65686579, sound effects); the sound code is ~4,800 instructions (module 0x0F00-0x5A00). It drives the
  SPU2 only through libsd - no direct SPU2 register access: SetParam x36, VoiceTrans x7 + VoiceTransStatus x8
  (bank / stream data into SPU2 RAM), SetSwitch x7 (key on / off), SetCoreAttr x6, SetAddr x5, GetParam x1 (ENVX),
  Set{Trans,IRQ}Callback, Init; cdvdman (Read / Sync / SearchFile / GetError / DiskReady / Break ...) for its own
  reads; 7 threads, 8 semaphores. libsd's import ordinals (LIBSD.IRX's order, 4 = sceSdInit) were told apart by the
  call arguments (GetParam(core | voice << 1 | 0x500 = ENVX), 36 SetParam sites).
- **What the EE asks of the IOP** (`PS2X_TRACE_RPC`, boot + 35 s of play including driving, 2026-10-24): only
  CDSTREAM's three servers. 0x65686579 (sound effects) 23,901 calls in 46 functions: `0xA0A00`-`0xA0A2B` (0xA0A07,
  0xA0A09, 0xA0A2B ~950 each), `0xA0B00` / `0xA0C00` x7722 each (per-sound queries), `0xB0000` x3016, `0xB0100`
  x1908 (bank status), `0xA0D00` x3 (bank loads), `0xA0000` once. 0x65686578: 6,595 calls - `0xB00` x3022,
  `0x80E00` x3207, `0xC00` x224, `0x80700` x120, `0x80000`-`0x80221`, `0x90000`-`0x90400` a few. No PADMAN /
  MCSERV RPCs in play (pads and the card don't go through RPC here). All nine IRX modules run as IOP code.

### 9a. The sound engine, read from CDSTREAM.IRX (native since milestone 6 step 2, 2026-10-24)

How it was read: `tools/irxdis.py --decompile` (headless Ghidra on the module as a raw MIPS image, import stubs and
exports named, `tools/ghidra/DecompileAll.java`) for the logic, the plain listing for register-level detail, and the
SPU2 write traces of the emulated IOP (`PS2X_SPU2_TRACE`) as the final word. **Ghidra drops stores to hardware
registers it thinks are dead** (a write followed by another to the same address, e.g. libsd's `ATTR &= ~0x30` before
the voice reset) and reorders stores (SPDIF: the IRX writes 0x7C8, 0x7C0, 0x7C6) - the traces caught both. Offsets
below are module offsets (CDSTREAM loads at IOP 0x1D600 on the emulated IOP); "data" is module data.

- **Replies**: both servers answer with a pointer to one result word (0x8160 effects, 0x7FD0 music); the EE asks for
  64 bytes (1444 for `0xB0000`) and only the first word means anything. Commands that return nothing leave the old
  word in place (`0xA0F00`, `0xB0000`, `0xA0200`; music `0xB00`, `0xC00`, `0xD00`, `0x100`, `0x90400`, `0x90500`).
  The request's first word is the argument; `cmd & 0xFF` is a second one.
- **Effects server 0x65686579** (handler 0x57C0): `0xA0000` init(extra sounds) -> 1; `0xA0200` shutdown (key off
  all); `0xA0A00+v` voice v playing (ENVX != 0); `0xA0B00` / `0xA0C00` a sound's length in samples (bytes x 7 / 4) /
  sample rate; `0xA0D00` request bank (arg >> 8 = bank, arg & 0xFF = 0xFF to load); `0xA0F00` effects volume
  (signed byte, 0x7F); `0xB0000` the voice block; `0xB0100` bank status (1 loaded, 2 loading); `0xB0200` / `0xB0300`
  is a special sound ready / load one (arg >> 8 = sound, & 0xFF = slot type).
- **The game decides by `0xA0A00+v`** whether an effect voice is still sounding (ENVX != 0, ~150 queries a second
  while driving): an engine loop it believes stopped is keyed on again. A native port that read the wrong voice's
  envelope (`index | core << 1` for libsd's `core | index << 1`) made the bike's engine restart every frame (a short
  sample looping fast, sometimes silence at speed) and cars play rev samples while slowing (found 2026-10-24 by the
  user; 676 key-ons vs the IOP's 156 over the same ride, 91 back-to-back restarts vs 0).
- **Vehicle skid sounds: `FUN_001BA600`** (cAudioManager::ProcessVehicleSkidding; found 2026-10-24 through the float
  constants 13000.0 / 35000.0 of the pitch formula; a1 = cVehicleParams: +0 type, +8 squared distance, +0xC vehicle,
  +0x10 transmission, +0x18 speed along the vehicle's forward axis). Vehicle types 0 / 9 have four wheels (count of
  wheels on the ground +0x7FC, gas pedal +0x800, wheel states +0x804), 6 / 8 (bikes) two (+0x668 / +0x66C / +0x670);
  wheel states 0 normal, 1 spinning, 2 skidding, 3 locked. Per wheel: drive type (transmission +0x5C, 'F' / 'R' /
  '4') picks `FUN_001BA9A8` for driven wheels (spinning: (gas - 0.4) x 1.667; locked / skidding: max of that and
  |speed| / max velocity (transmission +0x50)) or `FUN_001BAB18` for the others (skidding only), both raised to
  |turn speed z| x 20 (vehicle +0x78), capped at 1. The largest x 55 is the volume before distance (40 m). Sound by
  the surface under the vehicle (+0x137 - 2, jump table 0x4A7EA0): grass / hedge sound 239 (a hiss) at
  35000 + 13000 x skid, 1/4 volume; gravel / sand sound 175 at 5000 + 3000 x skid; everything else (tarmac) **sound
  260, the screech, at 11000 + 5000 x skid, +2000 for bikes**. A bike at 120 km/h held on R1 and turned: front wheel
  skidding, rear locked, screech at volume ~36 (gas held) / ~25 (gas off), pitch 16,000-18,000, as long as the skid
  lasts (`VCS_SKID_LOG=1` logs the inputs for the player's vehicle).
- **Voices and cores**: the effect voices 0-23 are core 0, 24-43 core 1 voices 0-19 (music: core 1 voices 22 / 23); the
  game hands out voices in turn (a machine-gun burst walks 12/13, 14/15, ... - a left / right pair per shot). Core 0
  reaches the output only through core 1's external input, scaled by core 1's AVOL, which sceSdInit sets to 0x7FFF
  (BVOL, the AutoDMA input, 0). An SPU2 emulation that gets AVOL wrong silences ~half the game's sound effects at
  random (fixed 2026-10-26, PROGRESS.md).
- **Init** (0x3B10): `sceSdInit(0)`, SPDIF mode 0x800, main volumes 0 then 0x3FFF, a 128 KB IOP buffer, then every
  bank's `\AUDIO\SET<b/10>\SFX<b>_PS.SDT` read into one sound table: **12 bytes per sound** - offset in the bank's
  .RAW, bytes, sample rate - indexed from bank b's first sound (68 constants written by 0x3B10 to data 0x9FC0; last
  bank sized by the init argument: 1). ADSR of all 48 voices 0x0F0F / 0x1FC7.
- **The voice block** (`0xB0000`, 1444 bytes, sent every frame): words 3-4 key-on mask per core, 5-6 key-off mask
  per core, then eight arrays of 44 words from word 9: sound, bank, -, -, rate (Hz), pan (0-127), volume, -. Voices
  0-23 = core 0, 24-43 = core 1 voices 0-19 (core 1 voices 22 / 23 are the music). Applied by the thread 0x4B8C:
  key off; per voice: start address = sound offset + the bank's SPU2 address + 0x30 (past the VAG header) - a voice
  whose bank is not loaded is not keyed on -, pitch = rate x 4096 / 48000, volume L / R = min(2 x vol x (127 - pan) /
  127, vol) / min(2 x vol x pan / 127, vol), times the effects volume; then key on (KOFF + KON of the same mask).
- **Banks** (68; SPU2 address table in data 0x8000): 0-1 at 0x79810, 2 = specials at 0x21810, 3-0x27 "level" banks
  sharing 0xD010 (loading one unloads the others), 0x28-0x3C "building" banks in two slots at 0x21810 (the quieter
  slot is replaced; the loader prints "BUILDING BANKS NOT USED"), 0x3D-0x42 one area at 0x6F010, 0x43 specials.
  Loading (thread 0x50DC): the .RAW through the 128 KB buffer, `sceSdVoiceTrans` on core 0 in 128 KB pieces.
- **Special sounds** (speech, mission audio): sounds 0x125-0x16BA come out of bank 2's .RAW, sounds above 0x1750 out
  of bank 0x43's, one at a time, into a slot by type: 0-4 fixed (0x3A810, 0x45010, 0x4F810, 0x5A010, 0x64810), 5-7
  at 0x74010, 0xFF a ring of 5 at 0x21810 + n x 0x5000 (voices find a ring sound among the last two loaded).
- **Music server 0x65686578** (handler 0x36DC, thread 0x26E8): `0x80000` init (finds the 112 streams - names at
  data 0x6410, 40 bytes apart: radio stations, ambience, news, cutscene audio; SLES builds swap in WAVE_EU.VB) ->
  the thread id; `0x80200+n` open stream n at arg ms; `0x80700` stop; `0x80E00` state; `0x90000` a stream's length in
  ms (bytes x 7 / 256); `0x90100` position in ms; `0x90200+n` preload n held (cutscenes) / `0x90300` start it;
  `0x90400` mono; `0x90500` loop; `0xB00` volume at the next step; `0xC00` volume now (left voice gets the high
  half, right voice the low half); `0xD00+core` a core's main volume.
- **Streams** (.VB): stereo ADPCM at 32 kHz, interleaved in 8 KB chunks (L R L R ...). Two 256 KB IOP buffers read
  from disc; two 16 KB SPU2 rings at 0x5010 (left) / 0x9010 (right) played by core 1 voices 22 / 23 as loops. The
  IOP marks the ADPCM block flags in each 8 KB chunk before sending it (first block 6 = loop start or 2, last block
  of the ring 3 = jump back). The IRQ address sits at the start of the half that just started playing: the IRQ
  (every 448 ms) refills the other half - left, then right on its DMA completion, then the IRQ moves. A start
  position in ms maps to a 16 KB-aligned offset piecewise over eighths of the file (0x1524).
- **The music thread** is a state machine driven by one semaphore (max count 1) signalled by commands, DMA
  completions (`sceSdSetTransCallback`) and the SPU2 IRQ (`sceSdSetIRQCallback`): 0x80200 open -> fill both disc
  buffers -> 0x1000 / 0x7000 / 0x2000 / 0x8000 send the four ring halves -> 0x3000 IRQ armed -> 0x4000 preloaded ->
  0x5000 playing (keys on, volume, IRQ enable) -> 0x6000 stop. A stop request takes effect at the next step.
- **Timing the game relies on**: after opening a radio station the EE sends the station's volume 3 samples later;
  on the IOP the preload's four 8 KB DMAs take ~22 samples, so the volume is in before key-on. A preload done
  instantly started the radio at the old volume (0) for one refill period (found by the replay comparison).
- **The level swap waits for the music to stop** (found 2026-11-02 with `tools/cruise.py`: the longest hitches of a
  drive across the bay, 150-410 ms, all game-thread idle; `PS2X_WAIT_LOG`, then `VCS_TRACE_DELAY=1`, which also logs
  the music manager's mode changes now). **FUN_003C6D08** swaps the level when the one wanted (`*0x488C60`) differs from
  the current (`0x4CD118`, CGame::currLevel: Beach / Mainland): FUN_002FA908(0x520D60, 2) - the music manager
  FUN_001891A0(0x4E41E0, mode); mode 2 stops the stream (0x80700) and then loops `while (state poll 0x80E00 != 0)
  DelayThread(1000)` (return address 0x1892B0) -, the level's sound banks, **FUN_0023BAF0** (the level loader, .LVZ),
  then mode 1 (music back on). The IRX's stop handler (0x200C) only sets the request flag; the music thread sees it at
  its next step, which for a playing stream is the next refill IRQ - up to 448 ms. So the whole game stood until then,
  on the IOP as well (and each `DelayThread(1000)` took 3-6 ms of host time here, so ~100 calls per stop). The native
  driver now runs that step at once for a stop while playing (`VCS_MUSIC_STOP_WAIT=1` = at the IRQ; the sound replay
  comparison keeps the IRQ timing): the swap went from 150-410 ms to the load's own ~40 ms game frame. Other
  FUN_002FA908 callers: cutscenes and menus (modes 1 / 3; mode 2 at 0x3C6DA8 is the level swap).
- **libsd** (LIBSD.IRX, `src/sound/libsd.cpp`): entries are `core | voice << 1 | param << 8` (+0x80 for per-core
  volumes, core stride 0x28 instead of 0x400); the register table is data 0x4CA0. `sceSdSetCoreAttr` 2 / 4 / 6 =
  ATTR bit 7 / 6 / 14, 8 = noise clock, 10 = SPDIF. VoiceTrans mode 0 = TSA, ATTR DMA-write mode (0x20), DMA; the
  DMA interrupt clears ATTR & 0x30 and calls the channel's callback. The SPU2 IRQ handler clears the IRQ-enable bit
  of the core that hit, then calls the IRQ callback. `sceSdInit(0)` resets the hardware, writes a silent looping
  block at 0x5000 through the data port, points every voice at it, keys all on and off.

### 9b. Running without the IOP (milestone 6 step 3, 2026-10-24)

- With streaming and sound native the EE's only remaining IOP call is the stream server's fn 0 at boot (CDSTREAM
  0x0000: read queues + CD thread; the IOP's own sound code reads through its channels 2 and 3, so with the emulated
  IOP on this call must still reach it). `retireIop` (`src/game/vcs_stream.cpp`) answers it natively and switches
  the emulated IOP off: no IRX module is loaded or run (loads report success), no IOP time is emulated.
- Pads and the memory card never went through the IOP here (EE-side HLE in the runtime), nor did the EE's own
  `cdrom0:` file access. IOPRP300.IMG is never needed.
- Boot to gameplay from the rig card: 4.9-6.1 s with the IOP off, on with the native paths, or fully emulated (the
  game is the limit). IOP time per frame: 0.2 ms off, 0.3-0.4 ms emulated.

### 9c. The game's music / effects levels (2026-10-28)

How it was found: RAM dumps (rig `dump`) before and after steps of the pause menu's sliders (Audio page: Music
Volume, SFX Volume - 8 bars each; no dialogue level; then Speaker Setup and Radio Station), then a write watch
(`VCS_WATCH=4cda0c,4cda0e`, `VCS_WATCH_STACK=1`) from boot through a save load and the sliders, then the generated C++
of every writer and caller.
- **cSampleManager at 0x4CDA08**: +4 effects level, +5 music level (u8, 0..127; +6 / +7 / +8 = 0x7F, likely the fade
  levels). Its constructor FUN_0010D718 sets 0x70 / 0x50 (the defaults: 7 and 5 bars). The setters are one store each:
  FUN_0010E2B8 (+4) / FUN_0010E2C0 (+5), reached through DMAudio (0x514A90) FUN_002A6C10 / FUN_002A6C38. The sound
  code reads them (0x1C7110 effects, 0x189B54 / 0x2AE9B8 music); the menu draws its bars from them (FUN_002AE958).
  A slider step is 16; up from 112 the setter's signed-byte test turns 128 into 127.
- **The menu's prefs**: an object at `*0x48F3D0` (heap, 0x1FB5800 in the rig run) with +0x38 effects, +0x3C music (u32),
  getters FUN_002AF320 / FUN_002AF310, setters FUN_002AF328 / FUN_002AF318. Saves carry these.
- **The menu's setters FUN_002FA538 (effects) / FUN_002FA5B0 (music)** (a0 = 0x520D60, a1 = level; a negative byte
  becomes 0x7F) call DMAudio's setter, then the prefs setter. Callers: the Audio page (FUN_002AEF38: return addresses
  0x2AEF84 music, 0x2AEF9C effects), the save load FUN_003ADAE0 at its end (0x3ADD88 / 0x3ADDB0, the prefs' values
  just read from the save by FUN_003ACEF0; at boot too), and FUN_003163D8 (from FUN_003157E0), which sets the defaults
  0x50 / 0x70. The effects RPC 0xA0F00 is not used for this (no call in play): the levels act on the EE side.
- `src/game/vcs_volume.cpp` makes them the settings Audio > Music / Sound effects volume: writes both places at the
  frame kick, takes the Audio page's changes back, and gives every other setter call the player's level (a save load
  logs `[volume] the game sets the music level 80 (ra=0x3add90): the player's 32 instead`).
- **Speech** has no level of its own: it plays on effect voices from banks 2 and 0x43 (9a, special sounds) under the
  effects level, while speech in cutscenes and on the radio is part of their streams. The voice block sends a voice's
  volume only when it changes, so a new sound can start on a voice with its old volume. People near an explosion
  scream (speech sounds). A Voices volume that scaled those voices in the native driver was tried on 2026-10-28 and
  removed (DECISIONS.md).

### 9d. GTA3PS2.IMG: the model archive and its directory (2026-11-03)

Found for the mod loader's game files (docs/MODS.md "Game files"), with the rig reading the tables in a running game
(`tools/img.py table`) and the disc image's own file list (`tools/vcs_disc.py ls`).

- **The disc's file table** (built at boot, section 3): `0x5038A8`, 16 bytes a file - `{name ptr, 0, LSN, bytes}` -
  count at `0x486E8C` (341 files). GTA3PS2.IMG is at LSN 870012, 54,030,336 bytes (26,382 sectors); BEACH / MAINLA /
  MALL.IMG (the world, streamed by the level code, 6b) and MOCAPPS2.IMG (with its MOCAPPS2.DIR) are other files.
- **No directory on the disc**: GTA3PS2.IMG's is CStreaming's info, from GAME.DTZ - `*(*(0x489FF8) + 200)`, **0x18
  bytes a resource**, indexed like the model ids (`+0xC` byte: the load state, 1 = loaded; `+0x10` the offset and
  `+0x14` the size **in sectors inside GTA3PS2.IMG**, `0xFFFFFFFF` = no data; links and `0xAAAA` fill elsewhere).
  8482 records (the array ends there), 1580 with data. The game streams an entry with `FUN_00221818` at
  `LSN = 870012 + offset`, `size` sectors - one read per entry, into a buffer of the entry's size.
- **Names are keys**: a model info (`*(0x486518)[id]`) holds at **+0x8 the key of its name = ~CRC32 of the upper-case
  name** (CKeyGen; `ADMIRAL` -> `0xAB691458`). The names themselves are not kept; a vehicle's text key (+0x158) is its
  model name when the keys match - 75 vehicles named that way (`admiral` 171: offset 7242, 37 sectors; `cheetah` 172:
  7284, 30).
- **Entries are relocatable**: the CHEETAH's entry served as the ADMIRAL's (smaller: the rest of the buffer zeros)
  loads and draws a Cheetah under the name Admiral, no crash (`tools/gamefiles_check.py`) - a model resource carries
  what the game needs to place it anywhere in the streaming memory.
- **GTA3PS2.IMG is read as an open file**: `FUN_00220EC8(file, ?, bytes)` reads from the open-file records at
  `0x5058A8` (0x14 bytes: {?, LSN, bytes, position, ?}) at `LSN + position / 2048` and moves the position on, held to
  the file's size - so an entry's offset past the archive's end is cut back to it (found when a larger mod entry,
  moved past the disc, was never read: 2026-11-03). Raising the archive's size there (and in the file table) lets
  the game read it.
- **Loose files go through the same stream**: `AMERICAN.GXT` / `ENGLISH.GXT` (and `MAIN.SCM`) are read by
  `FUN_00221818` at their file-table LSN, `bytes` long - a mod's copy laid over those sectors is what the game loads
  (a GXT with "Admiral" written "ADMIRAL" made the game's text for the key ADMIRAL read "ADMIRAL", 2026-11-03).

- **A vehicle's entry** (`admiral.mdl`, 75,776 bytes): a relocatable resource - magic `ldm\0` ("mdl" reversed), then
  sizes and the relocation table's offset (`0x12095`, `0x1182c`, `0x11880`, 0x1e2 ...: the LCS / VCS resource header,
  to be mapped). Its strings are **the frame names of a PC VC `.dff`**: `chassis_dummy`, `chassis_hi`, `chassis_vlo`,
  `wheel_lf/rf/lb/rb_dummy`, `door_lf/rf/lr/rr_dummy`, `bonnet_dummy`, `boot_dummy`, `bump_front/rear_dummy`,
  `windscreen_dummy`, `wing_lf/rf_dummy`, `car_parts`; and its textures' names: `admiral868bit_sl` (the body),
  `xv_vehiclegeneric_sl`, `xv_vehiclegrunge_sl`, `xv_scratch_sl`, `xv_vcs_licenseplates_cs`, `xv_vc_crackedglassa_sl`,
  `xv_vehiclesteering_sl` (found 2026-11-03 with `tools/img.py extract admiral` and a strings pass).
- **The `ldm` / `tex` resource layout** (2026-11-03, `tools/mdlinfo.py`, `tools/dff2mdl.py`, `tools/txd2xtx.py`;
  aap's storiesconv as a reference): a 0x20 header `{ident, shrink, fileSize, dataSize, relocTab, numRelocs,
  globalTab, u16 classes, u16 functions}`; pointers are file offsets, each listed in the relocation table. The game
  keeps `[0, dataSize)`; what follows is the global table, the relocation table and - **after the relocation table** -
  strings read once at load: the materials' texture names (`admiral868bit_sl` at 0x12084 of 0x12095). A rewritten file
  has to move that tail as a whole and every pointer into it.
- **A vehicle `ldm`**: at 0x20 `{element group, extras count, extras, paint}`: the **first-colour paint list** at 0x2C
  (30 material pointers, null-ended when fewer) and the **second-colour list** at 0xA4 (25); the game colours those
  materials with the car's colours (the ADMIRAL lists 14 materials, all first colour; the COMET 15 + 2). Paint
  materials use `xv_vehiclegrunge_sl` / `xv_scratch_sl` (the dirt and scratches over the colour) or the car's own
  texture. Node `RslNode`: modelling matrix +0x10, child +0x90, next +0x94, name +0xA8 (size 0xB0); element: node +4,
  geometry +0x14, group link +0x1C; an intact and a damaged element per `X_dummy`. Geometry: a 32-byte RslGeometry
  (materials list +0x0C, count +0x10), the 0x40 PS2 header, 0x30-byte meshes, the DMA data (section 6d's batches).
  Material (16 bytes): `{texture name, RGBA, 2, matfx}` - a null name is drawn in its colour; matfx points at the
  effect data (paint's reflection).
- **A vehicle's elements, more** (2026-11-03, the player's review of converted VC cars): an `X_dummy` holds its
  intact element(s) - **object flags (+2) bit 4 set** - and its damaged one (bit clear, component id +0x2A one higher),
  in either order in the group's list; a van's door dummy holds two intact pieces (door, window) and one damaged.
  Extras are a list of their own at 0x28 (count 0x24), elements whose nodes hang free (no parent: root-relative), not in
  the group. At load `FUN_00394438` gives each element a render callback by its node's name: `moving_rotor` ->
  `0x36e100`, `moving_rotor2` -> `0x36e1f8` (drawn translucent, sorted by distance), `extra*` and the rest by whether a
  material has alpha. A helicopter's `moving_rotor` is a blur disc (texture `rotor` or `black64a`, in the vehicle's own
  dictionary) faded in with the rotor's speed, `static_rotor` the blades. The 0x30-byte mesh header ends with the
  mesh's **int16 bounding box** in the geometry's quantised space (min xyz, max xyz, two units out), the geometry header
  (+0x1C) with the union; mesh +0x18 is an int the storiesconv notes tie to the raster (0 in 169 of the game's meshes).
  Every vehicle's TXD has the vehicle's name; `boxvil`, `enforce`, `fbiran`, `policem`, `sabretb`, `washin` are VC's
  `boxville`, `enforcer`, `fbiranch`, `police`, `sabretur`, `washing`. The wheel models `wheel_rim` .. `wheel_smallcar`
  (320-329, VC's names) have no streaming data: resident, from GAME.DTZ.
- **Texture dictionaries** are archive entries too (ident `tex\0`, 823 of them): **TXD slot `i` is streaming entry
  `*(*(0x489FF8) + 8) + i`** (7534 on this disc; then `+0xC` 8365, `+0x10` 8392, `+0x14` 8482 the total). A model
  info's TXD slot is the halfword at +0x1E (ADMIRAL 167 -> entry 7701, COMET 262 -> 7796). The TXD store's pool is
  `*(0x489ECC)` = `{items, flags, size 2000, ...}` with 0x1C-byte items `{RslTexList *, refs, name[20]}` - a vehicle's
  TXD carries the vehicle's name (found in a RAM dump: the pool header holds the name "TexList"). The resource: at
  0x20 the RslTexList `{object (type 6), texturesInDict {next, prev}, lInInstance}`; per texture `{raster, dict,
  lInDictionary {next, prev}, name[32], mask[32]}`; the raster `{0, upload word, texels, flags}` with flags = log2 w |
  log2 h << 6 | depth << 12 | levels << 20 | swizzle << 24 and the upload word `0x450000 | row bytes` (4 bit) or
  `0x250000 | row bytes` (8 bit). The texels are linear, the palette after them (RGBA, alpha 0..0x80; 8 bit in CSM1
  order: index bits 3 and 4 swapped). **Every texture on the disc is 4 or 8 bit, one level, unswizzled, 8..256
  pixels** (a survey of all 823 dictionaries).

### 9e. Memory: the main heap and the streaming heap (2026-11-03)

Found when VC car mods (4-5x the size of VCS's cars) made `LOAD_ALL_MODELS_NOW` spin after five of them (11e: the
game's own cars do that after 28); read with headless Ghidra (`tools/eedecomp.py`) and a RAM dump taken in game.

- **One heap type** (`FUN_002bb768(heap, base, size, granularity)` builds it): `+0x0` size, `+0x4` the
  out-of-memory callback (called by the large-block allocator `FUN_002bc7e8` when the free list `+0x8` has no fit),
  `+0xC` first block, `+0x10` end, `+0x14` bytes in use, `+0x24..` 64 pools for blocks up to 256 bytes
  (`FUN_002bc730(heap, size)`: pools, else `FUN_002bc7e8`). Blocks are `{size, flags}` headers (flags < 0: free);
  `FUN_002bc268` prints a "HEAP DUMP" (size / in use / free / wastage / pools) - debug code, never called.
- **The main heap** `0x51D6A0` (`FUN_002bb658` returns it, built once by `FUN_002bb6a0` ->
  `FUN_002bb6f8(0x74DF54, 0x18920AC)`, granularity 0x80000): everything from the end of the executable's data
  (`0x74DF54`) to `0x1FE0000` - 24.57 MB. In game (Vice Point, after a load) **23.03 MB in use**: about 1.5 MB slack.
  Out of memory it calls `FUN_0021c468`, which removes vehicles and peds the player does not see and retries.
- **The streaming heap** `0x51F420`: `FUN_002e5688(heap)` takes **0x567000 bytes (5.4 MB)** from the main heap
  (`FUN_002bd178`, kept at `heap + 0x130`) and builds the heap in it - base `0x1180B78` in the dump, its blocks
  between it and `0x16E77F8`. **Every streamed resource lives here**: `FUN_003c7ed8` (CStreaming's request) makes room
  (`FUN_003cb760(streaming, bytes)`), then `FUN_003c6478` allocates the entry's size from `0x51F420`
  (`FUN_002bc730`) and the read goes straight into it (`FUN_002213d0`). Out of memory it calls `FUN_0021c300`:
  CStreaming removes what it can (`FUN_0021c270` -> `FUN_003cba50(0)`), the heap is **defragmented**
  (`FUN_002bcfc0(0x51F420)`: resources are relocatable, 9d), and the allocation is tried again. In the dump: 89 blocks,
  4.33 MB in use, 1.07 MB free.
- So VCS keeps about 5.4 MB of models and textures in memory at once (a VCS car with its textures: 30-120 KB, median
  76 KB). Larger resources mean fewer at once; when a script forces loads the game cannot meet it spins
  (`LOAD_ALL_MODELS_NOW`). Within the PS2's 32 MB there is no room to grow the streaming heap by much (the main heap
  has 1.5 MB slack): the plan is more EE memory for the port (FORK.md T10).
- **CStreaming's cap of 51** (read in `FUN_003c7ed8`): before a request it counts the resources on its loaded list
  (`streaming + 0xD8`); at `0x33` or more it calls `FUN_003c9de8(streaming, 0x83)` (call at `0x3C8030`, return
  `0x3C8038`), which removes the least used one even when the heap has room. The other caller is `FUN_003c9f10`
  (return `0x3CA0AC`), the make-room loop: that one is about bytes and stays.
- **In the port (T10, 2026-11-03).** The host holds 64 MB of EE RAM (`PS2_RAM_SIZE`); the game still sees 32 MB
  (`GetMemorySize`, the default thread stack top: `PS2_GUEST_RAM_SIZE`), so its own layout is the PS2's. Game >
  Memory > Streaming memory (`memory.streaming`, `VCS_STREAMING_MB`; src/game/vcs_memory.cpp) replaces `FUN_002e5688`:
  the streaming heap is built at `0x2000000` (past the PS2's RAM) with 12 / 20 / 30 MB - heap `+0x130` = that base,
  `+0x12C` = 1 as the original leaves them, then a tail call to `FUN_002bb768` - and the cap of 51 is skipped for the
  request's caller only (`FUN_003c9de8` with `$ra` = `0x3C8038` returns 0). PS2 (5.4 MB) leaves both alone.
  Measured with `rig.py memstate` (Vice Point, 30 MB): the main heap's use drops from 24.2 to 18.6 MB (the 5.4 MB it
  no longer gives), the streaming heap holds ~6 MB (5.5 MB at the PS2's size); the game only keeps what it asks for, so
  more room shows as more car models in traffic (fewer repeats), not as more world detail - that is the areas (9f).

### 9f. The world's streamed areas (2026-11-03)

Read with `tools/eedecomp.py` (`FUN_0023c820`, `FUN_002415b0`, `FUN_002417d0`, `FUN_0023d740`) and RAM in game, for
the pop-in while driving (FORK.md T2).

- The level object (`*(0x48F4F8)` = `0x750340`) streams the high-detail geometry of **areas of 3 x 3 hex cells**
  (cells 125 x 108.25 units) into **four slots** of 0xA0 bytes at `level + 0x2C`, their buffers in
  `0xCB7A80..0x112A660` (~4.5 MB, inside the main heap: not the streaming heap). `+0x248` is the area being drawn,
  `+0x250` the next, `+0x258` another (the words after each, `+0x24C` / `+0x254` / `+0x25C`, their pairs).
- `FUN_0023c820` every frame picks the next area from a predicted point: camera + front x 50 + velocity x 150.
  `FUN_0023c188` chooses the area for a position.
- `FUN_002415b0(level, cell x, cell y)` requests an area; `FUN_002417d0(level, piece)` reads one of its pieces
  asynchronously (`FUN_002213d0`, priority 500), whose callback `FUN_00241440` -> `FUN_00241278` takes the piece and
  starts the next (`level + 0x270` = 4 while reading). `FUN_00242078` says whether an area is in; `FUN_0023d740`
  swaps the next area in once the camera is nearer it than the current one.
- The sectors' draw (`FUN_00240ed8` / `FUN_002409c8`) has no distance LOD: what is in the slots is drawn, the rest of
  the city is the level's low-detail image. The "70 / FOV" scale in `FUN_003fb610` is the sprites', not a LOD.
- `VCS_TRACE_AREAS=1` (src/game/vcs_world.cpp) logs every request, piece read / done and swap with its time.
  Measured on the cruise route (144 km/h over the roofs, the player's mods, 30 MB streaming memory;
  `build/cruise/t10_areas/game.log`): the request names a new cell every ~2.7 s (one cell per 108 m), and is repeated
  every frame until the area is in; 107 cell changes, an area in **~20 ms** after its request (p50; p90 200 ms: behind
  the models' reads), a piece 7 ms (p90 12 ms); the swap comes 1.1 s (p50, p90 2.2 s) after the request - by design,
  when the camera is nearer the new area. So reading is not what makes the pop-in: the detailed city reaches as far
  as the areas in the slots cover, the rest is the low-detail image. Further detail needs more slots (their buffers,
  the chooser) or another source of detail (FORK.md T2). The trace's own logging costs frames (~160 lines a second):
  not for timing runs.

### 9g. Weapon models and dictionaries (2026-11-03)

Read from the extracted entries (`tools/mdlinfo.py`, `tools/txd2xtx.py info`) for PC weapon mods (FORK.md T11).

- **Weapon model infos** are type 4 (byte `+0x10`; vehicles 6), models 330-369 (339 machete, 341 chnsaw, 343 teargas,
  344 molotov, 346 beretta, 347 python, 348 ak47, 349 chromegun, 350 shotgspa, 351 buddyshot, 352 m16, 353 skorpion,
  354 uzi, 355 ingramsl, 356 mplng, 357 sniper, 358 laser, 359 rocketla, 360 flame, 361 m249, 362 minigun). The
  trainer gives them as weapon types (src/game/vcs_trainer.cpp; type 25 holds model 354 `uzi`, 24 model 353).
- **The minigun is two models**: 362 `minigun` (the body) and 366 `minigun2` (the barrels, in their own space,
  `x` -0.46..0.02: the game places and turns them while firing), as in VC; SA keeps both as parts of one `.dff`.
- **The model** (`ldm`): at 0x20 one element (object type 1, flags `0x0004AA01`), not a vehicle's element group (type
  2); its node has no name, parent or child. One geometry, 140-593 triangles, one or two materials (`gun_uzi3`,
  `iz_gun_beretta`). Its file's tail is not a vehicle's order: the relocation table comes right after the data, the
  global table (one entry) after it, strings on both sides (`molotov`: `xv_chromenv` before the relocation table,
  `iz_gun_molotov` after the global table); the header's `globalTab` > `relocTab`.
- **The dictionary** (the TXD of the model's name) holds the weapon's textures and its **HUD icon**, the texture named
  like the model (`uzi`: `uzi` 64 x 64 4-bit + `gun_uzi3` 32 x 32): the HUD finds the icon by the model's name in its
  dictionary, as VC does - with VC's `.txd` converted as it came, VC's icon (and its look) replaced VCS's. Shown by
  the conversion: VC's texture renamed (`uzi_pc`) and VCS's kept, the HUD draws VCS's icon.
- **VCS holds its weapons elsewhere than VC** (found when the player saw VC mods misplaced in Vic's hand). VCS's
  weapon models are VC's own meshes (VC's `gun_uzi2` / `gun_chainsaw2` textures are VCS's `gun_uzi3` /
  `iz_gun_chainsaw2`; VC's originals fitted onto VCS's leave 2-6 mm), each moved in the hand's space and turned:
  4-14 cm along the barrel (+x) and up to 16 cm along z, 2-10 degrees for guns, 15-30 for melee weapons, the
  chainsaw and the minigun. VCS's animations hold the moved models, so a VC model (or a VC mod, made to sit where
  VC's own does) is drawn into Vic's palm, a chainsaw's handles miss his hands. `tools/weaponfit.py` fits VC's
  originals (read from a VC PC's `models/gta3.img`) onto VCS's models into the converters' table
  (dff2mdl.WEAPON_PLACES; San Andreas holds its weapons as VC does - the table places SA mods too, MODS.md): by their
  outlines (side, top and front silhouettes, intersection over union), not nearest
  points - where VCS's model is another mesh (VC's M4 onto VCS's M16, VC's rocket launcher with its rocket) a
  nearest-point fit (ICP) slid along the barrels and left the grips 10 cm apart, or turned the launcher over. Some are
  other meshes: ak47 (VC's ruger), m16 (m4), m249 (m60), rocketla, minigun - fitted by grip and magazine.

### 9h. Draw distances: the city's detail, entities, population (2026-11-03)

Read for FORK.md T2 with `tools/eedecomp.py` (the world draw `FUN_0023e748`, the area chooser `FUN_0023c188` /
`FUN_0023c820` / `FUN_0023d740`, the piece reader `FUN_002415b0` / `FUN_002417d0` / `FUN_00241278`, CRenderer's
`FUN_0035efd8`, CCamera::Process `FUN_00378dd8`), then measured in game with the rig (`peek` of the level object and
its lists; `pokef` of the scale below with screenshots).

- **The city is two images.** The level's sector list for the current cell (9f: slot `+0x248`, its list `*(slot +
  0x20)`: `+0` (id, geometry) pairs, `+4` their count (u16), `+8..+0x2C` ten bucket pointers; 9 buckets of 0x50-byte
  sector records, sorted by sector id) holds **the whole map**: at Vice Point 585 sectors with centres from -1135 to
  +1173, downtown 367, Little Havana 633. Buckets 0 / 1 / 2 / 6 are the **low-detail city** (ids above ~5500, 0.3 -
  2.6 km away, radii 40 - 400), whose geometry is resident at `0x700000..0xC00000` (loaded with the level); buckets
  3 / 4 / 7 (and some of 6) are the **detailed city** (ids below ~5000), only within **~260 m of the camera** (p90
  235 m, at most 290-360 m), its geometry in the cell's piece and its 3 x 3 area's piece (owner tags in the table:
  `0x40000000 | y << 16 | x` the cell's, `0x80000000 | ...` the area's). A low-detail sector that a detailed one
  replaces near the cell carries bit 0x8000 (hidden) in that cell's list. So the pop-in while driving is the cell
  change: the detailed band ~250 m around the camera moves with it (the cross-fade below softens it).
- **Sector record** (0x50): `+0` u16 id | 0x8000 hidden (bit 0: a second pass), `+2` u16 index into the level's
  geometry table `**(level + 0x1C)` (0xC entries: geometry, its cached DMA packet, owner tag), `+4/+6/+8/+A` half
  floats: centre (world) and radius (the frustum test `FUN_003816f8`), `+0xC` a matrix pointer or 0, `+0x10..+0x4F`
  the matrix - relative to its own cell's centre (`FUN_00241f00(cell x, y)`); the draw moves the world origin there
  (`FUN_0018ed28`, `0x6FE980..0x6FE98C`). Ids below 0x20 are the moving sectors, placed every frame from
  `level + 0x5E0 + id * 0xD0`. The draw skips sectors (all of them, also in `FUN_00240ed8`) while the packet buffer
  `0x520F90` has less than 0x12C00 free of 0xE6670.
- **Cross-fade**: after a swap (`FUN_0023d740`) `level + 0x264` = 0x1DF counts down (`FUN_0023c820`, by the time step
  times `0x487018`); while it is above 0, `FUN_0023e748` merges the current and the previous cell's lists by id
  (one draw for a sector in both, `FUN_0023e548` with the fade alpha for one only in either), the previous cell's
  with the world origin at its centre.
- **Cell pieces** (the level's piece index `*(level + 0x1C) + 4 + y * 8`: base, first x; 36 rows of 14-19 cells, x
  starting at 2 / 3 alternately - the hex grid): 0x20 bytes each, `+0` 'DLRW', `+8` bytes read, `+0xC` bytes
  kept, `+0x18` offset in the level's image; 110-220 KB apart in the image. `FUN_002417d0` reads a piece into the
  slot's buffer cursor (`level + 0x280`, file `level + 0x278`, async `FUN_002213d0`, priority 500) and appends it to
  the slot's lists; `FUN_00241278` relocates it (`FUN_00324ac0`) and starts the next queued piece (`level + 0x414` of
  them at `level + 0x294`). The area pieces (`+0x24C/+0x254/+0x25C`, keyed by cell / 3) register their geometry in the
  same table through `FUN_0023c018` (owner `0x80000000 | area`). The slot buffers share `0xCB7A80..0x112A660`.
- **Seen**: with the camera 150 m ahead of where the cell was chosen (the streaming frozen, the free camera moved back)
  the band reaches further along the view and the camera's own surroundings stay detailed; 250 m ahead the buildings
  at the camera are low detail - the cell's list stops ~1.5 cells out.
- **Entities** (objects, crowds; not the city) are VC's CRenderer: the sector lists of the world's entity grid
  (`FUN_0035fae8` / `FUN_0035fed0` / `FUN_003602f8`, as VC's ScanSectorList) call **SetupEntityVisibility
  `FUN_0035efd8`** (0 invisible, 1 visible, 2 offscreen but near, 3 request the model) and fill the visible lists
  `0x742B80` (count `0x4CD598`), `0x744AC0` (`0x4CD59C`: vehicles, peds), `0x746C80` (`0x4CD5A0`), `0x746A00`
  (`0x4CD5A4`, at most 0x95, within 30 / 65 m). The camera position for it is `0x6F3C00`. Model info: `+0x10` type,
  `+0x28` atomics, `+0x2C` LOD distances, `+0x38` count, `+0x39` fade alpha, `+0x3A` flags. GetAtomicFromDistance
  `FUN_003a52f8`: the first atomic with `distance < lod[i] * LODDistMultiplier`; GetLargestLodDistance `FUN_003a5380`;
  past it a 20 m fade, the model streamed within +50 m (+30 without fade), beyond 330 m the distance is clamped to
  the largest LOD distance. VC's IDE distances in VCS's data (e.g. model 0x1D1E and 0x18F5 forced to 300).
- **Rendering the lists, vehicles' classes, the draws' origin** (found 2026-11-04 for DISTANT_LIFE.md A2):
  `FUN_0035c4b8` walks the four visible lists (entity pointers, 4 bytes each) calling each entry's vtable `+0x40`
  (PreRender), then the moving list at `0x6F40D0`; the render functions that read the lists are `FUN_0035cd98` /
  `FUN_0035d308`; `FUN_0035c2c8` empties them. The vtable is at entity `+0x5C`, Render at slot `+0x44`. Vehicle
  classes by vtable (the pool in a street and a car made by the rig): `0x48F8C0` cars (Sentinel 211, 184, 236, 243,
  259...; Render `0x14C308`), `0x490F10` (models 191, 212; `0x343F30`), `0x4901C8` the ambient planes (model 280,
  1-3 km away, no clump; `0x22CF80`), `0x491680` (238, no clump). Every Render ends in **CEntity::Render
  `FUN_003c3e58`**: entity `+0x50` the RW object (type byte 1 an atomic: its callback `+0x24`; else the clump's
  render `FUN_0039ec18`). Hooking the class Render (`0x343F30`) saw nothing for the cars - CEntity::Render is where an
  entity's draws begin. **The draws' world origin**: the renderer keeps positions relative to a cell's centre;
  `0x6FE980` holds minus that centre (775, 1188.12, 0 with the camera in cell (13, 7)) and the VU1 object matrices are
  the entity's world matrix plus it (a car at -766, -1208.7 drew with its matrix at 8, -21.1). The VU1 object matrix
  carries the vertices' dequantisation scale in its rows (2, 4, 1 or 2, 8, 2 seen for car parts).
- **The multipliers** (CCamera::Process `FUN_00378dd8`, `0x37AF4C..0x37B078`): `LODDistMultiplier` (camera
  `0x6F44D0 + 0x7DC` = **`0x6F4CAC`**) = 70 / FOV first, times `1 + max(0, (z - 55) / 60)` when the camera is above
  55 m (the near plane grows the same way, `FUN_0018f3b0`), at most 2.0; copied to `GenerationDistMultiplier`
  (`+0x7D0` = **`0x6F4CA0`**), then times **CRenderer::ms_lodDistScale `0x4887F8`** (1.0; nothing writes it, only
  this reads it). Measured: 1.0 / 1.0 at street level at 70 degrees. Writing 2 / 4 into `0x4887F8`: the visible
  entity list at Vice Point beach went 1 -> 10 -> 20 (crowds far down the beach drawn), downtown 2 -> 10 -> 15; the
  city did not change. Graphics > Draw distance > Object distance (src/game/vcs_distance.cpp) writes it before every
  CCamera::Process.
- **Population** reads `GenerationDistMultiplier` (gen): **`FUN_00430290`** (as VC's CCarCtrl::GenerateOneRandomCar:
  a path node within 110 x gen, the car placed 100 x .. 160 x gen from the player (x 1.5 for some), never within 82.5 x
  gen of the camera), **`FUN_00433378`** (a car's removal: beyond 160 x gen, 60 for some, 70 with the camera looking
  down), **`FUN_002afe80`** (as CPopulation::Update: peds added 35 x .. 60 x gen times `FUN_002b44c8` by
  `FUN_002b17a8`, as AddToPopulation), and `FUN_00219898`, `FUN_002b0200`, `FUN_0036ebf8`, `FUN_003da070`,
  `FUN_003f29d8` (not read yet). It is copied before the scale, so Object distance leaves the population alone.
- **Pools** (`CPools::Initialise FUN_00288100`): peds 70 (`FUN_00478f28`, 0xD20 bytes each), vehicles 40
  (`FUN_00478fc0`, 0x8C0), then 0x1DB, 0x14 and 0xC0 for the other pools; each constructor allocates its own entries
  and flags from the main heap. CPool::New for peds `FUN_004767b0` searches up from the last index (`+0xC`) and wraps,
  Delete `FUN_00476850` lowers it; GetHandle `FUN_004701b8` = index << 8 | flags, GetAt `FUN_00471788`. Enlarged
  through the constructors' size (2026-11-04, `VCS_POOLS=peds,vehicles` for a test). **More than 128 peds broke loads**:
  with a save loaded, 130+ froze within seconds (RpClumpRender `FUN_002c0270` looping on a broken atomic list, the main
  heap's bytes-in-use negative, an anim block's unload `FUN_00225610` -> `FUN_001cf848` over a garbage count), while a new
  game ran clean with 280. The cause is the game's own: the world clear before a load / restart (**`FUN_0018f5d0`**)
  deletes the peds with `for (i = 0; i < pool size; i++) if ((ped = GetAt(pool, i))) delete` - GetAt takes a *handle*, so
  i is slot 0 with flag byte i. Below 128 that only matches slot 0's 7-bit id in use (the PS2 deletes that ped; the
  flags are reset after, `FUN_004740b0`); from 128 up it matches slot 0's byte when the slot is **free** (bit 7) and a ped
  that is not there gets "deleted". Found by elimination: not the heap (7 MB free), not slot use (a host CPool::New that
  never hands out slots >= 128 still froze), not the size readers (VCS_RWATCH on the pool's size: 23 sites); a new game
  never clears. The port gives that call (return `0x18F870`) an invalid handle for i >= 128 and builds 280 peds / 200
  vehicles. No vehicle-size reader calls GetAt with an index. Two checks of the index elsewhere: `FUN_0020c3c8` (< 71)
  and `FUN_0020c400` (< 70) answer false for higher slots.
- **Sector entries** (found by an Open Limit Adjuster style audit, `build/analysis/t2_agents/limits.md`): two CPools
  from the game image (GAME.DTZ, handed out by `FUN_003321f0`), not CPools::Initialise - **EntryInfoNode `*0x487A90`**
  (5200 x 0x14 {list, ptr node, sector, prev, next}, chained at an entity's +0xE4; New `FUN_00478bf0`, Delete
  `FUN_00478c88`) and **PtrNode `*0x487A8C`** (40000 x 0xC, New `FUN_004716a8`). The world grid is 50 x 50 cells of 80 m
  (records of 0x38 at `*0x486464`); CWorld::Add `FUN_00347348`, Move `FUN_00347740`, Remove `FUN_00347660`; nobody checks a
  full pool. Measured: the city takes ~4810 entries, the crowds at the maximums 5020 (PtrNodes 24.5 k). The port adds an
  overflow pool of 8000 entries (vcs_distance.cpp).
- **Peds** (`FUN_002afe80` CPopulation::Update): AddToPopulation **`FUN_002b17a8`(nearest, farthest in view, nearest,
  farthest out of view)** places a ped (CPopulation::AddPed `FUN_002b2908`, the only creator of random peds; spot finder
  `FUN_00268048`) - in view 35-60 x GenerationDistMultiplier x a factor (`FUN_002b44c8`), out of view factor x 20 - 10 ..
  factor x 20 (a fixed 10-20 m). After a camera cut Update counts down `0x4CD3BA` and then `FUN_002b0070` calls it 100
  times with 10 .. factor x 60 for both. UpdateAnim (`FUN_003c50a8`, CEntity's, vtable +0x48 for every moving entity)
  always asks peds for the full animation update (`FUN_00154d10(step x 0.02, clump, 1)`); vehicles and objects out of
  view get the root-only one (`$a1` = 0, `FUN_001552f8`). Vtables (+0x30 ProcessControl / +0x34 ProcessCollision /
  +0x48 UpdateAnim): random ped `0x490840` (`FUN_002d8788`), player `0x4902a8` (`FUN_00230640`), vehicles `0x4901c8`,
  `0x490f10`, object `0x48f848`; ProcessCollision `FUN_00348d40` and UpdateAnim shared by all.
- **CVisibilityPlugins** (VCS's, `FUN_0036ebf8(RwCamera)`, at each render from the frame function): squared
  distances its render callbacks compare with - `0x4CD5C0` vehicle components culled past 20 m x LODDistMultiplier
  (1e6 in camera modes 1 / 0x25), `0x4CD5D8` = `0x4CD5CC` peds drawn up to 70 m x LODDistMultiplier (the ped atomic
  callback `FUN_0036eb70`), vehicles' LOD / fade 60, 70, 90, 100, 150 m x GenerationDistMultiplier (`0x4CD5C8`,
  `0x4CD5BC`, `0x4CD5D0`, `0x4CD5D4`, `0x4CD5C4`). The camera's position for them: `*(*(camera + 4)) + 0x40` at `0x488E0C`.
  Peds and vehicles have no other draw distance (SetupEntityVisibility gives them the frustum only).
- **The frame** (`FUN_0021e950`: CGame::Process `FUN_003ba8d8`, then the render steps `FUN_0021eef0` (render list,
  pre-render), `FUN_0021efb8` (the entities, the world `FUN_0023e748`, effects), `FUN_0021dad8` (twice)). Timed by
  call site (`VCS_PHASES=1`, `phases` command) in Downtown with every population slider at 50 (106 peds, 39 vehicles,
  15.7 ms): CWorld::Process 6.7 ms, `FUN_0021efb8` 3.1, `FUN_0021eef0` 2.2, `FUN_0021dad8` 1.3, the script VM
  (`FUN_002559c8`) 0.4, CPopulation::Update 0.3, the car generator 0.3; with 42 peds 2.9 / 1.2 / 1.8 / 0.8 ms.
- **Random traffic** (`FUN_00430148` six tries a frame -> GenerateOneRandomCar **`FUN_00430290`**; measured with entry
  counters, 2026-11-04). Wanted = the zone's car density x a per-player factor (the product kept at `0x4E4A54`, s16; 18
  at the Little Haiti street) x the scripts' `0x48A968` x `0x48A680`; then the cap `0x48A9A8` against the sum of the
  counts `0x48A96C` (random) .. `0x48A984` (`0x48A97C` not in it), main heap >= 0x19000 free, a loaded model
  (`FUN_00432888`), a node near the spot (`FUN_00268880`), a spot (**`FUN_00267928`**): the point exactly `distance` from
  the player on a road link crossing that circle, inside the cone along the direction (`cos > c`: 110 x gen ahead, 3
  frames in 4) or outside it (40 m, the 4th: an offscreen spawn), from a node list it rebuilds every 64 frames
  (**`FUN_00269228`**: up to 250 nodes in a square of +-310 m around the player, in grid order). Then a road chance:
  `rand & 15 <= class value` (table **`0x487498`** = 0, 4, 7, 15 by the link's density class) - of ~950 spots found per
  10 s only ~80 passed; a 10 m clearance (`FUN_00195380`); the car is built; then deleted again unless: out of view
  (`FUN_003c4318`, the bounding sphere against the frustum: near `0x4CD130` 0.3, far `0x4CD12C` 1000) within **60 m**
  (90 with extended range) or in view 100-160 x gen from the player and >= 82.5 x gen from the camera, then 20 m of room
  and moving towards the player. Removal **`FUN_00433378`** (as VC's PossiblyRemoveVehicle): out of view past **60 m**
  (70 with the camera looking down, x 1.5 extended), in view past 160 x gen. Standing still, 86 % of the new cars went
  at once; driving about half (the offscreen ones behind).

## 10. Memory card and saves

## 10. Memory card and saves

libmc calls are HLE (`Kernel/Stubs/MemoryCard.cpp`); files live in `%LOCALAPPDATA%/VCSRecomp/mc0`
(`src/main.cpp`), one host folder per card folder.

### 10a. Card manager

- Card manager object `0x510480`, one 0x4C0-byte block per port. Status word `+0x54` reads 27 (0x1B)
  when idle / the last operation was fine, 14 (0xE) after "no entry", 1 after "no format".
- `FUN_00275068(mgr, port)` / `FUN_002760d8(mgr, port)` list the current folder:
  `sceMcGetDir(port, 0, "*", 0, 15, mgr + port*0x4C0 + 0x100)` (path string at `0x4B0148`), then
  `sceMcSync(0, ...)` - **synchronous**, waits for the result. The 15-entry table (64 bytes per
  entry, name at +0x20, attributes at +0x14, size at +0x10) is what everything else reads.
- `FUN_002788e0` is the save / load state machine (states 0..5 at `0x487730`, error at `0x487744`,
  open fd at `0x487748`); it builds paths with `sprintf` (formats at `0x4B03D8` / `0x4B03E8`) and calls
  `sceMcGetDir` with 1 or 20 entries for single files.

### 10b. Save folder and slot files

- `FUN_002778a8(mgr, slot)` (from the save menu's "save to slot", `FUN_00108128`, slot byte at
  `0x4818AC`) creates `BASLUS-21590GTA40001/` with `icon.sys` and `vcsicon.ico` if missing, then
  writes the slot file and the `BASLUS-21590GTA40001` data file (179,350 bytes). Order of creation
  on a fresh card (host creation times): `icon.sys`, `vcsicon.ico`, `BASLUS-21590GTA40001`, then the
  slot file.
- Slot files are named `"<slot> <mission name>"` (e.g. `1 In the beginning`, `1 Soldier`), 98,304
  bytes; the data starts with the block magic `"SIMP"`. Saving over an existing slot (checked 2026-10-23 with
  `VCS_TEST_SAVE` on a copy of the user's card): the game lists the folder, reads both slot-1 headers, issues two
  `sceMcDelete`s (both succeed), then opens `1 Soldier` with flags 0x203 (create | write | truncate), writes
  98,300 + 4 bytes and rewrites `icon.sys`. No new file appears; a stray older `1 ...` file (made before the
  directory-order fix) is left alone.
- `FUN_00277688(mgr, slot)` checks the card and free space (0 ok, 2 not enough space, 3 error); the
  game calls it at boot.

### 10c. How the game finds its saves - card directory order matters

`FUN_00276bf0` (loop at `0x276da0..0x2772dc`) scans the listing for **8 slots**: for slot s (1..8)
it walks **table entries 5 to 14**, and takes an entry when its name is non-empty, its attributes
have bit 0x80 (closed) set, its size is > 0x17FFF, and its first character equals `sprintf("%d", s)`
(format at `0x4B00A8`). Then it reads each slot file's first 268 bytes; with two files for slot 1
on the user's card it read both headers and then loaded the newer one (`1 Soldier`) in full.

It **skips entries 0-4**, assuming they are `.`, `..` and the three files created with the folder.
That is only true in a real card's **directory-table order** (creation order; a deleted entry's slot
is reused by the next file created). Our first `sceMcGetDir` sorted names alphabetically, which put
`1 ...` at index 2 - so saves were written but never found: no load at boot, an empty Load Game
list, and a new save to slot 1 created a second `1 ...` file next to the old one (found 2026-10-22
from the user's card: `1 In the beginning` and `1 Soldier` side by side). Fix: each host card folder
keeps its order in a hidden `.ps2x-order` file (PS2RECOMP_PATCHES.md).

### 10d. Loading

- At boot the game reads `icon.sys` (964 bytes), lists the folder, reads every slot file's header
  and then the newest slot file in full (98,300 + 4 bytes), showing "Loading data..." and "Load
  Successful." before the world loads at the save point.
- `FUN_003adae0()` (generic load) runs from world init `FUN_00255308` when a load was requested; it
  reads through `FUN_002759e8` (`sceMcRead`).
- `VCS_TEST_SAVE=<s>` (`src/game/vcs_overrides.cpp`) runs the save sequence as guest tail calls from
  `scePadRead`. A direct host call of a guest function is not safe: a scheduler checkpoint inside it
  unwinds the call and the rest of the routine never runs.

### 10e. The save / load / delete menus (driven 2026-10-23, `tools/saves.py`)

- **Save Game** is opened by scripts with `ACTIVATE_SAVE_MENU` (opcode 0x0260, no arguments; handler entry at
  `0x4BFDF0` in the opcode table) - the rig can open it anywhere. "Save the current game?" -> Save Game -> the
  8-slot list ("1 Soldier  22 Oct 2026 23:31:27", "Save File 2 Not Present" ...) with the cursor on **Cancel**;
  an empty slot asks "Are you sure you want to save this game?", an used one "Proceed with overwriting this saved
  game?" (both default to **No**), then "Save Successful. Select OK to continue."
- Saving advances the clock (12:02 -> 22:00 at Fort Baxter). Loading that save then starts the story's next
  mission intro (script `ch_jerr`, "Take Sgt. Martinez's bike") - game flow, not a load problem; the original
  Fort Baxter save loads straight into the barracks.
- **Load Game / New Game / Delete Game** are on the pause menu's *Game* tab (Map, Brief, Game, Stats, Controls,
  Audio, Display). Load lists the slots with their dates, asks "All unsaved progress in your current game will be
  lost. Proceed with loading?", and reads the slot file in full (98,300 + 4 bytes). Delete asks "Proceed with
  deleting this saved game?" and ends with "Delete Successful"; the file and its `.ps2x-order` entry go.
- With two files for slot 1 on the card (the stray `1 In the beginning` from before the directory-order fix) the
  menus show only the newer one.
- At boot the newest slot (by the header date) is loaded automatically; with slots 1 and 2 present it took slot 2.

### 10f. Save names, a failed save, and saves lost on the host (2026-11-01)

Players' reports after 0.1.0: "it crashed and destroyed my savegame", "the game closes and deletes the save file when I
save at Phil's safehouse". Found by reading the mission names out of `TEXT\AMERICAN.GXT` and saving under each odd one
with the rig (`REGISTER_MISSION_PASSED` 0x01EB with a text-label argument: type byte 0x0A, the key, a NUL - e.g.
`EB 01 0A "REN_C05" 00`, then `ACTIVATE_SAVE_MENU`).

- **The slot file's name is `"<slot> <name of the last mission passed>"`**, the GXT text of the key the last
  `REGISTER_MISSION_PASSED` gave (the game's text table, 12-byte entries, MAIN's `TKEY` / `TDAT`). The Load / Save /
  Delete lists show that name back from the file name. Mission names with characters a Windows file name cannot hold:
  `REN_C05` "Kill Phil: Part 2" (a colon), `MAR_A05` "Got Protection?", `MAR_A04` "O, Brothel, Where Art Thou?";
  `LOU_A03` "D.I.V.O.R.C.E." ends in a dot (Windows drops trailing dots). The game itself drops the `?`
  ("1 Got Protection" on the card); the colon and the dot it keeps.
- **"Kill Phil: Part 2" on NTFS**: the card layer's `fopen("...\1 Kill Phil: Part 2")` made an empty file `1 Kill Phil`
  with the 98,304 bytes in an alternate data stream ` Part 2`. The game wrote, closed and said "Save Successful" - but
  it had already deleted the old slot file (10b), and the listing now showed `1 Kill Phil`, 0 bytes, which the slot scan
  skips (10c): the save was gone. Fixed in the card layer: card names are stored with such characters as `%XX`
  (`1 Kill Phil%3A Part 2`) and listed back decoded; files lost to a stream this way are recovered at the next listing
  (PS2RECOMP_PATCHES.md, DECISIONS.md 2026-11-01). Checked: the save is whole, listed and loaded at boot ("2 Kill Phil:
  Part 2" in the Load list); a card made by the old code is put right at boot and that save loaded.
- **The slot comes from the file name alone**: `1 Soldier` renamed `4 Soldier` is listed as slot 4 and loads from it.
  With two files for one slot the menus show only the newer (by the header date). A player who lost a save and then
  saved again in that slot (a new game, say) would get the recovered save back hidden behind the new one, so recovery
  moves it to the first free slot ("slot 1 holds a newer save: the recovered one goes to slot 2" in the log); both
  listed, the newer loaded at boot, the recovered one from its slot. With all 8 slots used it stays in its own slot,
  on disk and in the backups.
- **A failed save deletes the whole save folder.** With the slot file's open failing (reproduced: a read-only file in
  the way; anything that fails the open or a write does it), the game shows "Save failed! Check the memory card (8MB)
  (for PlayStation 2) in MEMORY CARD slot 1 and please try again." - and before that deletes every file in
  `BASLUS-21590GTA40001` (every slot, the data file, `icon.sys`, the icon; `sceMcDelete` x6) and tries to remove the
  folder. On a PS2 that clears a half-written save; here a host error took every save with it. Together with the delete
  before the write, any crash or error in a save loses saves, so the card layer now zips both cards into
  `<data>/backups/auto-<date>.zip` before the first delete / overwrite / rename / format of a burst (tools/backup.py's
  format: the launcher's Saves sheet lists and restores them; newest 10 kept). Checked: the folder wiped by a failed
  save, then `backup.py restore auto-...` put every file back byte for byte.
- Not found: a crash in the save itself. Neither the stream case nor the failed open crashed the game (both hidden and
  quit cleanly); the one crash log from a player (issue #34) is a lost D3D11 device on Intel graphics. A crash at any
  point in a save is still covered by the backups.
- Check: `tools/saves.py` (recover, save as "Kill Phil: Part 2", load, delete, backup).

## 11. Scripts: MAIN.SCM and the script VM

Found 2026-10-22 for the automation rig (`src/debug/rig.cpp`, `tools/rig.py`) from a RAM dump at the Fort Baxter
save (`rig.py` `dump`), a read watch on the script (`VCS_RWATCH`), and disassembly. Opcode *names* come from the
Sanny Builder Library's VCS database (`vcs.json`); every number we use was checked against the handler table
below (e.g. 0x0023 terminates, 0x0289 loads a mission, 0x0238 names a script).

### 11a. MAIN.SCM layout

`MAIN.SCM` (root of the disc, 1,876,562 bytes):
- Header: u32 main-section size (0x3CAE3), u32 largest mission size (0x96A9).
- From file offset 8 the script itself; segments are separated by `GOTO` (opcode 0x0002, int32 target):
  globals (file 0x0F..0x63F4) -> segment 2: u8 0, u32 count 1092, 1092 x u16 (a model-id table; searched by
  `FUN_00255188`) -> segment 3: u8 0, u16 0x14E2, u16 0x0419, u32 0x96A9, u32 **101 missions**, 101 x u32 mission
  start IPs -> main code from file 0x7814 (`SCRIPT_NAME "MAIN"`).
- Missions follow the main section in the file; mission n is IP `table[n]`..`table[n+1]` (file offset = IP + 8),
  loaded on demand. Names (their `SCRIPT_NAME`): 0-5 `INITPU INITCG INITGA INITOB INITPLA INITGEN` (the main
  script launches these six at start), side activities (`PARAMED VIG FIRETRK CHOPLIF TAXI` races, trials...) and the
  story chains `JER_A* PHI_A* MAR_A* LOU_A*/B* LAN_B*/C* UMB_B* BRY_B* MEN_C* REN_C* GON_C* DIA_C*` (45..100).
- **Mission names for people** (the trainer, 2026-10-26): almost every mission opens with `LOAD_MISSION_TEXT
  "<name>"` (opcode 0x033A; its own `SCRIPT_NAME` 0x0238 is missing in some, e.g. 44 `JER_A2`, 77 `MEN_C6`, 80
  `REN_C3`, 88 `LAN_C6`, and differs in others: 11 `RENC7` / `REN_C7`, 96 `TEMP1` / `DIA_C1`). The title is the main
  text table's key of that name with a two-digit number: `JER_A01` "Soldier", `REN_C07` "In The Air Tonight". 59
  story missions; 7 `JER_A1` opens the story and 11 `REN_C7` ends it, the rest run 44..100 in story order. Mission 6
  is a 24-byte stub. Side jobs: 14 `PARAMED`, 15 `VIG`, 16 `FIRETRK`, 17 `CHOPLIF` (Air Rescue), 19 `TAXI` - launched
  on foot each says "You have 39 seconds to return to your vehicle". 0x045C (VC's FAIL_CURRENT_MISSION) is something
  else in VCS (one parameter, `FUN_00266350`); no command fails a mission.
- `INITPLA` creates the player: `CREATE_PLAYER 0` at (-1710.7, -306.7, 14.86) into **global 0x2A5**, then
  `SET_CHAR_HEADING` on that same global - VCS uses one handle for the player and the player's char.

### 11b. In memory

| What | Where |
|---|---|
| ScriptSpace | `*(0x48741C)` = `0x1DE0800` = MAIN.SCM file offset 8; an IP is an offset from it |
| Model table / mission table (segments 2, 3) | `*(0x487420)` (count `0x4CD3CC`); mission IPs copied to `0x7404D0[101]` by `FUN_0025b420` |
| Mission count / largest mission size | u16 `0x4CD3EE` (101) / u32 `0x4CD3E8` |
| Mission space offset | `*(0x4CD3D8)` (where `LOAD_AND_LAUNCH_MISSION_INTERNAL` reads the mission to) |
| A mission is running | u8 `0x4CD3E7` |
| MAIN.SCM file handle | `*(0x487418)` (kept open; missions are read from the disc) |
| Script id counter | `0x48742C` |
| Active / idle script lists | `0x4CD3C4` / `0x4CD3C8` (doubly linked: +0 next, +4 prev; insert at head) |
| Script parameters (CollectParameters output) | `0x50DEF8` |
| Opcode table | `0x4BEAF0`: 8-byte GCC pointer-to-member entries {s16 delta, u16 valid, u32 handler}, 1381 opcodes, 1338 with handlers |

`CRunningScript` (0x218 bytes, pool around `0x738948`): +0 next, +4 prev, +8 id, +0x10 IP, +0x14 16-word stack
(gosub), +0x54 0x6A-word local variables (timers at locals 0x68 / 0x69), +0x1FC local frame base, +0x200 wake
time, +0x208 active, +0x20A "mission script" (but set on ambient scripts too: `ambtaxi`, `rampage`, `usj`, `carpark` ... in
free roam with no mission running; seen 2026-10-31 in the F3 game-state panel), +0x20C NOT flag of the current condition, +0x20D, +0x20F name (8
chars; `Init` writes `"id%02i"`), +0x217 "is the mission" (cleared by terminate; with Shakedown launched only its
cutscene script `ch_jerr` has it - the F3 panel's `mission` line uses it). **The stack** (read from a running
mission's slot, 2026-10-29): a GOSUB pushes its return IP as it is, a CALL a packed word (not a plain IP: the caller's
frame goes with it, e.g. 0x0884077D); **+0x204 (u16) is the depth**, and a CALL adds its frame offset to +0x1FC.
Setting IP, depth 1, frame 0 and local 0 between frames (rig `pause on`, `poke`) puts a mission back at its state
dispatch in another state - how `tools/finale_check.py` skips Last Stand to its roof (11g). **A CALL from rig
bytecode does not come back**: the return IP of code in a slot's locals lies outside ScriptSpace (11d: it works by
32-bit wrap-around) and does not survive the packing - the subroutine's RETURN went on at 0x17CFFE, garbage, and the
game loop stopped answering. To run a main-script subroutine the host starts a script at it with a GOSUB stack of one
entry, a TERMINATE_THIS_SCRIPT of the main script (`vcs::rig::startScriptAt`).

`FUN_00255188(id)` returns the index of `id` in that model-id table (count at 0x4CD3CC, table at *0x487420) or -1, by a
linear scan; `FUN_0025A348` calls it for each entry of a list every 30 s of game time (millions of iterations: a
frame held ~14 ms by the EE clock's charge for them until it ran natively, PERFORMANCE.md 14).

Functions: `FUN_002551d0` Init, `FUN_00255950(ip)` StartNewScript (idle -> active, id, Init, IP, +0x208 = 1),
`FUN_002558f0` / `FUN_00255928` list remove / add, `FUN_002560d8` ProcessOneCommand (u16 opcode at IP, bit 15 =
NOT, dispatch through the table), `FUN_00254b90(script, &ip, n, out)` CollectParameters, `FUN_00254ff8` variable
address, `FUN_003b2208` LOAD_AND_LAUNCH_MISSION_INTERNAL (seek MAIN.SCM to `table[n] + 8`, read into the mission
space, StartNewScript, mark it the mission, clear the mission locals), `FUN_003b1d68` TERMINATE_THIS_SCRIPT.

58 scripts run at the Fort Baxter save: `storysc` (story), `pager`, `rampage`, `usj` (unique jumps), the
vehicle-mission monitors (`paramon`, `vigmon`, `firemon`, `taximon`...), `ga_*` (gang), `safehou`...

### 11c. Bytecode encoding

- Opcode: u16 little-endian; bit 15 = NOT for conditions.
- Argument = type byte + data (`FUN_00254b90`, jump table `0x4AE120`): 0 end of a variable argument list,
  1 / 2 zero (int / float), 3 / 4 / 5 float with only its top 1 / 2 / 3 bytes stored, 6 int32, 7 int8, 8 int16,
  9 float32, 10 NUL-terminated string. A byte >= 0x0B is a variable (`FUN_00254ff8`): 0x0B / 0x0C the two timers,
  0x0D-0x6C local variable (byte - 0x0D + frame base), 0x6D-0xCC local arrays, 0xCD-0xE5 global
  ((byte - 0xCD) << 8 | next byte; address ScriptSpace + 4 * index), >= 0xE6 global arrays.
- **CollectParameters runs natively** (2026-11-02, `collectParamsNative` in src/game/vcs_overrides.cpp, PERFORMANCE.md
  23): these rules, the same `*ip` and return value (a type-0 byte returns the count left - 1); `VCS_NATIVE_PARAMS=0`.
  Its callers often write a command's results back into the parameter block (word 0, sometimes more) before the next
  call - which is why the verifier compares words 1.. only.
- **Arrays** (read from FUN_00254ff8, 2026-10-27): a global array is 4 bytes - (byte - 0xE6) << 8 | next byte = the
  base global, then the number of the *local* holding the index, then the array's size; address ScriptSpace + 4 *
  (base + index). A local array is 3 bytes - byte - 0x6D = the base local, the index local, the size; address script +
  0x54 + 4 * (frame + base + index). An index < 0 or >= size points at a dummy word (0x50C140). A local is script +
  0x54 + 4 * (frame (+0x1FC) + byte - 0x0D); the timers (0x0B / 0x0C) are locals 0x68 / 0x69 without the frame.
- **CALL (0x037B) / CALL_NOT (0x037A)**: three raw bytes (in count, out count, the callee's local frame offset), the
  label (typed int; negative = mission-relative), then the in arguments. Labels in a mission are negative offsets from
  the mission's start; positive ones are main-script IPs (the missions call shared main-script subroutines).
  START_NEW_SCRIPT and the other "arguments" opcodes end their variable list with a type-0 byte.
- **Disassembler** (scratch, analysis only): with those rules, Sanny's VCS parameter counts and the argument types above,
  all of MAIN.SCM (main + 101 missions, ~252,000 instructions) decodes without a desync; listings under
  `build/analysis/scm/dis/` (not committed: game code).
- Example (the rig's teleport): `EA 02 01` SET_AREA_VISIBLE 0, `53 02 09 x 09 y 09 z` LOAD_SCENE,
  `44 00 CF A5 09 x 09 y 09 z` SET_CHAR_COORDINATES $PLAYER (z <= -100 finds the ground), `23 00` terminate.

### 11d. Running our own scripts (the rig's `scm` command)

A host-side copy of StartNewScript at the frame kick (between frames, never during script processing) takes an
idle script, initialises it, puts our bytecode in its own locals 0x30.. (`+0x114`, 176 bytes; locals 0..0x2F
stay free for outputs) and sets the IP to that address minus ScriptSpace (the interpreter adds them with 32-bit
wrap-around, so code outside the script space works). The game's interpreter then runs it like any script; it ends
with `TERMINATE_THIS_SCRIPT` and the rig answers when +0x208 drops to 0. Missions start the same way, with the
game's own `LOAD_AND_LAUNCH_MISSION_INTERNAL` (refuse while `0x4CD3E7` is set).

**A slot address is ours only while its IP is in the slot's own code** (2026-10-28, the Load Game hang). Once our
script has terminated its slot is at the head of the idle list, so the game's next StartNewScript takes exactly that
slot; and a save load runs `CTheScripts::Init` inside world init `FUN_00255308` (disassembly at 0x255368..0x25539C:
both list heads cleared, every pool slot `Init`ed and added to the idle list in array order), then starts the
save's scripts from the top of the pool downwards - into the slots the rig and the trainer last used. The trainer
kept its tick script's slot (god mode and the other held toggles) and, on any frame the game's clock stood still,
took a still-active script in it back out of the active list (`cancelCodeScript`, so a paused game never saves
one of ours). During "Loading..." the clock stands still, so it unlinked the script the load had just started in
that slot (`id151`, IP 0x2ADF7, at slot 0x73C1D0 in the reproduction): the load then never finished - "Loading..."
for ~45 s and the game thread returned to 0x4548435F ("_CHE", text) from ProcessOneCommand's dispatch (ra
0x256164). It needed play that left the trainer's last slot among the ones the load fills (the city after the
Fort Baxter barracks: fewer scripts run there than the save restores). `vcs::rig::scriptActive` now also checks that
the IP (+ ScriptSpace) is inside the slot's code area `+0x114..+0x1C4` and logs `[rig] script slot ... holds the
game's '<name>'` when it is not; in plain play the same mix-up stopped the trainer's tick (and cancelled a game
script on the pause menu) whenever the game reused the slot. `tools/saves.py` loads with god mode on.

### 11e. Script commands checked in the game (the trainer, 2026-10-26)

Opcode numbers from the Sanny Builder Library's VCS list (`build/analysis/vcs_sanny.json`), each run from the rig
and its effect read back: 0x024B STORE_CAR_CHAR_IS_IN_NO_SAVE gives the player's vehicle handle - **0x007B
STORE_CAR_CHAR_IS_IN answers -1 for a car made by CREATE_CAR** (MAIN.SCM uses it on cars the player took); a
vehicle command on handle -1 never ends its script. 0x004D GET_CAR_COORDINATES, 0x00D0 / 0x00D1 GET / SET_CAR_HEADING
(SET rebuilds the matrix upright: a flip), 0x03B9 SET_CAR_ROTATION, 0x02E9 SET_CAR_FORWARD_SPEED, 0x01CA
GET_CAR_SPEED, 0x027F / 0x02EE GET_OFFSET_FROM_CAR / CHAR_IN_WORLD_COORDS, 0x00CE GET_CHAR_HEADING, 0x0216
WARP_CHAR_FROM_CAR_TO_COORD, 0x0110 STORE_WANTED_LEVEL, 0x0096 STORE_SCORE, 0x005A GET_TIME_OF_DAY, 0x013C
ADD_EXPLOSION, 0x02F9 SET_CHAR_DROWNS_IN_WATER, 0x03FA SET_PLAYER_CURRENT_WEAPON_AMMO_IN_CLIP (one parameter: the
current weapon's clip). `SET_CHAR_COORDINATES` on the player while he drives moves the vehicle with him.
GIVE_WEAPON_TO_CHAR takes weapon types 0..39; 40 and up hang the script.
- **LOAD_ALL_MODELS_NOW (0x0228) can hang the game**: it loads the requested models inside the frame and does not
  return while the streaming memory cannot take them - after 28 different vehicle models were spawned in one street
  the game thread spun in it (watchdog: PCs all over the game, no frame kicks). The trainer requests and polls instead.
- **HAS_MODEL_LOADED** (0x0161, handler `FUN_0012be70`) -> `FUN_003c6738`: the model's streaming info is
  `*(*(0x489FF8) + 200) + model * 0x18`, its byte +0xC is the load state (1 = loaded). A condition's result lands in
  the script at **+0x209** (with +0x20C the NOT flag and +0x206 the AND / OR count).
- The rig's `scm` answers `ok <slot>`: the finished script's outputs (locals) are at slot + 0x54 until the game starts
  another script in that slot (reading the head of the idle list instead raced with the trainer's own scripts).

### 11f. Mission meters that count per frame (2026-10-27)

The user's report: in PHI_A2 "Boomshine Blowout" the heat bar fills much faster at 144 fps (independent timing). Every
script runs once a frame and a `WAIT 0` loop does one pass a frame, so anything a script adds a fixed amount to per pass
moves with the frame rate. On the PS2 a frame is always 1/30 s; the game's own time-based tools are the timers TIMERA /
TIMERB, GET_GAME_TIMER and the **timed opcodes 0x0031 - 0x0034** (ADD / SUB_TIMED_VAL / FLOAT_VAR: value * ms_fTimeStep),
which REN_C7, SRANGE, MAR_A5, LAN_C5 and DIA_C3 use for their meters. Some missions used the plain ones instead.

- **PHI_A2's heat** (mission 47, the subroutine at file IP 0xCACCD = mission +0x2026, called once per pass from the
  forklift-state loops at 0xCC655 / 0xCC771, both `WAIT 0`): `$G15AB += 0.015` (the heat's floor: it always rises,
  0.45 a second at 30 fps - 100, the explosion, after 222 s), `$G15AA += $G15AC` (the heat; $G15AC = 0 without a crate,
  +1 with the crate in a fire (PHI2_H5 "Keep the boomshine out of the fire!", 0xCAC5E), -1 otherwise),
  `$G15AA = max($G15AA, $G15AB)`, at 100 `$G159E = 7` (fail); the bar is `$G15A2 = (int)$G15AA`
  (DISPLAY_ONSCREEN_COUNTER_WITH_STRING "PHI2_TE"). Measured (`VCS_TIMESTEP`, per game second): floor 0.450 at 1/30,
  2.162 at 1/144 - 4.8x; heat at +1: 29 / 144 a second.
- **How the others were found**: every on-screen counter (024D / 0311) traced back to the variables that feed it
  (CSET_VAR_INT_TO_VAR_FLOAT, copies) and their arithmetic; every non-timed float add / subtract of a constant under 1
  (per-frame rates); every variable-rate float accumulator on a global; every int counter compared with 20 or more.
  Each hit read in context: who calls the subroutine (a call-graph over CALL / GOSUB labels) and whether that loop
  waits 0, whether the change is per frame or per event (a hit, a pickup), and whether it is gameplay or looks.
- **Per frame, gameplay** (scaled by `src/game/vcs_script_timing.cpp`, its table has each instruction):
  - 59 LOU_B1 "Hose the Hoes": the girls' bar `$G1513` -0.04 / -0.03 / -0.01 a frame (by the fires left, +0x6E2..);
  - 67 UMB_B2 "Balls": havoc `$G14E9` +0.005 a frame per living Cholo (+0x24CF), the mission fails at 100;
  - 97 DIA_C2 "The Exchange": the snipers' TIME bar `$G154C` -0.04 a frame from 100 (+0x1E3F; 83 s at 30 fps);
  - 80 REN_C3 "Kill Phil": the Phil bar `$G152E` -0.25 a pass in the thugs state, a pass = two `WAIT 0`s (+0x27EC);
  - 68 UMB_B3 "Papi Don't Screech": stress `$G15B3 += max(rate, damage this frame)`; the rates (-0.2 calm, 0.25 out of
    the car, 0.5 airborne / over 30 / upside down, 2) are set into local 0 at +0xE8A..0xF12 - those sets are scaled,
    the damage (an event) is not;
  - 35 FIREHEL: the bucket's water `$G1700` -1 a frame while dropping (+0x15E3; a fire inside the bucket's box goes out
    at once, so the drain alone is how much a bucket does), the helicopter's health -5 a frame per fire it touches
    (+0x1B73);
  - 77 MEN_C6 "Burning Bridges": pressure `$G1656` +1 a frame while the player is over 70 from the depot (+0x13BE;
    the +3s are per explosion, timed with GET_GAME_TIMER);
  - 18 (the driving range) and 93 GON_C2 "Home's on the Range": the aim `$G14F1` / `$G150E` +-0.2 a frame on the stick,
    back to the centre at 0.05 a frame (the power bar is timed with GET_GAME_TIMER);
  - 99 DIA_C4 "Over the Top": the searchlight targets (model -19, beams by COMMAND_0478 from the towers) step 0.3 /
    0.15 a frame along their routes (x3 alerted): `7L *= $G1587[i]` (+0x53B7) and `8L = $G1587[i]` (+0x5478);
  - 60 LOU_B2 "Robbing the Cradle": the distances the Welfare Man may get away (`$G1513..15`) shrink by 0.1 a frame
    towards 300 / 150 / 600 (+0x1353..).
- **Checked and fine**: TOUR's "Fun" (scored every 2.5 s of TIMERB), REN_C6 / LAN_C7 bars (TIMERA, GET_GAME_TIMER),
  REN_C7 cable damage (timed opcodes) and rig stress (+25 a hit), LAN_B4 cash, DIA_C3 signal, the counters of kills /
  pickups, health-derived bars (LAN_B6 / C3 / C8 Lance, damage bars), TAXI fares, E_RO cruise speed (set from the car's
  speed), position offsets (set, then +-c), the golf power bar.
- **Per frame but looks only** (not changed): text grow / scroll animations (CRASH, SRANGE), object slides and spins
  (GON_C4, MEN_C3's barrier, camera smoothing in LAN_B6), particle bursts, the shared camera-shake helper (0.63 / 0.7 /
  0.67 in many missions), debug tools in the main script (near clip, animation viewer). Minor: LAN_C3 counts frames
  while a spawn point is blocked (its fallback comes sooner).
- **The fix** (vcs_script_timing.cpp): the handlers of 0004/0005/0035/0036 (copy, 0x3B11D8), 0007 (0x3B1248), 0008/002A
  (0x3B12C0), 0009/002B (0x3B1338), 000A/002C (0x3B13B0), 000C/002E (0x3B14A0) are wrapped; ProcessOneCommand
  (FUN_002560d8) has moved the IP past the opcode and calls them through the table with the script in a0. At a listed
  instruction (mission offset = IP - 2 - `*(0x4CD3D8)`, checked against its decoded opcode and operands) the wrapper
  does the instruction with the operand scaled by ms_fTimeStep / (50/30), clamped to 0..2 (integers keep the fraction
  for the next pass), moves the IP on and returns 0, as the handler would. Only when the frame is not the PS2's
  (independent timing, or original timing at 60); original timing at 30 runs the game's handlers. Measured at 1/144:
  floor 0.4496 a second, heat 29.5. `tools/framerate_audit.py` scenario `heat` and `--sites` (every listed instruction
  found in its mission as the game decodes it).
- The mission is read to ScriptSpace + `*(0x4CD3D8)` (an IP; 0x3CAE3, right after the main section) by
  LOAD_AND_LAUNCH_MISSION_INTERNAL (0x3B2208), which starts it there; mission-relative labels resolve against it.

### 11g. The end: Last Stand and the credits (2026-10-29)

The user's report: in Last Stand, once Vic reaches the roof and the final cutscene has played, the game goes to a loading
screen and never comes out. Read from the disassembly (IPs below are MAIN.SCM IPs as the disassembler prints them; a
mission's runtime IP is 0x3CAE3 + (IP - its start)) and reproduced with the rig.

- **Last Stand** is mission 100, `DIA_C5` (text `DIA_C05`; starts at IP 0x1C291E). Local 0 is its state, 1..12, run by
  a dispatch at 0x1C6202 (one GOSUB per state): 11 (0x1C945C) the roof scene with Martinez and Mendez (`SDAC5EA`..`EK`,
  scripted, camera by `SET_FIXED_CAMERA_POSITION`), 12 (0x1C9DF1) the fight, which ends 2.5 s after both `$G16CA` /
  `$G16CB` are dead with state 0 -> the pass at 0x1CA12A: pickups removed, both deleted, `CALL 0x24137 (55)` - the main
  script's mission-cutscene helper, whose table entry 55 is cutscene `DIA_C5` (audio `\AUDIO\CUTSCENE\DIAC5B.VB`, Lance
  arriving on the roof, `MDAC501`..`522`, about 95 s) - then the mission-passed subroutine `0x2FB30 ("DIA_C05", 5000)`
  and the cleanup. The helper waits in **0x22F5E..0x22F97** (`HAS_CUTSCENE_LOADED`, `START_CUTSCENE`,
  `HAS_CUTSCENE_FINISHED`, `CLEAR_CUTSCENE`); only the pass path calls it. The cutscene ends with the screen faded out.
- **The credits** are the main-script subroutine **0x24DC3** (CALLed; locals 0..22, its callees' frames up to local
  42): `$G2AC` (on a mission) = 1, the clock to 21:00, the player hidden and frozen at (269.5, -1158.7, 10.9),
  widescreen, no HUD, ped and car density x10, `WAIT 2000`, `PLAY_END_OF_GAME_TUNE`, `START_CREDITS`, a loop of
  camera GOSUBs over traffic until `ARE_CREDITS_FINISHED` (several minutes), then everything back, the player at
  (-948.0, 1336.7, 10.1) below the Mendez building, `DO_FADE 1000 1` - the only fade in after the cutscene.
- **Who calls them**: after every mission `storysc` (0x2FC69) runs `0x2F9C9`, the story's chapter step: chapter `$G82`
  0 / 1 advance when their flags are set, and for chapter 2 with `$G296` (credits seen) 0 and the chapter flags `$G1B
  $G16 $G1A $G1D $G19` all 1 it waits 5 s (`WAIT 5000`), CALLs the credits, then sets `$G296` and `$G104` and calls
  four more story subroutines. The flags are set only by the story's own progression (0x2AA16..0x2AA52,
  0x39A70..0x39C9F). Nothing else references 0x24DC3 (a search of MAIN.SCM's bytes for the label found that one CALL).
- **The "loading screen"**: with Last Stand played out of story order - the trainer's mission launcher, the user's
  newest save being `LOU_A3` - the check fails, nothing fades in, and the game keeps running behind the faded screen
  (clock, scripts, the world: `storysc` back in its idle loop at 0x2FCD2). Each frame it draws only the copy of FBP
  140 into FBP 0, a black full-screen quad and the text **"Loading..."** (key `LOADIN2`, the frontend's string next to
  `WRONGCD` / `NOCD` / `CDERROR`) at the top right - an F12 dump of the stuck frame; the LoadingScreen function
  (FUN_0021E170) and the menu draw are not called. So it is the game's own picture, which the original would show the
  same way. With the flags set (rig `poke` of the globals) the same run rolls the credits and ends at the building.
- The fix is in the trainer (docs/TRAINER.md): a Last Stand it launched that passes (its script seen in 0x22F5E..
  0x22F97) and ends without the story's condition gets the credits 5.5 s after the end: a script `credits` started at
  0x24DC3 whose RETURN lands on the main script's TERMINATE_THIS_SCRIPT at 0x16E90 (11b); story globals are left as
  they are. `tools/finale_check.py` plays the end both ways (`--story`).
- How it was found: the mission's state from its slot (rig `peek`; the scripts list showed it waiting in 0x22F8E for
  the cutscene's 95 s), the frames still advancing and `CTimer`'s ms moving (not a hang), the active scripts after the
  end (no credits script), the main script's references to `START_CREDITS`, the raw label search, the LoadingScreen /
  menu hooks traced (not called), and the F12 draw log.

## 12. Player, areas and the world

- **Player ped**: `*(0x4E4910)` = `CWorld::Players[0].m_pPed` (`FUN_00237fb0(ped)` returns `&Players[0]` for the
  player's ped). The ped object starts with its matrix: right (+0), forward (+0x10), up (+0x20), position (+0x30),
  each row with a word after it. Found by searching RAM for vectors near the camera that moved when the player
  walked (`findvec`), then the pointers to the object (`findu32`).
- **TheCamera** `0x6F44D0`: +0x30 is the camera's own position (its matrix; a few metres behind the player in normal
  play - read as "the point it follows" until the free camera showed it equal to the camera, 2026-10-30); it also holds pointers to the player ped
  (+0xE0, +0x350, +0x7F8). Its active CCam is at +0x70 (12b).
  - **+0x7F8 = pTargetEntity** (what the camera should follow: the player's vehicle while he drives, else the ped)
    and **+0xE0 = the active camera's target** (what it does follow). Getting into a car on foot moves both to the
    car. **Warped from one vehicle straight into another** (`WARP_CHAR_FROM_CAR_TO_COORD` + `WARP_CHAR_INTO_CAR` in
    one script frame: the trainer's spawn while driving) +0x7F8 moves to the new vehicle but **+0xE0 stays on the old
    one**, and the camera keeps showing the old car while the player drives off (the user's report, 2026-10-28).
    `SET_CAMERA_BEHIND_PLAYER` (0x0221) does not fix it; `RESTORE_CAMERA_JUMPCUT` (0x01CF, handler `0x287BD8` ->
    `FUN_0037C0E8(&TheCamera)` = RestoreWithJumpCut) does: it resets the script camera flags, sets +0x60 / +0x820 /
    +0x888 to 1 and +0x7F8 from the player's state, and the next frame +0xE0 follows. Found by listing TheCamera's
    words equal to the ped / vehicle pointers after each spawn (rig, `trainer spawn`).
  - **+0x888 = m_bLookingAtPlayer**: 1 in normal play on foot and driving, 0 under a script camera
    (`SET_FIXED_CAMERA_POSITION` + `POINT_CAMERA_AT_POINT`, `POINT_CAMERA_AT_CHAR`), 1 again after `RESTORE_CAMERA`
    (0x00B6). +0x840 is 1 while a script points the camera; +0x60 / +0x820 are one-frame requests (read back 0).
- **Current area** (interior): `CGame::currArea` = `0x489F7C`, set by `SET_AREA_VISIBLE` (opcode 0x02EA, handler
  `0x1e31a8`, then `FUN_003cc0d0`). 0 = outdoors. The Fort Baxter save loads with **area 2** (the barracks), so a
  teleport outside must set area 0 first (the user's guess, confirmed by reading the global). VCS has no
  SET_CHAR_AREA_VISIBLE in the opcode database.
- Teleports are clamped to the map: about x -2178..1564, y ..1926.
- **Ped teleport**: `FUN_0020b3d8(ped, CVector *pos)` (remove from the world via `FUN_0018fa50(ped, 2)`, set the
  position `FUN_00472688`, clear movement state +0x1c8 / +0x814..+0x81c / +0x150, add back `FUN_0018f8f8`); reached
  through a vtable, every caller seen goes through `0x4127d0`. `VCS_TRACE_TELEPORT=1` logs each call.
- **The safehouse script moves the player back once**: after loading the Fort Baxter save (inside the barracks
  safehouse, area 2) and teleporting away, the *next* teleport far from Fort Baxter is followed by two teleports
  done by the script `safehou` (IP 0x20E86, then 0x20EA4): to (-1648.87, -262.50, 15.94) and (-1648.87, -261.00,
  14.50) - the barracks door. Once, whatever the destination; later teleports land. Found with a write watch on the
  ped's x (`VCS_WATCH`, pc `0x47268c` inside the teleport), then `VCS_TRACE_TELEPORT`, which hooks the interpreter's
  ProcessOneCommand to name the script that was running. First mistaken for entrance markers. The rig's
  `Game.prime()` triggers it on purpose before working. The world tour's other "relocated" points are the map-edge
  clamp.
- While the ped sits in a vehicle its own matrix is not its world position (it read (-0.4, 0.0, 21.6) in a car
  parked at (-596, 600)); use the vehicle's.
- `CREATE_CAR` (0x0048, handler `0x3d03b0`): 4 parameters (model, x, y, z; z = -100 finds the ground) plus the
  handle output (e.g. 0x1201 = vehicle pool slot 0x12). Vehicle model ids used by MAIN.SCM: 171..276 (211 most).
- `SET_CHAR_COORDINATES` with z = -100 puts the player on the ground (or a roof: (-1400, 1250) landed at z 84).
- **Pools** (`CPool`: +0 entries, +4 flag bytes (bit 0x80 = free, low 7 bits = the handle's generation), +8 size;
  lookup `FUN_00471788(pool, handle)`: slot = handle >> 8, the low byte must equal the flag byte): peds `*(0x487A94)`
  (70 entries of 0xD20 bytes from 0x1D3FF80 in a game started from the Fort Baxter card), vehicles `*(0x487A98)`
  (entries 0x8C0). A script char handle 0x1006 = ped slot 0x10, generation 6. **Objects** `*(0x487AA4)`: 475 entries of
  0x220 bytes (found 2026-10-31: object positions near the ferris wheel 0x220 apart, then the pool header with 475);
  the matrix at +0, model at +0x56, move speed at +0x140 as other CPhysicals. The pointers around it: 0x487A8C (40000
  entries), 0x487A90 (5200), 0x487A9C (6858), 0x487AA8 (20), 0x487AAC (3407), 0x487AB0 (192) - not identified.
- **Ped fields for the trainer** (found 2026-10-26 by setting each value with a script command and searching the ped
  for it): +0x4E4 health (float), +0x4E8 armour (float), +0x480 the vehicle it is in (0 on foot; +0x450 / +0x484 hold
  it too), **+0x8C4 state** (1 on foot, 0x37 driving; +0x8A4 read 0 in these runs), +0x578 weapon slots: 10 x 0x1C
  bytes {type, state, ammo in clip, ammo total (the clip included), ...}, slot 0 unarmed, 1 melee, 2 thrown, 3
  pistols, 4 shotguns, 5 SMGs, 6 rifles, 7 heavy, 8 snipers, 9 special (camera...). Entities: +0x56 model index.
- **Vehicle** (pool entry 0x8C0 bytes): +0x30 position, +0x20 up vector (z < 0 upside down), +0x278 health (float,
  1000 new; +0x27C holds 1000 too).
- **Clock and timers**: `0x4CD148` / `0x4CD149` the clock's hours / minutes (bytes; found by setting 13:37 then 7:11
  and diffing RAM); `0x4CD104` CTimer's milliseconds - it stands still while the game is paused (the pause menu, the
  save screen; found by diffing RAM in play and in the pause menu), so it tells running frames from paused ones.
  The player's money: `0x4E49CC` (in CPlayerInfo).
- **Names**: the text (GXT) table is `*(0x48A2A0)` = {entries, count, data, size} (2553 entries); an entry is 12
  bytes {UTF-16 text pointer (absolute), char key[8]}, sorted by key (corrected 2026-11-03: one dereference, not two). Vehicle models are ids 171..280 in `CModelInfo::ms_modelInfoPtrs`
  (`*(0x486518)`); a vehicle model info's game name (its text key, e.g. `SENTINL`) is at +0x158. Zones: 56-byte
  records {name[8], min xyz, max xyz, ...} around `0x1a6e3e0`; the main districts' names are text keys (`A_PORT`
  Escobar International, `ARMYB`, `BEACH1..3`, `DOCKS`, `DTOWN`, `GOLFC`, `HAITI`, `HAVANA`, `PORNI`, `STARI`, `JUNKY`).
- **Ped fields** used below: +0x48 entity flags (64-bit; bits 4..8 = status, bit 32 = bRemoveFromWorld), +0x56 model
  index, +0x1C8 ped flags (64-bit; **bit 63 = bFadeOut**), +0x6F8 near peds (10) with the count at +0x77A, +0x8A4 /
  +0x8C4 ped state (0x37 driving, 0x39 / 0x3A dying / dead), +0x8F4 created by (1 random, 2 mission).
- **Frame functions** (found tracing ped removal, 2026-10-25): FUN_003ba8d8 = CGame::Process (scripts, then the world,
  population and cars); FUN_001933c0 = CWorld::Process (each entity's ProcessControl through its vtable; entities with
  bRemoveFromWorld go to FUN_00198408, which removes and deletes them); FUN_00210780 =
  CPed::FlagToDestroyWhenNextProcessed (sets bRemoveFromWorld, gets the ped out of a vehicle); FUN_001f4bf0 =
  CPed::ProcessControl (at 0x1f4cfc: if bFadeOut, alpha -= 8 and FlagToDestroy below 0); FUN_00430148 =
  CPopulation::Update (FUN_00430290 adds peds); FUN_00433008 / FUN_00433378 = CCarCtrl::RemoveDistantCars /
  PossiblyRemoveVehicle (a removed car deletes its occupants); FUN_0014ca80 = CAutomobile::ProcessControlInputs (with
  the player's controls disabled it brakes and calls FUN_00237c30, which destroys the player's off-screen random near
  peds and makes the others flee). The script command at 0x2b0b9c (FUN_002b0200) sets bFadeOut on many peds at once
  (an area clear).
- **Mission audio slots** (main script, `AUDIMON` at 0xaf7f and the subroutines 0xa81a - 0xac40 its users CALL): 5
  slots in globals 0x606 (state bits: 1 free, 2 load requested, 4 loaded, 8 playing, 0x20 locked, 0x40 play once
  loaded), 0x60B (the line's name, a script-space string offset, e.g. "MARA1CA"), 0x610, 0x615 (the speaking char's
  handle, -99 none). 0xaa14 requests a line (opcode 0x0413 / 0x0257), 0xa9b6 finds the slot of a name and waits
  (`WAIT 0`) until bit 4 is set. AUDIMON, per slot: requested and loaded (opcode 0x0258) -> 4; **if the speaker is
  "dead" (opcode 0x009F IS_CHAR_DEAD: no ped for the handle, or state 0x39 / 0x3A) the slot is freed (1)** - and a
  mission already waiting in 0xa9b6 on that slot waits forever. Opcode 0x048F tests `(a & b) != 0`, 0x048D ORs.
- **Shakedown's shop "Loading..." hang (MAR_A1, 2026-10-25)** was that wait: entering the shop (the mission's own
  interior at (-1735, 634, 150), area 2) the mission creates the shopkeeper (model 152) and two extras, requests his
  line with him as the speaker - and he had already been deleted, by his own ProcessControl taking the bFadeOut path.
  bFadeOut was clear; bit 31 of the same 64-bit word was set, and the recompiler tested the low word in `bgez`
  (docs/PS2RECOMP_PATCHES.md). Found by: the mission script sitting in 0xa9f9 - 0xaa0b (the `scripts` / `peek` rig
  commands and a rough disassembler with parameter counts taken from each opcode handler's CollectParameters /
  StoreParameters / GetPointerToScriptVariable calls), a write watch on the slot globals (2 -> 0x42 -> 4 -> 1 in one
  AUDIMON pass), the speaker's handle (G615) and its free pool slot, `VCS_TRACE_PED_REMOVE` (who flags and deletes
  peds), a watch on every ped's +0x1C8 armed at the door (`VCS_WATCH_STRIDE`, rig `watch`: no store ever set bit 63)
  and the flag word logged at the removal (bit 63 clear, bit 31 set). It hung at every frame rate and timing mode.

### 12a. The pad and its vibration (2026-10-28)

- The game reads port 0 slot 0 once per frame (`scePadRead` at 0x449488, called from FUN_002843d8 = the pad update,
  0x2848a8) in analog mode: buttons active low, then rx, ry, lx, ly. The runtime's stub asks its pad backend for the
  report; our bindings fill it (`PSPadBackend::setReader`, src/game/vcs_input.cpp).
- **Vibration** (found reading FUN_002843d8 in the generated C++): at start-up it asks for the motors
  (`scePadInfoAct(port, 0, -1)`, 0x28470c; the stub answers 2), then aligns them with `scePadSetActAlign` (0x284770)
  from a 6-byte table at 0x514958 = {0, 1, 0xFF, 0xFF, 0xFF, 0xFF} - act 0 the small motor, act 1 the large one, the
  DualShock 2's usual layout. While the pad's shake timer (`pad + 0x94`, s16, counted down each frame) runs it calls
  `scePadSetActDirect(port, 0, 0x514950)` (0x284898) with byte 0 = 0 (the small motor is never used) and byte 1 =
  `pad + 0x99` (the large motor's level 0..255); when it reaches 0, once more with both 0 (0x284870). A second caller
  is FUN_00285258 (0x285294). A grenade's explosion 2 m from Vic gives level 128 (`tools/controls_check.py`).
  The host maps the large motor to the gamepad's low-frequency rumble and the small one to the high-frequency one,
  scaled by Gamepad > Vibration.
- **The pad's stick curve** (2026-10-29, FUN_002842e8, called for the four stick bytes in FUN_002843d8; decompiled):
  v = byte - 128; |v| <= 42 is 0, past that (|v| - 42) * 127 / 85 (integer, truncated), sign kept - **a third of the
  stick's travel is dead** on top of any gamepad deadzone. The results are CPad 0 (`0x5147A8`, 0xD4 bytes per pad,
  FUN_002852a8(n)) +2 / +4 left x / y, +6 / +8 right x / y (s16); **+0x96 = DisablePlayerControls** (the right-stick
  getters FUN_00286af0 / FUN_00286b20 return 0 while it is set; FUN_00286b20 negates y when the game's own invert flag
  `0x487A58` is set). Found when mouse pitch below about half a tilt came out a third short.
- **scePadRead runs three times per game frame** (366 reads in 122 frames, counted with the `reads=` of `padstate`).
  vcs_input.cpp works the mouse out once per `m_FrameCounter` 0x4CD124 and repeats that report for the frame's other
  reads, so whichever read the game keeps carries it (not measured which one that is).
- **Driving buttons**, measured with a car on a straight street (575, 175): Cross accelerates (forward speed 0.28 after
  2 s), R1 is the handbrake (0.12 to 0 in 0.7 s; coasting 0.28 to 0.20), Square brakes, then reverses. Vehicle +0x140
  is its velocity (units per 1/50 s), +0x10 its forward vector.

### 12b. The camera and the mouse (2026-10-29)

- **TheCamera `0x6F44D0` + 0x70 is the active CCam** (Cams[0]; Cams[1] / [2] at +0x2E0 / +0x550, 0x270 apart; still
  [0] after a lock-on and after getting out of a car). Fields (relative to the CCam): **+0 mode** (4 follow ped on
  foot, 11 locked on with R1 - with fists too -, 18 behind a vehicle; VC's numbering), +0x10 front, **+0x78 alpha
  (pitch; negative looks down, about -1.48 .. 0.79)**, **+0x7C beta (heading, 0..2 pi; the stick to the right makes it
  fall; it is the angle from +x of the target-to-camera direction: 270 deg behind a car heading north, where GTA's
  heading - 0 = +y, counter-clockwise - reads 0; the F3 panel shows the heading from Front instead)**, +0x130 / +0x140
  pitch / heading speeds, +0x134 FOV (70), +0x1A0 the target point, **+0x220 / +0x230 the
  source points** (where the camera stands). Found by diffing TheCamera while the right stick turned it, poking each
  candidate, then a write watch on alpha / beta (`watch`): the writer is **FUN_0029dab0 = the follow-ped camera**
  (decompiled with `tools/eedecomp.py`).
- How FUN_0029dab0 turns (k = pow(0.8, step), 0.8 from the table at `0x487B30` + 0x3C per outdoor / interior): the
  heading's speed = k x the last speed + (1 - k) x (the stick's term |x| x 4.9e-5 x 0.000893 x FOV + a pull toward
  where the camera stands + the swing behind a walking player), capped at 0.1 a step; beta += speed x step. The source
  points are put back around the target from beta and the pitch at the end of each update. Pitch: the speed = k x the
  stick's term (|y| x 4.9e-5 x 0.000536 x FOV, y > 0 looks up) + (1 - k) x the last one, and it **moves the source
  points**, and alpha only
  eases toward the pitch the camera stands at (limits from the table: 0.785 up, 1.4835 down). So a heading written
  alone is pulled back (by ~0.1 a frame for many frames) unless the source points turn with it, and an alpha written
  alone is undone in a few frames; the stick's pitch speed integrates exactly (the smoothing sums to 1). The stick's
  top speeds: heading 0.05, pitch 0.03 radians a step (2.5 / 1.5 rad/s). The pull and the swing are a per-frame turn
  divided by `max(step, 1.0)`, too slow above 50 fps until patched (2026-10-31, section 4: the follow cameras).
- The follow camera ignores the stick while `0x488EB4` is set (FUN_002a2e30) and while CPad +0x96 is set; under a
  script camera TheCamera + 0x888 is 0 (section 12).
- **Mouse look** (src/game/vcs_input.cpp, Keyboard > Mouse camera): on foot (mode 4, no script camera, controls on, not
  paused: the pause menu leaves the mode at 4, `m_UserPause` 0x4CD2AC / `m_CodePause` 0x4CD11C are bytes) the
  pad read adds the mouse's turn to beta and turns +0x220 / +0x230 about the player by the same angle (exact: 300
  counts in 30 moves -> -0.750 rad, 400 counts a radian), and does the same for the pitch: alpha += the tilt, and
  +0x220 / +0x230 turned up / down by it about the target point +0x1A0 (90 counts -> +0.225 rad; 120 counts -> +0.3
  two frames later, and it stays). Other cameras (vehicles, aiming, lock-on) get the mouse's speed as the right stick,
  past the dead third. Checked by `tools/controls_check.py` (the `mouse dx dy` control command moves the mouse in
  hidden runs).
- **Why up / down was slower than left / right** (2026-10-31, FUN_0029dab0 decompiled again; until then the mouse's
  pitch went through the stick): the stick's pitch speed (+0x130 = 0.8^step x the stick's term + the rest x the last
  one: hardly smoothed) does not move alpha. It moves the pitch the source points are placed at (the camera's stand
  pitch = asin(Front.z) from +0x220 minus 0.15, table +8, plus the zoom's offset), and alpha eases toward that stand
  pitch, clamped to the limits: `alpha += clamp((1 - 0.8^step) x (stand - alpha), +-0.1 x step)` (table +0x14 / +0x18;
  indoors 0.9: twice as slow). The heading's speed (+0x140) is smoothed instead (0.8^step x the last), then added to
  beta directly. So on the pad the pitch is 0.6 x the heading's top speed (0.000536 / 0.000893: the game's own design,
  kept) with one ~90 ms lag on each axis; the mouse's pitch through the stick was capped at 1.5 rad/s and eased (the
  heading had neither). The follow camera's table at `0x487B30` (outdoors; +0x3C indoors, picked by `0x489F7C` being
  neither 0 nor 4), read from the ELF: +0 0.6 (target height), +4 2, +8 0.15 (pitch offset), +0xC 2, +0x10 4, +0x14 0.8
  (alpha ease), +0x18 0.1 (its cap a step), +0x1C 0.5, +0x20 0.8 (the speeds' smoothing), +0x24 0.1 (the speeds' cap),
  +0x28 0.1, +0x2C 0.02, +0x30 1, **+0x34 0.7854 up / +0x38 1.4835 down** (indoors 0.7854 both).
- **The camera update** (2026-10-30, for the trainer's free camera; FUN_00378dd8 decompiled with `tools/eedecomp.py`):
  FUN_00378dd8 = CCamera::Process. At `0x378FC0` it calls **FUN_00294540 = CCam::Process** on the active CCam
  (`TheCamera + 0x70 + ActiveCam x 0x270`, ActiveCam = byte `TheCamera + 0x50`; the only caller), then
  **FUN_002A2ED0** (CCam::ProcessSpecialHeightRoutines: lifts the follow camera for peds near the player in modes 4,
  0x1E, 0x24) on the same CCam. CCam::Process switches on `mode - 1` (0..0x37) through the jump table at `0x4B1870`
  (`jr` at 0x2949E8); mode 4's case calls FUN_0029dab0. After them CCamera::Process builds the frame's camera from
  the CCam's **+0x20 Source**, **+0x10 Front**, **+0x60 Up** and **+0x134 FOV** (and +0x1A0, the target point, when
  `TheCamera + 0x890` is set: front = target - source), blending with the previous camera while `TheCamera + 0x8D0`
  (a byte) marks a transition. Writing those four after CCam::Process (in place of FUN_002A2ED0) moves everything: TheCamera +0x30,
  the frustum planes, culling, LODs and the world streaming (the city streamed in at full detail 640 m from the
  player), the sound listener. Found by tracing the call (generated code), reading the decompile for the fields the
  matrix is built from, then the free camera: its position appeared at TheCamera +0x30 / +0x90 / +0x250 / +0x810 /
  +0x960 / +0xA50 the next frame.
- **A cutscene waits on its camera** (2026-10-30): Soldier (JER_A1) launched with the free camera replacing
  CCam::Process sat on "Loading..." for good - the mission script at `0x22F8E` (main script: the cutscene's wait,
  inside 0x22F5E..0x22F97) and the camera in mode 17 at 240 s, where the same launch without it left the cutscene
  ~175 s in (mode 15 for its first ~35 s, then 17; on foot, mode 4, at ~190 s). With the update left running and the
  free camera written after it, the cutscene ends on time. So the cutscene's end is driven by the camera's own update
  (as in VC, where CCutsceneMgr::HasCutsceneFinished asks TheCamera for its position along the cutscene's spline).
  Found by comparing the two runs' active scripts and camera mode every 5 s.
- Teleported to (575, 175) the camera ends up at its lowest pitch (-1.483, straight down) - the game's own doing (the
  same with `VCS_INPUT_MAP=0`).

### 12c. Aiming: lock-on, the game's free aim, shots, scopes (2026-10-31)

Found for free aim with the mouse (src/game/vcs_aim.cpp), by decompiling (`tools/eedecomp.py`; the files in
`build/analysis/decomp/aim_cams.c`, `weapon_fire2.c`) and by watching the player ped while holding R1 / tapping L3 in a
hidden run (rig `peek` / `poke`, snapshots diffed).

- **The game has a free aim** (its help text `H_GUN03`; `C?FREE1` = L3, `C?FREE2` = the left stick): R1 held, then L3
  (R1 and L3 in the same pad report work too, from nothing). It is **a lock-on to an invisible dummy object**: the
  player's **+0x820 (the lock-on target)** becomes an entry of the object pool (`*(0x487AA4)`, model 671, `+0x48 & 0xE`
  = 8: an object; a ped is 6), first placed 30 m ahead of Vic, and **+0xC90** holds the same point. The left stick moves
  the dummy (Vic stands still), the right stick and the mouse do nothing, and the camera is the lock-on camera (mode 11,
  FUN_0029c280) centred on it. **The white circle with a dot** drawn on the dummy is the game's free-aim reticle.
- **Free-aim flag: bit 51 of the player's 64-bit flags at +0xCC8** (bit 19 of the word at +0xCCC; found by diffing the
  ped locked on a ped against in free aim: the only flag that differs). Bit 48 (+0xCCC bit 16) is set by any lock-on.
  Who reads bit 51: the lock-on camera (with it set there is no pitch limit and no crosshair offset: the target is
  centred), FireInstantHit (no random spread with it set, below), the HUD (circle with it, the lock-on triangles
  without), and the ped's control code (the left stick moves the dummy with it, Vic without it).
- **Cleared, the dummy stays the target** - the game then treats it as a lock-on to an object: Vic walks / strafes as he
  does locked on with that weapon (a pistol strafes: 3.3 m in a second with D), facing it, and moving the object moves
  the aim (the camera follows). **The lock drops when the dummy is over ~30 m from Vic** (25 m held, 40 m dropped:
  the weapons' lock-on range); behind a wall or behind Vic it holds.
- Lock-on camera (FUN_0029c280, modes 11 and 28): with a target it turns alpha (+0x78) / beta (+0x7C) toward the
  target's direction from Vic at most `0.1 x time step` a frame (1000 on its first frame, CCam +0xAC set); with no
  target (+0x820 = 0) the right stick turns it like the follow camera. Alpha (pitch) follows the target's elevation
  from about Vic's head. Front = (-cos beta cos alpha, -sin beta cos alpha, sin alpha): beta is the follow camera's
  convention (12b). `TheCamera + 0x950 / 0x954` = 0.53 / 0.47: the third-person crosshair's place on screen (VC's
  m_f3rdPersonCHairMultX / Y), used to offset the lock-on view for guns.
- **Blend or cut**: CCamera::CamControl (FUN_00372450) decides each mode change. **FUN_001F1698(ped) = the ped's weapon
  lets him move while aiming** (`FUN_00407120(type)` byte 0, bit 7): going from the lock-on camera (11, or 28) back to
  the follow camera (4) it is asked (call at 0x375B3C, return 0x375B44); 0 - a gun that stands Vic still (types 20 Equalizer,
  21-23 the shotguns, 25 Scorpion, 27 SMG, 28 / 29 the rifles, 33-35 flame-thrower, M249, minigun; 19 pistol, 24 Micro
  SMG and 26 Mac 10 walk - checked locked on in free aim, one hidden run per gun) - takes the jump cut: no blend (+0x8D0 left 0), +0x8AC set, the new CCam's heading at +0x190 set from its
  +0x80 / +0x84 plus pi. Otherwise CCamera::StartTransition (**FUN_0037C9C8**, the only writer of the durations: 1350,
  then the pair's own) blends. Found by a write watch on +0x8DC (pc 0x37CFEC / 0x37D1E8 inside FUN_0037C9C8) for a
  pistol and an AK release, then its callers (three, all in CamControl) and the decompile. vcs_aim.cpp answers 1 to that
  call in the frames after free aim ends, so every gun's aim ends in the same blend.
- **The blend between cameras**: when the active camera's mode changes, CCamera::Process blends the old view into the
  new one; **TheCamera + 0x8D0** (byte) is set while it runs, **+0x8D8** its start (CTimer ms, 0x4CD104), **+0x8DC** and
  **+0x8E0** its durations in ms - 400 / 350 into the lock-on (free-aim) camera, 350 / 350 out of it; the camera eases
  (0.5, 1.0, 0.5 m a frame cut to 100 ms). Found with the rig's `trace` of TheCamera + 0x800..0x9FF per frame through
  an aim. Lowering both durations after the blend has started shortens it smoothly (vcs_aim.cpp: 100 ms in, 200 ms
  out).
- Leaving any aim back to the follow camera (mode 4): **the follow camera's first update starts it afresh** - beta and
  the source points (+0x220 / +0x230, 12b) set to one heading of its own (156 deg in two releases whose aims were 100
  and 188 deg; the rig's `trace` of the CCam per frame), and that frame's view is built from it: a one-frame snap to
  another angle (the user saw it), then the follow camera eases toward its source points - a swing back to the view
  from before the aim when they are left. vcs_aim.cpp, in the hook after CCam::Process (FUN_002A2ED0, shared with the
  free camera), turns that first frame's whole view about Vic to the aim's heading - beta, source points, Source,
  Front, Up, the target point - before the frame's camera is built.
- **Shots** (a write watch on the shot ped's health with `VCS_WATCH_STACK=1`): FUN_002C3E48 = **CWeapon::FireInstantHit**
  (weapon, shooter, source, ...) -> FUN_002C6460 (the bullet's impact: damage through FUN_001D7940, effects) and
  FUN_002C5320 (tracers / shells). For a shooter with a target (+0x820) and ped +0x35C bit 0 (set while aiming) the end
  is the source plus the direction to the target (an object's +0x30, a ped's bone) times the weapon's range
  (`FUN_00407120(type) + 8`); for the player without bit 51 a random spread scaled by accuracy (+0x78E) and recoil
  (+0xC70) is added. Then **the line test FUN_002D0870(from a0, to a1, colpoint a2, entity a3, ..., shooter t1)** (a
  wrapper of CWorld::ProcessLineOfSight FUN_001908C0) - this call at 0x2C479C, return address **0x2C47A4**; FireInstantHit
  keeps its own copy of the source (s6) for what follows. The other calls of the wrapper in FireInstantHit: 0x2C41A4
  (vehicle targets), 0x2C490C, 0x2C4C98 (vehicle guns), 0x2C4F10 (no target). FUN_002CB7C0 is the drive-by shot,
  FUN_002CDA88 / FUN_002CD288 the NPCs' / doom aim corrections, FUN_002CEA70 = IsTypeMelee (weapon info +4 == 0).
- **A gun running dry** (clip and total at the slot's +8 / +0xC poked to 5, fired in free aim): about 100 frames in
  (60 fps) the game drops the lock and puts the fists in his hand (slot 0) - even with an SMG in another slot. R1 still
  held with the fists is the PS2's lock-on, on a ped near Vic (free aim used to pass it on; not observed directly - the
  user's report and the fists' lock-on checked elsewhere).
- **Weapon changes on foot**: R2 the next weapon, L2 the previous (one step a press); locked on they change the target.
  The current slot is the s8 at ped **+0x789**; slot n's weapon at +0x578 + n x 0x1C (type first).
- **Scopes and first-person weapon cameras** (FUN_00297CB8: modes 7 sniper, 8 rocket launcher, 34 M16, 45, 46 camera, 47
  binoculars): the stick's speed is added to alpha / beta (+0x78 / +0x7C) and alpha clamped to the limits of
  FUN_00297BC8, so writing them before the update turns the scope directly (vcs_input.cpp: exact, 400 counts a radian at
  70 deg). Modes 7, 46, 47 zoom: Square / Cross change the FOV (+0x134) and **0x487E2C** (the zoom it eases back
  toward, FUN_00371938) between 70 and 15 (4.8 for the camera, mode 46).
- **HUD** FUN_00323018 (called from CHud::Draw) draws the scope overlays for modes 7, 8, 34, 40-47 and the binoculars /
  camera / laser-sniper extras; the free-aim circle and the lock-on triangles are drawn elsewhere (not traced).
- Guest RAM below the game (the EE kernel's 0x00000-0x7FFFF) is all zero in play except a few words at 0x10000,
  0x74000 and 0x7C000; vcs_aim.cpp keeps the shot's start point at **0x60000**.
- **Free aim in a car: the drive-by mode** (2026-11-01; Jive Drive, mission 61 `LAN_B1`). The mission puts Vic in
  Lance's Stinger with `WARP_CHAR_INTO_CAR_AS_PASSENGER`, `SET_CHAR_DRIVEBY_MODE(0442) $G2A5 1` and
  **`SET_PLAYER_DRIVEBY_MODE(0481) $G2A5 1`** (0x100377..0x100384, again at 0x101BD8 after its cutscene) - the only
  mission that uses 0481 (a search of every mission's disassembly for 0481 / 04D6 SET_PLAYER_FORCE_FREE_AIMING; 04D6
  appears in JER_A1 (only ever 0) and SRANGE, the shooting range, on foot: below). From then on the game is in its own free aim: ped
  state 0x32, bit 51 set, the dummy object (+0x820 = +0xCF8) the target, the lock-on camera (mode 11) and the circle,
  no button held. Switching 0481 / 0442 off again does not end it; leaving the car does (checked with VCS_FREEAIM=0).
- **FUN_002343E0(ped, pad) is the free-aim dummy's update** (found by a write watch on the dummy's +0x30 in the car: pc
  0x47268C, a vector set, called from 0x234634 with s1 = the player; its only caller is FUN_00230640 at 0x23152C, the
  player's control for states 0x10 / 0x16 / 0x24 / 0x32 / 0x33 with bit 51 set). FUN_00219CC0(ped) = "in a vehicle"
  by state (0x32, 0x33, 0x37, 0x39, ...). In a vehicle it first moves the dummy by the car's movement since the last frame
  (+0xD00 keeps the car's position), then the left stick (FUN_00286928 / FUN_00286A20: x 1.5 times faster than on foot)
  turns it about the camera and puts it at **the weapon's range** (weapon info `*(*(0x712770) + 4) + type * 0x70`, +8:
  pistol 30, SMG / Mac 45, M249 / minigun 75, rifles 90; byte 0 bit 7 = moves while aiming) - in the car with no
  side-to-side limit (on foot +-110 deg from Vic's heading) and up / down only between 70 and 95 deg from Vic's up (20
  above level, 5 below). Then a line test from the camera to the dummy (FUN_001908C0) pulls it onto what it hits. The
  car's turning does not turn it: the aim keeps its world direction.
- The passenger's shots go through FireInstantHit's line test at 0x2C47A4 like the on-foot ones (vcs_aim.cpp's shot
  line counted them: 62 shots in 3 s of fire in Jive Drive). vcs_aim.cpp takes the car's free aim over when free aim
  is on (either device): the dummy written at the pad read at the gun's range along the mouse / right stick's aim
  (kept in the game's height limits), the lock-on camera turned by the same amount, the left stick centred; the game's
  update then adds the car's movement and the line test, as for its own stick.
- **The forced free aim on foot: Phil's shooting range** (2026-11-01; mission 13 `SRANGE`). **04D6
  SET_PLAYER_FORCE_FREE_AIMING** (handler FUN_002D5C20: the script handler table at 0x4BEAF0 is per opcode, 8 bytes
  each, the function at +4) sets **bit 53 of the ped's flags at +0xCC8** (word +0xCCC, mask 0x200000) and, unless he
  is already free aiming (FUN_00235C70), starts the free aim (FUN_002394D8) and the lock-on camera (FUN_0037C488(0x6F44D0,
  11)); 0 clears the bit and ends it (FUN_0023ADC0). Seen as a memory diff of the command on and off: +0xCCC 0x0040d400
  -> 0x0069d400 (bits 48, 51, 53), +0x820 the dummy, state 1 -> 0x11 (0x10 in the range), +0xD6C bit 0x400. The range
  sets it after each round's start (0x054EA8) and clears it between rounds and at the end. On foot FUN_002343E0 limits
  the stick's dummy to 50..110 deg from Vic's up (40 above level, 20 below) and 110 deg either side of his forward
  (+0x10) - only in the frames the stick moves it (with the sticks centred only the line test touches it). Bit 53 with
  an entity at +0x858 takes the car-like path (the dummy moved with that entity; not seen in a mission). The right
  button (R1) does nothing in the forced aim, but our own free aim's R1 + L3 and then R1 let go ended it for the round:
  vcs_aim.cpp now takes a script's free aim over (bit 51 + the dummy, in a vehicle or with bit 53) before its own entry.
  The range itself: an interior (area 2) behind Phil's door at (-710, -1541.8, 12.4) (the main script warps Vic to
  (-685.31, -1550.65, 30) and back), the marker at (-695.95, -1549.34, 30) launches mission 13
  (LOAD_AND_LAUNCH_MISSION_INTERNAL at 0x24957). Launched from elsewhere (the trainer) Vic falls through the booth's
  floor to the ground at z 12: the interior was never loaded. Targets: objects (models -18 / -42) in `$G1543[11]`,
  scored by HAS_OBJECT_BEEN_DAMAGED (+100; -200 when `$G1590[i]` is set - presumably the board not to shoot, not
  checked), the score `$G15A6`, the round's target `$G15A7`; their
  origin is at the board's foot (aim ~0.6 m above it).

## 13. Open questions / next research targets

- Main loop, frame limiter, CTimer (section 4) - milestone 4 (60 fps).
- Vehicle structure, "in vehicle" / "in water" ped flags.
- After dying (hospital respawn, clock jumps to ~01:25) the world around the hospital was not drawn for ~40 s,
  only lights (autodrive soak 2026-10-23) - streaming or rendering, to find out (milestone 2).
- Mission-passed flags (storyline globals) - for reaching late-game states.
- Cheat handlers (weapons, health, vehicles, weather) - the script commands cover these for now.
- Collision: VU0 program 0 (section 6c), collision streaming - the fall-through bug.
- HUD / font / sprite call sites (CHud, CFont, CSprite2d, CSprite) - milestone 3 (2D at the RW level).
- Sound driver command protocol in full (section 9) - milestone 6.
- Threads and their priorities.

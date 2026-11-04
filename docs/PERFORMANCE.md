# Performance: well past 100 fps everywhere

The goal (the user, 2026-10-25): at the PS2's resolution the port runs well past 100 fps everywhere, without changing
what the game does. This is the working list: what was measured, what was done (with numbers), what is next.

## Where it stands (2026-10-25)

Display > Game timing "Frame-rate independent" with the frame rate cap at 240 or Unlimited (= up to 240; the game's
time step stops getting shorter there). The PGO + ThinLTO build (`tools/build.py --opt pgo`), i7-7700K + GTX 1080 Ti,
640x448 internal resolution:

| | before (uncapped, floor lowered) | now |
|---|---|---|
| Fort Baxter | 111 fps | 223 |
| city 575,175 / -675,1425 / 1325,-325 / -1425,-575 | 166 / 118 / 130 / 125 | 240 (ceiling) / 201 / 226 / 220 |
| AI drive | 95 | 150 |
| world tour, 40 points (median / slowest 10% / slowest) | - (200 / 140 / 110 after step 1) | 208 / 165 / 120 |
| 30-minute uncapped autodrive soak | - | 60/60 legs ok; frame time median 6.5 ms, worst 30 s average 9.0 ms |

Before this work the game could not run above 100 fps in any setting (independent timing's ceiling) and the AI drive
did not hold 60 in original timing. Original timing (30 / 60) is unchanged: reference scenes identical throughout.
Where the slowest frames are now: heavy views (~1,950 draws: a rooftop facing the city) at ~120 fps, the game thread
(6.2 ms + 1.6 ms submitting the chain) and the render worker (6.2 ms) both near the limit. Memory: the RSL replay's
recordings grow with the blocks seen (~1.3 GB private after 15 minutes of driving, then flat - keyed by address, so
bounded by the world; predates this work).

## How it is measured

- `python tools/bench.py OUT --fps 0 VCS_TIMING=1 VCS_MIN_STEP=0.05`: uncapped, frame-rate independent timing with the
  step floor lowered (so the 100 fps cap of independent timing does not apply - see "The 100 fps cap" below). Five
  standing spots (Fort Baxter, four city spots from the world tour) and the AI drive from the first city spot.
  Columns: fps, game-thread ms, render-worker ms, GS-stage ms, frame ms. The build is `--opt none` unless noted (PGO
  was ~20-30% faster on the game thread before this work, ~5-10% after it). Since section 5 the floor is 50/240 by
  default: `VCS_TIMING=1 --fps 0` alone is the user-facing "Unlimited (up to 240)".
- `VCS_TIMING=1 VCS_FPS=0 tools/tour.py OUT --every 3 --limit 40`: the world tour uncapped (perf per point).
- Stutters: `tools/cruise.py run OUT [ENV=VAL ...]` - the car along a fixed route at 144 km/h, every frame in a CSV,
  then a hitch report (section 21). A/B by the switches of a change, or two builds; two runs each.
- At the default cap (`tools/bench.py` with no options: 60 fps, original timing) `game` is the game thread's work
  per frame, the rest of the frame is waiting.
- Game-thread / worker profiles: `VCS_PROFILE=5` (+ `VCS_PROFILE_THREAD=RenderWorker`, `VCS_PROFILE_CALLERS=name,...`
  for call stacks).
- Behaviour: `tools/scenes.py capture` + `compare` against `build/scenes/reference`, `tools/tour.py OUT --every 9
  --limit 18` (15 ok, 2 relocated, 1 falling at (-925, 1175) - unchanged since before this work).

## Where it started (2026-10-25, `--opt none`)

At 60 fps (original timing): game thread Fort Baxter 10.9 ms, city 6.7 / 11.2 / 7.0 / 10.2, AI drive 17.1 ms (52 fps:
the drive did not hold 60). Uncapped the game was held at ~100 fps by the EE clock (below).

## Done

### 1. Guest memory accesses: one compare (2026-10-25)

Every guest load / store ran ~25 instructions inline: a vector compare against five special ranges, the debug
watchpoint test (two global loads), a wrap-around check, then the access - and jumps to and from an out-of-line
block. Now (`ps2_runtime_macros.h`, `ps2Read` / `ps2Write`): `addr < g_ps2FastReadLimit` (main RAM, 0x01FFFFF0) ->
the access in place; everything else (scratchpad, I/O, the last 16 bytes of RAM, mirrors, and every access while a
`VCS_WATCH` / `VCS_RWATCH` is armed - the limit drops to 0) goes through the out-of-line checked path, which does what
every access did before. Also:
- the scratchpad: the recompiler resolves VCS's scratchpad addresses (render state at 0x70002020+) as constants and
  calls `runtime->Load32 / Store32` for them - an out-of-line call, a `try`, PS2Memory's range decoding and, for
  32-bit stores, a mutex (`drainCompletedDmacHandlers`). `PS2Runtime::LoadN / StoreN` are inline now with the
  scratchpad in place (an aligned access: one subtract, one test); DMAC completions are flagged by an atomic, so the
  32-bit store checks one flag instead of taking the mutex;
- guest register writes are a 64-bit store of the low half instead of a 128-bit read-blend-write (no dependency on the
  register's previous value).

The executable went from 154 MB to 73 MB. Game thread at 60 fps: Fort Baxter 10.9 -> 4.8 ms, city 6.7 -> 3.3,
11.2 -> 5.3, 7.0 -> 3.7, 10.2 -> 4.7, AI drive 17.1 -> 8.7 ms (now at the 60 cap). Scenes and tour unchanged.

### 2. Recompiler: pc stores only where something reads them (lean pc)

The generated code stored `ctx->pc` before every load / store and around every branch (the branch's pc, the delay
slot's pc, `in_delay_slot`, `branch_pc`, the target, the fallthrough) - four to six stores per branch. Nothing reads
them while straight-line code runs except the runtime calls it makes. Now (ps2xRecomp `pc_observation.h`):
- RAM / scratchpad accesses take their pc as a constant `ps2AccessPc` next to the access, stored only by the checked
  path (I/O, watches, exceptions);
- branches whose delay slot reads no pc: no pc / delay-slot stores; forward gotos and the not-taken fallthrough store
  nothing; back-edges keep theirs (a checkpoint resumes at the loop head); JAL's dispatch stores the pc itself;
- a function entered at its start (the common case) skips the resume `switch`.
What sees a stale pc now: the sampling profiler's guest pc and crash reports (the last call / loop head / I/O
access). `PS2X_RECOMP_FULL_PC=1` when running the recompiler restores the old emission. Game thread at 60 fps:
city 3.1-4.8 ms (-5-10%), AI drive 8.7 -> 7.6 ms.

### 3. Recompiler: `ctx` as `__restrict`

Guest registers live in `ctx`; every guest store goes through `rdram` (a `uint8_t *`, which may alias anything), so
the compiler reloaded every register from `ctx` after each store. Generated functions now take
`R5900Context *__restrict ctx`: every runtime call that touches the context is handed `ctx` (checked: dispatch,
syscalls, breaks, traps, VU0 calls, the unlisted-resume report; `eeCheckpointDue()` only reads the scheduler's clock),
and the guest's RAM never holds the register file. The busiest function's assembly went from 72.8k to 25.7k lines.
Fort Baxter 5.3 -> 4.4 ms, city a few percent. `PS2X_RECOMP_NO_RESTRICT=1` (recompiler) = off.

### 4. Runtime overheads on the game thread

- **The EE clock held the game at ~100 fps.** Guest work is charged estimated EE cycles (8 per call, 32 per loop
  iteration); above ~100 fps the clock ran ahead of real time and the scheduler held the EE at every due event (VBlank)
  until real time caught up: `wait_ms` ~3 ms a frame. Frame-rate independent timing tops the clock up to real time at
  each frame's end anyway, so it now charges a quarter (`EeScheduler::setGuestChargeShift(2)`, `VCS_EE_CHARGE_SHIFT`);
  original timing is unchanged.
- **The flip waited for the render worker.** FUN_003706c8 reads GS CSR for the FIELD bit; a CSR read fences on the
  render worker (SIGNAL / FINISH come from GS work), so the game thread waited for the worker to drain once a frame -
  ~25% of the game thread uncapped. FIELD is set at VBlank on the game thread: that one read skips the fence
  (game-internals.md 6a; `VCS_CSR_FENCE=1` = as before).
- **Unlimited frame rate was 180 fps.** The main loop executes the frames the game queued (at most 3 ahead) and was
  paced at `max(60, cap)` - 60 for an unlimited cap: at most 180 game frames a second. Unlimited paces it at the
  platform's maximum (1000 Hz).
- VU0 microprogram calls: the native lookup (a `memcmp` of the program every call) is redone only when VU0 code
  memory changed (its write generation); the VU's rounding mode is set in MXCSR alone (`fesetround` also reprograms
  the x87 unit); the call no longer reads the clock unless `PS2X_TRACE_VU0` is set.
- The scheduler's event queue and due-deadline list are buffers kept between calls (a container was allocated and
  freed per call, ~2% of the game thread with the other allocations).

Uncapped after 1-4 (`--opt none`): Fort Baxter 209 fps (game 4.2 ms), city 331 / 202 / 135 / 232, AI drive 126 fps
(game 5.6 ms, worker 8.0 ms). The render worker is now the limit at the busy city spot (7.4 ms) and on the drive.

### 5. Past 100 fps: the ped-movement step bug (2026-10-25)

Frame-rate independent timing was capped at 100 fps because below CTimer's 0.5 step (1/100 s) peds ran at a quarter
speed. Write watches on the player's move speed at 1/100 vs 1/110 s steps traced it to one branch in FUN_001f1d38
that the PS2 never takes (game-internals.md 4); one instruction patch fixes it without changing steps >= 0.5. The audit
matches 1/30 - 1/100 up to 1/240 s, so the ceiling is now 240 fps: caps 120 / 144 / 165 / 240 added, display refresh
and unlimited go up to 240 (DECISIONS.md 2026-10-25). Also fixed on the way: `tools/gamectl.py` closes its sockets
without TIME_WAIT (the audit at 200+ fps ran out of ports) and the rig kills the game with its launcher when it does
not quit.

### 6. Render worker, first pass (2026-10-25)

Now the limit at busy spots (the game thread waits on it ~400 ms a second at city 1325,-325). Done: ped skinning in SSE
lanes (same operation order, bit-identical - `VCS_SKIN_VERIFY=1` checked 850k packets), recorded unpacks copied in one
`memcpy` / one blend per qword, the CLUT hash in four lanes (VCS loads a CLUT before nearly every draw). City 1325,-325
worker 7.4 -> 6.7 ms, drive 8.0 -> 7.1 ms. What the worker spends its time on there (inclusive profile): native draws
from VU1 programs ~55% (of which the packed draws' recording ~25%), GIF data (texture / CLUT uploads: ~1,070 uploads
and ~930 CLUT loads a frame, nearly all overwritten before use) ~30%, the chain / replay ~15%.

Uncapped at the user-facing settings (independent timing, unlimited = up to 240, `--opt none`): Fort Baxter 170 fps,
city 149 / 192 / 205 / 239 (at the ceiling), AI drive 136 fps.

Then: packed 3D draws recorded per packet (one state check, triangles appended at once - the same draws), and a memo of
texture binds (by their registers, valid while the texture's pages / CLUT / render targets / evictions are unchanged).
Small gains: at the busy spot ~2,700 binds a frame, 69% the same as the last, ~840 through the full lookup (which finds
the texture cached 99.9% of the time; ~1 texture a frame is decoded); the memo catches ~20% of those - VCS uploads each
texture into shared VRAM before its draw, so the pages' generations change constantly. Content-derived page generations
would let more of them hit (not done).

### 7. The GS stage: the render worker on two threads (2026-10-25)

The world tour uncapped (40 points, `VCS_TIMING=1 VCS_FPS=0 tools/tour.py`) had a median of 200 fps and its slowest
points at ~110 fps, all limited by the render worker (8.5-8.8 ms, ~1,900 draws). Its work splits cleanly: the VIF1 /
VU1 / native-draw side ~45%, the GS front end and frame building ~55%. The GS side now runs on its own thread behind
the worker (NATIVE_RENDERER.md "The GS stage", `PS2X_GS_THREAD=0` = off). Uncapped (`--opt none`, independent timing):

| spot | before | after (game / worker / GS stage ms) |
|---|---|---|
| Fort Baxter | 211 | 194 (4.6 / 2.1 / 1.7) |
| city 575,175 | 235 | 239 - the ceiling |
| city -675,1425 | 204 | 213 (4.2 / 0.8 / 0.7) |
| city 1325,-325 | 151 | 215 (3.6 / 3.5 / 4.5) |
| city -1425,-575 | 220 | 203 (4.5 / 0.9 / 0.7) |
| AI drive | 129-136 | 133 (6.3 / 4.4 / 4.3) |

(standing spots vary +-10% between runs with the scene). Now the game thread is the limit nearly everywhere.

### 8. Guest functions without register saves (2026-10-25)

Every generated function used the Win64 convention: its prologue saved 8 general and 6 vector registers and its
epilogue restored them, on every guest call - though generated code keeps nothing in registers across a guest call (the
guest's state lives in R5900Context). Guest functions now use clang's `preserve_none` (`ps2_guest_cc.h`,
`PS2_GUEST_CC`): the recompiler emits it on every generated function and stub wrapper, `RecompiledFunction` carries it,
and the host functions that replace guest ones (src/game, src/debug) are marked; named stub / syscall handlers keep the
platform convention behind a thunk. Host code calling into guest code pays for the saves at that boundary (the
scheduler, an override calling its original). The scheduler's `EeDispatcherTransfer` exceptions unwind through the
guest frames as before. A/B against the build before (same route, two alternating rounds, 60 fps): game thread
standing 4.18 -> 3.90 ms, driving 7.18 -> 6.97 ms (~3-7%). Scenes unchanged.

### 9. Game-thread odds and ends (2026-10-25)

- The FPU multiply took its out-of-line path for every zero result (`ps2_fnormal(0)` is false) - multiplying by 0 is
  common; an exact +-0 product is the PS2's result too, so it returns inline now (`ps2_fmul_slow` was 1.3% driving).
- Chain buffers taken from the render worker's pool went back to PS2Memory's own 4-buffer pool for the transfers handled
  on the game thread; it filled and the rest were freed every frame (VirtualFree, ~1.6% driving) while the worker's
  pool allocated new ones. They go back where they came from.
- FUN_003F9E50 (identity matrix: 16 words zeroed in a loop, a checkpoint per iteration) runs natively
  (`VCS_NATIVE_IDENTITY=0` = the original).
Each was ~1%: gone from the profile (guest share 65.7 -> 67.8% of the busy game thread), below what the A/B resolves.
Scenes unchanged.

### 10. PGO retrained; what did not help (2026-10-25)

- PGO + ThinLTO retrained on the new code (`--opt pgo --train`): smaller gain than before (the codegen work removed much
  of what it used to optimise). Uncapped, user-facing settings: Fort Baxter 213 fps, city 212 / 239 / 221 / 223, busy
  city 1325,-325 223, AI drive 144 (game thread 6.0 ms + 0.8 ms submission; worker 4.2, GS stage 3.8).
- The write-watch fold (~0.155 ms per frame, ~430 pages written per fold) cannot be narrowed to a page range: the
  marked blocks span pages 376-8155 of 8192. Its cost follows the pages written, not the range. Not changed.
- The sampling profiler counted a host sample as guest code when it fell within 1 MB after a generated function; it now
  uses each generated function's own symbol size (ThinLTO / PGO builds interleave runtime and generated functions).

### 11. VU0 natives on the context's registers (2026-10-25)

The VU0 natives (~3,100 calls a frame driving: box clip, 3x matrix product, sphere / segment vs triangle) copied the
whole VU0 register file in and out per call; they now work on the context's VF / ACC in place (VF0 reads (0, 0, 0, 1)
during the call, the context's own VF0 put back after, as the copy left it). `PS2X_VU0_VERIFY=1`: millions of runs of
each against the interpreter, 0 differed. Their share of the game thread 6.75 -> 6.4% (smaller than the copy's size
suggested).

### 12. Render worker: rounding mode without the CRT; what is left there (2026-10-25)

The native draws switch the SSE rounding mode around every VU1 program (VU maths rounds toward zero, the caller's mode
is put back for the GIF packets): `std::fegetround` / `std::fesetround` also read and reprogram the x87 unit, unused on
x64, and were ~3% of the render worker's first stage driving. They are now `sseRounding()` / `setSseRounding()` in
`src/game/vcs_hle_world.cpp` (MXCSR bits 13-14 only). `vcsvu::vuExact`'s `std::signbit` (a CRT call, `ldsign` in the
profile, from every matrix set-up) is a bit test. `PS2X_HLE_VERIFY=1` at a city spot: re-draws 147,761 exact, C++
clipper and env pass exact; the world fast path's 16 packets "within 1" (37 vertices, ST LSBs) are the same on the build
before (14 / 34 there). Scenes: static pictures identical.

What the first stage does now (`--opt none`, uncapped drive, worker 3.8-4.4 ms a frame against the game thread's
6-8 ms): idle ~58% of the time; of the rest, about a third is copying - the RSL replay writing recorded unpacks into VU
memory, `GS::submitNative` / `submitPackedKick` putting draws on the GS stage's ring, VIF DIRECT data to the GIF. Those
copies are what step 4 of docs/FULLY_NATIVE.md removes (draws replayed without going through VU memory); smaller
trims there are worth little while the game thread is the slower of the two. The 16 KB save of VU memory before each
native re-draw (restored if it falls back) is not visible in the profile.

Looked at and left (measured on the drive, uncapped):
- **The write-watch fold** at each VIF1 kick: 0.15 ms per fold, ~400 of 8,192 pages written per frame. With the watch
  off (`PS2X_WRITE_WATCH=0`) the standing game thread is ~0.35 ms faster (the fold and the observer's page snapshots),
  but driving is slower overall: the worker loses the RSL replay's skips (worker 3.9 -> 4.3 ms, 121 -> 113 fps). The
  skip decisions are taken during the walk, so the fold cannot run beside it.
- **Copying the VIF1 chain off the game thread** (the kick's walk and copy, 1.0-1.6 ms): the game has no VIF1 DMA wait
  before rewriting what the chain reads - the flip (`FUN_003706c8`) kicks it through `FUN_002721f8` / `FUN_002723E8`
  (game-internals.md 6a); the DMA syncs (`FUN_00272480`) are only in other paths. The chain CALLs packets rebuilt every
  frame and reads RSL's matrix blocks, so a copy running beside the next frame's code would race with it.
- **Checked memory accesses** (`ps2ReadChecked` / `ps2WriteChecked`, ~0.8% of the game thread): nearly all from the
  busiest draw function `FUN_002b8ed0` through scratchpad pointers held in registers; the scratchpad path behind the
  check is already inline (section 4).

### 13. Draw items (FULLY_NATIVE.md step 4c, 2026-10-25)

World / model draw calls became items (one tag in the chain each). Measured even: A/B, three alternating rounds,
`--opt none`, uncapped - driving 110.5 -> 111.3 fps, chain submission 1.12 -> 1.02 ms, worker 4.38 -> 4.29 ms; standing
within noise. The walker's share falls by about what making the items costs (`itemDraw` ~0.5% of the game thread):
step 4b had already taken the copy savings. Where the render worker's time goes did not change: the replay's copies
into VU memory are ~5% of its samples (~12% of its busy time while driving), `GS::submitNative`'s ring copy 3.4%.

### 14. Long-run frame-time spikes (2026-10-26)

**How they were found.** A 24-minute uncapped soak (`tools/soak.py`: autodrive, a teleport to the next city spot every
2 minutes) with the per-frame CSV (`PS2X_PERF_LOG`, `tools/perf_report.py`), then three new tools:
- `VCS_PROFILE_SPIKES=<ms>` (with `VCS_PROFILE`): a frame longer than <ms> gets its own report - the samples taken during
  it, by guest function / host symbol, with a timestamp; `VCS_PROFILE_THREAD` takes a list (`GameThread,RenderWorker,
  GsStage`, threads found as they start) and `VCS_PROFILE_CALLERS` adds the call chains of matching host samples to
  the spike lines (what a wait waits in).
- `PS2X_WAIT_LOG=<ms>`: the EE scheduler logs a wait longer than <ms> - pacing the EE clock to a deadline, or idle with
  every guest thread waiting - with each thread's state, wait reason and pc / ra.
- `VCS_NATIVE_INFLATE_LOG=1`: inflate time per 10 s and the slowest call.

**What they were, and what was done** (each kept bit-exact, each with a switch back):
- **An upstream VAG decoder on every sound-file read.** PS2Recomp's file I/O copied every read of a file starting with
  `VAGp` and decoded it to PCM at close, for a guessed-sample playback that is off (the sound is the SPU2 core's): ~25 ms
  on the game thread per sound-bank load and the PCM kept for good. Now skipped (`PS2X_VAG_DECODE=1` = before).
- **zlib's `inflate_fast` natively** (`src/game/vcs_inflate.cpp`): a line-for-line port of zlib 1.2.2's `inffast.c` on
  guest memory (the EE build's `z_stream` / `inflate_state` layout, OFF = 1), non-overlapping match copies as one
  `memcpy` each. The game inflates whole files with `avail_in` ~16 MB ("enough"): the input is checked by its start and
  every read stays inside main RAM. 0.64% of the game thread while driving before; a whole area (~5.5 MB at a
  Beach / Mainland swap or a teleport) now ~20 ms. `VCS_NATIVE_INFLATE_VERIFY=1` (decode into a shadow copy, compared
  with the original's output at the next call of the same stream): tour subset 1,920 + 1,757 calls, boot + drive 419,
  all the same (a ring of small chunks the game relocates in place before the next call shows as a false
  "differs" - the 21 bytes are the relocated pointers). `VCS_NATIVE_INFLATE=0` = the original.
- **A model-id search every 30 s.** `FUN_0025A348` calls `FUN_00255188` (a linear search of MAIN.SCM's 1,092-entry
  model table, game-internals.md 11a) once per entry of a list: millions of guest loop iterations, charged to the EE
  clock, which then held the frame until real time caught up (`PS2X_WAIT_LOG`: "pacing to a deadline" at pc 0x2551a8,
  ~14 ms, every 30 s). Native, with the registers the loop leaves (`VCS_NATIVE_MODEL_INDEX=0` = the original).
- **Texture decodes on the GS stage.** A texture missing from the cache is decoded from the upload that wrote it,
  mapping every texel through the GS swizzle (`GSMem::PixelBits`, three calls a texel): ~60 ms frames with both the game
  thread and the render worker waiting on `GsStage` after a teleport. When the upload and the texture share format,
  base, width and origin (every decode in the game so far) the texel at (x, y) is the upload's pixel at (x, y): taken
  directly. `PS2X_HW_DECODE_VERIFY=1`: 3,500 textures the same as the VRAM decode (`PS2X_HW_DECODE_FAST=0` = before).

**Result**, the same 24-minute soak before / after (uncapped, `--opt none`): 133.3 -> 138.4 fps, p99 13.7 -> 11.9 ms;
frames of 17-34 ms 291 -> 138, over 100 ms 40 -> 23; away from the teleports, frames over 25 ms with the game thread
the cause 39 -> 15. An uncapped AI drive of 150 s after boot: 3 frames over 17 ms (worst 26.5 ms), p99 9.3 ms.
`tools/bench.py --fps 0 VCS_TIMING=1 VCS_MIN_STEP=0.05`, two alternating rounds: Fort Baxter 203 / 206 -> 219 / 217 fps,
city (1325, -325) 196 / 203 -> 216 / 222 (render worker 5.0 -> 3.3 ms), the other spots and the drive within noise.

**What is left, and why it stays:**
- Teleports and area loads (a black fade, then 150-450 ms): the game's own load - the EE idle while the native sound
  engine plays out sound-bank DMA in real time (its duration is kept: the game relies on it, game-internals.md 9a) -
  then the area's inflate (~20 ms). A soak artefact too: its screenshot per leg (a GPU read-back, 130-170 ms).
- The EE clock's pacing (charged guest cycles running ahead of real time) remains for authentic timing; in
  frame-rate-independent timing a smaller charge (1/16 instead of 1/4) changed nothing measurable.
- Memory grows while new places are visited (529 -> 1,587 MB over the 24-minute soak) and converges when they repeat
  (three rounds of the same 8 spots: +260, +89, +45 MB): caches filling (texture and mesh caches are LRU-bounded; the
  RSL recordings ~115 MB of it), not a leak.

### 15. The helicopter's dust at high internal resolutions (2026-10-30)

**The report.** At 3x internal resolution with 2x MSAA, a helicopter flying low made the frame time spike (the overlay's
graph: the render phase). Reproduced with the rig: the Fort Baxter save, `trainer setwanted 4`, the police helicopter
hovering over the player (a script for it: 3x, 16:9, MSAA 2x, perf every 0.5 s). Game 60 -> ~47 fps, presented frames
60 -> ~16 a second; at 1x nothing (60 fps throughout); MSAA on or off the same.

**The cause** (game-internals.md 6j): the rotor dust is one GS-path draw of ~1,040 large additive triangles, 60-66
screens of overdraw - ~206 Mpixels a frame at 2560x1344 through the GS-emulating pixel shader. Every CPU thread was
waiting (render worker, GS stage ~85%, main thread ~90% in `ZwWaitForSingleObject`): the GPU. The overlay's "GPU" is
the main thread's submit time, so it showed 1.7 ms. New: `PS2X_GPU_TIMING=1` (D3D11 timestamp queries around a marker
per draw, ms per frame by kind / target / texture every 5 s) - 46 of 61 ms of a presented frame on that draw kind, ~14 ms
a game frame on the dust's one 32x32 texture. Ruled out on the way: MSAA resolves and the self-copy (the dust samples no
render target), early Z (declaring conservative depth changed nothing), the shader's tail (a lean early-out path:
-1.2 ms). Dropping its depth test entirely took it to ~5.7 ms - the late, per-pixel depth test of the GS's exact Z is a
real part of the rest and stays.

**What was done** (bit-exact; `PS2X_HW_GATHER=0` = the four loads):
- REPEAT wrapping by a mask for power-of-two sizes instead of `((c % size) + size) % size` - two integer divisions per
  coordinate, eight per bilinear tap set.
- **Bilinear by Gather**: for a game texture (power-of-two, plain REPEAT / CLAMP, no mip chain, not a render target) the
  shader wraps / clamps the top-left texel itself and gathers the 2x2 block at that texel corner through a nearest sampler
  with the texture's wrapping; the GS's weights as before. 96% of textured draws take it (`PS2X_HW_ANISO_LOG=1` counts:
  4.04M gather, 0.16M region wrap in the helicopter run).
- **A pixel shader built for that path alone** (`GATHER_ONLY`: 10 temporaries / 119 instruction slots against the full
  shader's 16 / 1,234 - the render-target paths' registers held occupancy down), chosen per pipeline.
- Checked: `PS2X_HW_GATHER=verify` computes both and paints a pixel magenta where any of its four texels differ - 0
  magenta pixels over the reference scenes and 12 shots of the helicopter scene at 3x (a positive control, comparing
  against the wrong texel, marks ~29% of the screen). Reference scenes four loads vs gather: static scenes 0.00.

**Result** (3x 16:9, MSAA 2x, GTX 1080 Ti, `--opt none`; two alternating rounds each, from 35 s on, the dust on screen):

| | game fps | dust GPU ms / game frame | presented / s |
|---|---|---|---|
| before (first runs) | ~47 | ~14.4 | ~16 |
| mask only (`PS2X_HW_GATHER=0`) | 52.9 / 53.1 | 11.8 / 12.3 | 18 / 18 |
| gather, full shader | 60.0 / 60.0 | 9.7 / 9.5 | 36 / 30 |
| gather, its own shader (now) | 60.0 / 60.0 | 8.9 / 8.9 | 46 / 56 |

Away from the dust the GPU frame at 3x is unchanged (6-7 ms). What is left is the fill itself: ~9 ms for ~206 Mpixels with
the exact Z; anisotropic filtering (the hardware's sampling, an option) or a lower internal resolution take it lower.

**Seen on the way, fixed after** (below): the effect buffers FBP 420 / 384 were re-created ~4 times every frame, helicopter or
not (game-internals.md 6j: a CLUT upload inside them drops both; FBP 420 then flips between the reflection map's window
scale and the bloom write-back's integer scale) - new textures of up to 1536x1536 at 3x, each with a rescaling blit.
`PS2X_RHI_LOG_CREATES=1` shows them.

**The effect buffers' re-creation** (the same day; `PS2X_HW_RT_REUSE=0` = before). What a target holds is unchanged -
the drop stays, a re-made target starts cleared as before:
- one scale per FBP: any windowScale role at the FBP (FBP 420's reflection map) scales every draw into it, the native
  bloom's write-back (width 4) included - no flip, no rescaling blit;
- a dropped or outgrown colour target's texture is kept (up to 8) and taken by the next target of its size, cleared;
- a dropped target is re-made at the size it had, not grown again a step at a time (a copy each).

Result (3x 16:9, MSAA 2x, the helicopter run, two alternating rounds): `gs target` textures created per run ~2,000 (the
log's cap, reached within seconds) -> 7, all at start-up; the main thread's submit time ~1.1 -> ~1.05 ms before the
helicopter, ~1.85 -> ~1.6 ms with it; 60 fps either way. A clean-up more than a speed-up on this machine: the
allocations were cheap here, but they were ~200 a second of up to 9 MB each. Checked: the evening step of
`tools/regress.py` (12:00 / 20:00 / 21:00 at the hospital, at 3x: no black frames), reference scenes reuse off / on at
1x and 3x 16:9 (below).

### 16. Linux on the modest-hardware laptop (2026-10-30)

First numbers of the Linux build (docs/LINUX.md): i5 Skylake (4 threads), Intel HD 520 on Mesa ANV / Vulkan, 1x
internal resolution, a 960x672 window, the title flyover and intro cutscene from an empty card.

| build | timing | game fps | host fps | game ms | main thread draw (`gl_ms`) |
|---|---|---|---|---|---|
| quick (`--opt none`) | original (30 cap) | 26.7-29.3 | 45-51 | 10-14 | 2.5-6 |
| ThinLTO (`--opt lto`, 14 m 49 s, largest process 1.3 GB) | original (30 cap) | **30.0 locked** | 43-51 | 16-20 | - |
| ThinLTO | frame-rate independent, uncapped | 28-51 | 12-17 | 12-13 | 20-47 |

Locked 30 on this machine meets the roadmap's modest-hardware bar for 30 fps. Uncapped (first look), it is the GPU: the Linux
profiler (`VCS_PROFILE_THREAD=MainThread`) puts the main thread 49% in `ioctl` (the i915 driver waiting for the GPU)
and 30% in Mesa's Vulkan driver itself. Next for Vulkan, by that profile: push descriptors only for the bindings that
changed (all 20 go with every changed draw now), barriers narrower than ALL_COMMANDS, and the textures gs_hw makes and
drops each frame (`mmap` / `munmap` show up) kept for reuse. GPU timing per pass (`PS2X_GPU_TIMING`, D3D11 only so far)
would say which passes the HD 520 spends its time on.

**Vulkan work, measured** (same machine, the same 75 s flyover + intro, frame-rate independent timing, window visible,
no control channel - `tools/run.py` with `PS2X_PERF_LOG`; averages of the [perf] windows from 20 to 70 s):
- The first look above was skewed: the control channel turns on the brightness probe (`frameLuma`), a synchronous
  readback - a full GPU wait - every frame. Players never run it; test runs always do. Without it, uncapped: game 84 fps,
  host 29.5 fps, the main thread waiting 16 ms a frame on the GPU fence: **GPU bound**, 33.8 ms of GPU per shown frame,
  which carries three game frames' rendering (~11 ms each; bloom and trails ~2.4 ms each of those).
- **At a 60 fps cap the HD 520 holds 60**: game 59.7 fps, p95 17.0 ms, host 59.9 fps, GPU 12.8-13.0 ms a frame.
- GPU timing for Vulkan (timestamps): `PS2X_GPU_TIMING=1` as on D3D11 (gs_hw then breaks the pass around every draw,
  which on Vulkan costs a pass and two barriers per draw - use it for per-draw shares only), and `PS2X_RHI_GPU_TIME=1`:
  the passes as they are and the whole frame. `PS2X_RHI_STATS` now gives the CPU's waits (fence, acquire, submit,
  present) and the descriptor writes.
- Barriers from the image layouts (stages and accesses of what used / will use it) instead of ALL_COMMANDS / MEMORY:
  GPU 33.8 -> 33.2 ms uncapped - small; kept (`PS2X_RHI_FULL_BARRIERS=1` = before). Synchronization validation
  (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` with `PS2X_RHI_DEBUG=1`) is clean; it caught the swapchain image's
  transitions, which must chain to the acquire / present semaphores at COLOR_ATTACHMENT_OUTPUT.
- Push descriptors: only the slots that changed since the command buffer's last push: 14,499 -> 1,683 descriptor writes
  a frame at 60; main thread 6.1 -> 5.5 ms a frame (GPU unchanged).
- `spirv-opt -O` over the SPIR-V (the GS pixel shader 46.6 -> 39.5 KB): GPU 12.44 / 13.11 ms against 12.77 / 12.97 - noise;
  Mesa optimises the same. Not kept.
- At 60 the main thread now sleeps 69% of the time (frame pacing), 11% in Mesa's driver. Large textures are made ~24 times
  a minute (no churn). What is left is GPU time in the GS emulation's passes (bloom, trails, the world's draws through the
  GS pixel shader) - the renderer's work, the same on D3D11, not the backend's.

### 17. Show every frame the GPU renders (frame-rate independent timing, 2026-10-30)

The game thread may run up to three frames ahead of the main thread, and each host frame rendered every queued frame
and showed the last (gs_hw.cpp). With the GPU behind, that settles at batches of three: the HD 520 rendered 84 frames a
second and showed 30. Under frame-rate independent timing game time follows the clock, so a game thread that waits
loses nothing: `gsHwSetShowEveryFrame` (vcs_framerate.cpp turns it on with that timing) queues one frame and renders
and shows one per host frame. Original timing keeps the old pacing (game time there is frames: waiting would slow the
game, as the PS2 did, where dropping shown frames keeps its pace). `PS2X_HW_QUEUE=<n>` / `PS2X_HW_BATCH_MAX=<n>` (0: all)
override either. Laptop, quick build, flyover + intro, window visible:

| | game fps | shown fps | p95 ms | GPU ms / shown frame |
|---|---|---|---|---|
| before (queue 3, whole batch) | 84.0 | 30.2 | 19.0 | 33.4 |
| batch 2, queue 3 | 81.2 | 42.6 | 17.3 | 23.2 |
| batch 1, queue 3 | 75.3 | 76.5 | 16.2 | 12.6 |
| **batch 1, queue 1 (now)** | 74.9 | **76.5** | 16.3 | 12.5 |
| now, 60 cap | 59.3 | 60.1 | 16.9 | 12.2 |
| original timing (unchanged) | 29.9 | 60.2 | 33.4 | 6.5 |

The ~10% fewer game frames were frames never shown. Queue 1 shows the newest frame (least input delay) at the same
rate as queue 3. On Windows it matters wherever the GPU is the limit (3x / 4x internal resolution).

### 18. The GS pixel shader built per texture read (both renderers, 2026-10-30)

The GS draw pixel shader (gs_draw.hlsl) chose its texture read per draw from the constants: none, nearest, the GS's
bilinear by four loads, the hardware sampler, Gather, a render target read 1:1 at full resolution. Every draw paid the
registers of the largest path (the render-target and four-load reads), the effect passes (bloom, trails: render targets)
most. Only Gather had a variant of its own (section 15). Now `SAMPLE_MODE` 1-6 builds one variant per read (7 x 2 with
the dual-source output; 0 is the full shader) and gs_hw.cpp's `drawSampleMode` picks it from the same constants the full
shader branches on, in the same order - the same code per draw, so the same pixels. `PS2X_HW_SPECIALISE=0` = the full
shader for all but Gather, as before. The variants are 5-22 KB of SPIR-V against the full shader's 47 KB.

Laptop (HD 520, Vulkan), quick build, flyover + intro, frame-rate independent timing, pairs run alternately:

| | GPU ms / frame, 60 cap | bloom ms | trails ms | uncapped shown fps | GPU ms / frame uncapped |
|---|---|---|---|---|---|
| full shader | 11.75 / 12.90 | 2.23 / 2.34 | 2.19 / 2.61 | 76.2 / 76.5 | 12.59 / 12.61 |
| per read | 11.39 / 10.81 | 1.91 / 1.84 | 1.69 / 1.60 | **83.0 / 82.9** | **11.66 / 11.38** |

Pictures: `scenes.py capture` with and without (original timing): the scenes that show the same frame in both runs -
loading, the flyover road (the world and the post effects), the pause map - are identical (mean difference 0.00); the
moving ones differ less than two runs of the same build do. On Windows (D3D11, fxc) the same variants are built; there
the gain wants measuring at 3x / 4x, where the GPU counts (and a picture check: `scenes.py compare`).

### 19. Input delay (2026-10-30)

The user felt more input lag after section 17. Measured now, not guessed: `PS2X_RHI_STATS=1` reports **input to GPU
done** per shown frame (Vulkan) - from the input snapshot the game read (`platform::inputSnapshotNs`, stamped on the
pad read and on the frame) to the GPU's end of that frame on the CPU's clock (`VK_KHR_calibrated_timestamps`); the
display adds up to a refresh. gs_hw splits the part before the GPU: input -> queued (game, render worker, GS stage) and
queued -> taken (waiting for the main thread). Laptop (HD 520), quick build, flyover + intro, frame-rate independent:

| | old pacing | section 17 | now |
|---|---|---|---|
| 60 cap: input -> GPU done | 44.5 ms | 50.4 ms | **33.9-35.0 ms** |
| 60 cap: game fps, p95 frame | 59.5, 16.8 | 59.7, 16.9 | 58.7-58.9, 17.1-18.2 |
| 144 cap (GPU bound here) / uncapped | 84 ms, 30 shown fps | 45 ms, 76-80 | **44.6 ms**, 80.9 |

What was wrong, in order:
- **Two clocks.** The game slept to its own deadline, the main thread to its own; a finished frame waited for the next
  host frame - on this machine almost a whole one. Now the main thread's frame paces the game's
  (`ps2HostTick` / `ps2WaitHostTick`, ps2_present.h): at a cap of 60 or more (or none) with frame-rate independent timing.
- **A standing backlog.** Once one frame queued behind another, production and consumption at the same rate kept it
  there for good - every frame a host frame late (input -> queued and queued -> taken both exactly one period). A tick
  counts the frames on their way (posted by the game, not yet taken); with a backlog, at a cap the tick starts nothing (the
  next one starts in time again), GPU or CPU bound it goes when the waiting frame is taken (that frame keeps the GPU fed:
  dropping it measured 36 ms but 77 fps and a p95 of 18.9 ms - judder).
- **Late start.** The game's frame (11 ms here) plus the render worker's and GS stage's tail (5.6 ms) just missed a
  60 Hz host frame. The **frame lead**: the main thread wakes before the next host frame by the frame's build time (the
  95th percentile of recent frames' input -> ready, `gsHwFrameBuildNs`) plus 1.5 ms, refreshes the input
  (`pumpEvents(false)`: key-press edges kept for the frame-start pump, so hotkeys miss nothing) and starts the game's
  frame then: it is ready just in time, on input read just before it started.
- Tried and dropped: a hand-over wait at the GS stage (stalls the pipeline, not the game: 70 fps); a frames-in-flight
  limit alone (1 starves the pipeline - 51-54 fps -, 2-3 change nothing); releasing a backlog's tick late (the backlog
  comes back).
Switches: `PS2X_HOST_PACED=0` (the game's own deadline), `PS2X_FRAME_LEAD=0`, `PS2X_FRAME_LEAD_MARGIN_US=<us>`.
Original timing is paced by the emulated VBlank and unchanged (49.8 ms at 30 fps). What is left at a cap: the frame's
build (~10 ms) and the GPU (~11 ms + recording). On Windows (D3D11) the pacing is the same code; DXGI's default of
three queued frames might add delay when GPU bound there - to measure (the delay report is Vulkan's only so far).

### 20. Linux: PGO, and the write watch (2026-10-30)

- **PGO** (`build.py --opt pgo`): training no longer needs the rig's card copies - `bench.py` starts a new game from
  an empty card when `build/rig/saves/fortbaxter` is missing (`rig.Game.new_game`: Cross until the player is outdoors),
  so the profile covers the city spots and the AI drive as well as the reference scenes (a profile from the scenes alone
  would have marked the driving code cold). Laptop, ThinLTO + PGO against ThinLTO of the same code (a second tree),
  bench alternating twice: game thread over the six spots 9.7 / 10.0 ms against 9.95 / 9.8 ms, the worker and frame
  times alike - no measurable gain here (as on Windows, section 10). ThinLTO stays the default for players.
- **The write watch on Linux** (`ps2_write_watch.cpp`): userfaultfd's asynchronous write-protect over guest RAM
  (`UFFD_USER_MODE_ONLY`, as unprivileged processes get it) and `PAGEMAP_SCAN`, which lists the written pages and
  protects them again in one call (Linux 6.7+). A prototype checked first that a kernel write (`read()` from a file into
  protected guest RAM, as the stream thread does) succeeds and counts. In the game: ~217 pages written per fold,
  0.10-0.16 ms a fold; `VCS_RSL_SKIP_VERIFY=1` over the bench (2.21 M decisions) and the reference scenes (1.35 M):
  **0 wrong**. A/B, same build, alternating twice: render worker 7.83 / 8.68 -> 7.53 / 8.20 ms, but the game thread
  9.30 / 10.30 -> 9.95 / 10.68 ms and fps unchanged: the first write to a page after each fold is a page fault on Linux
  (Windows reads dirty bits), ~220 a frame on the game thread. So it is **opt-in on Linux** (`PS2X_WRITE_WATCH=1`):
  the game thread is the longest CPU stage and on the input-delay path. Soft-dirty bits would avoid per-page work only
  by clearing the whole process's page tables at every fold.
- What is left for the game thread is guest code (64-80% of it, flat): deeper code generation in the recompiler
  (guest registers held in host registers across calls) - a project of its own.

### 21. Stutters while driving: the stutter rig, texture bursts, the level swap (2026-11-02)

**The reports.** Issue #29 (GTX 1060, 2560x1080, MSAA 4x, anisotropic 8x, 60 fps cap): hitches up to 150 ms driving
fast through the city, with the logs to show it - every episode had a burst of texture uploads (38-630 in a frame),
`wait_ms` carried the time, the stall often a frame or two after the burst. Issue #16 (RTX 4070): micro-stutter
whatever the timing and cap, gone under DXVK. Nothing more was found online.

**The rig: `tools/cruise.py`** (the user asked for one that travels through the city at driving speed). The rig's
`cruise` command (src/debug/rig.cpp) puts the player's car, every game frame, at the point of a route `speed x step`
further on, facing along it, with that move speed - streaming, the camera and the renderer get the same load every
run. The default route (tools/routes/city.txt) is a low flight, 35 m over the roofs, through the busiest cells of the
world tour on both sides of the bay: ~6 km at 40 m/s (144 km/h), 140 s, seven Beach / Mainland swaps. Routes from AI
drives were the plan (`cruise.py record`), but the AI cars the rig makes stood more than they drove. The game writes
its per-frame CSV; the report counts hitches (a frame over twice the running median and 8 ms over it), frames over
25 / 33 / 50 / 100 ms, and gives each hitch's texture uploads, decode / create times and the main thread's longest
host frame (new CSV columns `tex_create_us`, `tex_kb`, `tex_decode_us`, `host_max_ms`). The control channel's
brightness probe is off for it (`PS2X_FRAME_STATS=0`): its read-back waits for the GPU every host frame.

**What the hitches were** (the rig, then `VCS_PROFILE_SPIKES`, `PS2X_WAIT_LOG`, `VCS_TRACE_DELAY`):
- **Texture bursts**: a new area brings 100-400 new textures in one frame. The GS stage decoded each - a paletted one
  through the CLUT addressing and TEXA per texel - and the main thread made each one between its draws; with
  anisotropic filtering each was a render-target texture with a `GenerateMips` pass. A 399-texture burst: 52 ms of
  texture creation on the main thread, its longest host frame 77 ms, the game frame 144 ms.
- **The level swap** (the largest, 150-450 ms, all of it the game thread idle): crossing between Beach and Mainland,
  FUN_003C6D08 stops the music and waits in `DelayThread(1000)` steps until the stream says stopped - which the IOP's
  music thread only sees at its next refill IRQ, up to 448 ms later (game-internals.md 9a). The PS2 stood just as long.

**What was done** (each with a switch back):
- Paletted textures decode through a 256-entry table of the converted texels (`GSCpuBackend::paletteTable`; the same
  values - `PS2X_HW_PALETTE_TABLE=0`).
- Anisotropic filtering's mip chain is built on the CPU and the texture made with all its levels in one creation
  (`createTextureLevels`, immutable, no render target; `PS2X_HW_CPU_MIPS=0`).
- D3D11: new textures are made by two helper threads (the device is free-threaded), the mip chain with them; the main
  thread takes each at its `TexCreate` (`PS2X_HW_EARLY_TEXTURES=0`). Vulkan keeps its creation on the main thread (its
  first contents go into the frame's set-up commands).
- The native music driver runs the music thread's step at once for a stop while the stream plays, so the level swap no
  longer waits up to 448 ms for an IRQ (`VCS_MUSIC_STOP_WAIT=1`; the sound replay comparison keeps the IRQ timing).

**Result** (the cruise, `--opt none`, frame-rate independent timing at the 60 cap; all four switches off vs on,
GTX 1080 Ti / i7-7700K):

| | fps | p99.9 ms | max ms | frames > 33 / 50 / 100 ms | hitch time lost a minute |
|---|---|---|---|---|---|
| 3x 16:9, MSAA 4x, AF 8x, before (two runs) | 59.1 / 59.2 | 68.3 / 67.3 | 400 / 365 | 27 / 11 / 5, 23 / 12 / 6 | 799 / 779 ms |
| 3x 16:9, MSAA 4x, AF 8x, now | 59.9 / 59.9 | 33.4 / 33.0 | **46 / 48** | 9 / 0 / 0, 9 / 0 / 0 | **86 / 72 ms** |
| defaults (1x 4:3), before | 59.4 | 35.8 | 448 | 15 / 4 / 4 | 567 ms |
| defaults, now | 59.9 | 31.7 | **45** | 6 / 0 / 0 | **51 ms** |

Pictures: reference scenes with the four switches off vs on - the static scenes identical (0.00), the moving ones
within run-to-run noise; anisotropic 8x, mip chains by `GenerateMips` vs on the CPU: static scenes 0.00, the moving
ones 0.00-0.5 apart from the flyover's tower (timing).

**What is left on the route**: the level swap's own load, one game frame of ~36-46 ms (the .LVZ, its inflate and setup)
per crossing; the largest texture bursts still cost the GS stage ~10-20 ms of decoding (the frame ~45 ms); the rest
of the drive holds 16.7 ms.

### 22. VU0 natives: what repeats out of the loops (2026-11-02)

The uncapped cruise (`tools/cruise.py run OUT --fps 0`, `VCS_PROFILE=10`) puts the VU0 natives at ~9.4% of the game
thread: `matrixTimes3From` 3.7%, `maddBc` 3.4% (called by both), `boxClip` 2.3%. Each multiply-add re-loaded and
re-cleaned (VU operand rules) its matrix row, the broadcast input lane and ACC from memory. The 3x matrix product and
the sector box clip now clean their constant inputs once per call and each matrix row once per matrix, and keep ACC in a
register (stored once at the end); every operation is the same one, ACC still cleaned as an operand.
`PS2X_VU0_VERIFY=1` over a whole cruise: 5.06 M matrix products and 5.56 M box clips, 0 differed. Game thread: 9.4% ->
7.9% for the three; the uncapped cruise 168.3 -> 171.1 fps (`--opt none`).

### 23. The script VM's CollectParameters natively; the RSL skip test's lookups (2026-11-02)

The same uncapped profile, next items on the game thread:
- **FUN_00254B90, CollectParameters** (game-internals.md 11c): the script VM decodes every command's typed arguments
  through it (1,100 call sites; 1.4% of the game thread in guest code). Natively in `src/game/vcs_overrides.cpp`
  (`collectParamsNative`): the same reads of ScriptSpace and the script's locals, FUN_00254FF8's variable rules (timers,
  locals, local / global arrays with the dummy word for an index out of range), the same `*ip` and `$v0`; anything
  outside main RAM runs the original. Check: `VCS_NATIVE_PARAMS_VERIFY=1` lets the original run and compares its words
  with the native decode at the next call - words 1.. only, since a command often writes its result into word 0 (a new
  handle) before the next call. A cruise: 24 M calls, the types checked there int8 3.3 M, int16 0.1 M, packed floats
  2.2 M, zero 2.7 M, locals 1.7 M, globals 0.3 M, global arrays 2.4 M, strings and int32 a few hundred; 40 differed, all
  early, two call sites whose command writes its outputs into the block (a float result, a counter). 1.4% -> 0.6%:
  below what an A/B of the whole frame resolves (game thread 4.79 / 4.80 ms off vs 4.76 / 4.86 on), kept as measured
  in the profile. `VCS_NATIVE_PARAMS=0` = the original.
- **rslworld's `skippable`** (2.4%): per marked block (~2,000 a frame) two `std::unordered_map` lookups and an
  out-of-line call into the runtime per page for its write-watch generation. Now an open-addressing table
  (`FlatMap`: lookups and inserts only) and `ps2writewatch::generation` / `unchangedSince` inline over an exported view
  of the generations: 2.4% -> 1.3%. `VCS_RSL_SKIP_VERIFY=1 VCS_RSL_WORLD_LOG=1` over a cruise: 2,163,873 skip decisions
  would have replayed their recording rightly, 0 wrongly.

Uncapped cruise after 22 and 23: 182-185 fps (from 168), game thread p50 5.2 ms.

**The player's build** (ThinLTO, `--opt lto`; sections 21-23 all off by their switches vs on, the 60 cap):

| | max ms | frames > 33 / 50 / 100 ms | hitch time lost a minute |
|---|---|---|---|
| defaults, before / now | 370 / **45.6** | 17 / 4 / 4 -> 5 / 0 / 0 | 546 / **47 ms** |
| 3x 16:9, MSAA 4x, AF 8x, before / now | 346 / **45.0** | 20 / 9 / 5 -> 7 / 0 / 0 | 666 / **58 ms** |

Uncapped (defaults): 188 fps, frame p50 5.1 ms.

### 24. The level swap's inflate: a 64-bit bit reader, short copies (2026-11-02)

After sections 21-23 the cruise's only hitches on the ThinLTO build are the four Beach / Mainland swaps: one game frame
of 36-41 ms each, about half of it zlib's `inflate_fast` (native since section 14): ~5 MB per swap in ~150 calls,
~20 ms - 247 MB/s. Tried, each exact:
- **A 64-bit bit buffer** refilled 8 bytes at a time at the top of each symbol (a symbol takes at most 48 bits), with
  zlib's own `in` / `bits` still counted byte for byte as zlib pulls them - only counted. zlib's state after a call
  (next_in, bits, the unconsumed bits in hold) follows from the bits consumed, and its loop test reads its own input
  pointer, which is the counted one: so every call leaves zlib's state, not only the same output (`decodeT<Fast>` in
  `src/game/vcs_inflate.cpp`). 247 -> 267 MB/s.
- **Short copies** (most matches are 3-32 bytes) as two fixed-size moves of the first and last bytes instead of a
  `memcpy` call; a repeat at distance 1 as a fill; at distance 8 or more in 8-byte steps (each reads bytes already
  written). Exactly the bytes zlib's byte loops write. 267 -> 270 MB/s.

Check: `VCS_NATIVE_INFLATE_FAST_VERIFY=1` decodes every call with both readers into their own copies of the output and
compares the bytes and the whole state left: a cruise (boot, the swaps, streaming) 1,200+ calls, 0 differ.
`VCS_NATIVE_INFLATE_FAST=0` = the byte-wise reader. The gain is small - about 2 ms of a swap's 40 ms frame: the time is
in the per-symbol decode itself (a literal per iteration, the literal / match branch), where zlib's own bookkeeping has
to run per symbol to stay exact. A bigger step would be multi-symbol tables (libdeflate's), checked the same way.

### 25. Linux: the stutter work on Vulkan (2026-11-02)

Sections 21-24 were made and measured on Windows (D3D11). On Linux:
- **Portable, so already in the Linux build**: the palette table, the CPU mip chains (`createTextureLevels`, implemented
  for Vulkan in 21), the level swap's music stop, the VU0 natives, CollectParameters and the inflate reader - plain C++,
  no platform code. The RSL skip test (23) only runs with write watch, opt-in on Linux (section 20).
- **Texture helper threads (21) were D3D11 only.** Vulkan now has free-threaded creation too: off the recording thread
  the image and its views are made and the pixels copied into a staging buffer of their own (`stage`); the recording
  thread puts the copies into the set-up commands at its next submit (`drainStaged`), before anything recorded can use
  the texture. A texture released off that thread is never bound: its frees wait (`Core::orphans`) for the next submit
  and go with that frame. Three cruises with it on: no Vulkan errors in the logs.
- **Measured** (the cruise from a copy of the player's card - the rig's `fortbaxter` card is on the Windows machine only;
  quick build, defaults, i7-6600U / HD 520, three runs each, alternated, the laptop warming up over them: hitch time
  818-1,576 ms a minute either way). What holds in every run: in frames with 50+ new textures the main thread's
  longest host frame is ~3 ms shorter with the helpers (27-29 vs 30-32 ms) - its creation cost was ~8 ms a 350-texture
  burst, against D3D11's 52 - while the GS stage's decode is ~2.5 ms longer (7.4-7.9 vs 3.9-6.0 ms: two helpers on a
  2-core CPU take it from the game's threads), and such frames over 20 ms: 23-29 with vs 20-23 without. No gain here,
  so with Vulkan the helpers are off unless asked for (`PS2X_HW_EARLY_TEXTURES=1`); D3D11 keeps them on. To measure on
  a 4+ core machine before changing that default.
- **What the cruise shows on this laptop**: the largest hitches are game-thread frames of 58-76 ms with nothing to
  upload - the level swaps on a slower CPU (Windows: 36-46 ms) - and frames held behind the GPU (`wait_ms` 30-60).

### 26. Linux: hitches driving in the city (2026-11-02)

The user felt hitches driving around the city on the laptop (i7-6600U, 2 cores / 4 threads, HD 520, 15 W shared by
both; the 60 cap with frame-rate independent timing, 1x, the window maximised at 1920x993), apart from the level swaps.
The cruise from the player's card shows them: 120-230 frames over 33 ms in 140 s.

**What they are.** ~70% of the slow game frames are 33.3 ms (one 60 Hz slot missed) with the game thread only ~11 ms
busy and ~20 ms waiting for its tick: the main thread's host frame ran over 16.7 ms. New: `PS2X_HOST_SPIKES=<ms>` (the
main loop's phases and its CPU time per slow frame; Vulkan: the frame's waits, draws, binds, pushes, CPU time in
`prepareDraw` / `vkCmdDraw`). In those frames the main thread either
- spends 16-24 ms of its own CPU executing one game frame (`UploadFrame`: wall ~ CPU, so not blocked or preempted) -
  dense views, 1,300-2,200 draws at ~4.5 us each, almost all of it in Intel's driver (`vkCmdDraw` ~2 us, a pipeline bind
  ~2.5 us; 40-60% of draws change pipeline), or
- waits 10-20 ms for the GPU fence: the GPU's frame averaged 13-15 ms (`PS2X_RHI_GPU_TIME`), so heavier views pass
  16.7. At 2x the game's frame takes 3.6x as long: the GPU is bound by pixels, not draws.
Ruled out on the way: device memory churn (`PS2X_RHI_LOG_MEMORY=1`: 6 allocations a run, all at boot), new pipelines,
readback waits, swapchain acquire, texture creation (section 25), the specialised shaders' pipeline count
(`PS2X_HW_SPECIALISE=0`: the same binds per draw).

**What was done** (each with a switch back):
- **No shader depth on Vulkan.** The GS pixel shader wrote its depth (SV_Depth) for every draw - GL's fix for its NDC
  precision (PS2RECOMP_PATCHES.md), not needed with D3D / Vulkan's 0..1 depth: the rasterised value is the same vertex
  depth interpolated linearly in screen space. A shader depth turns early depth rejection off, so every hidden pixel of
  the city's overdraw was shaded and textured. Pixel shaders without it (`drawPSNoDepth`, SHADER_DEPTH=0): the game's
  frame 10.7 -> 9.3 ms of GPU. Reference scenes: static ones as before (0.02, and the map's player arrow), moving ones
  within run-to-run noise, the ground decals intact. Default with Vulkan; on D3D11 since 2026-11-03, when the shader
  depth turned out to hide the game's fire (game-internals.md 6m; `PS2X_HW_SHADER_DEPTH=1` / `0` forces either way).
- **Constants written only when they change** (`gs_hw` `updateConstants`): 45% of the draw constants' and 63% of the
  native constants' writes were the same bytes as the draw before; each was a ring slice and a descriptor push.
  Descriptor writes ~2,900 -> ~1,700 a frame. `PS2X_HW_CB_DEDUPE=0`.
- **Redundant state not recorded** (Vulkan): viewport, scissor, blend constants and vertex / index buffers only when they
  differ from what the command buffer has (the renderer sets them before every draw; each recorded one made the driver
  send that state again). ~9% less CPU per draw in the slow frames. Static scenes identical (0.00). `PS2X_RHI_REDUNDANT=1`.

**Result** (the same binary, all three off vs on, two runs each alternated, the laptop warm): GPU frame 13.2 / 13.1 ->
11.9 / 11.9 ms; the main thread's frame execution (`gl_ms`) 9.1 / 9.7 -> 8.3 / 8.3 ms; frames over 33 ms 191 / 228 ->
189 / 172. Real, but the hitches remain: in the dense views the laptop's CPU and GPU are both at the 60 Hz budget.

**Next, for this class of machine:** the driver's per-draw CPU on the main thread - fewer pipeline variants (blend and
depth state as dynamic state, VK_EXT_extended_dynamic_state3), textures through one descriptor array instead of a push
per draw, or the frame's commands recorded off the main thread; on the GPU, the window pass (1.2-1.5 ms at 1920x993)
and the bloom / trails (~3.7 ms).

### 27. Linux: the main thread's Vulkan cost per draw (2026-11-02)

After section 26 the main thread's slow host frames were mostly its own CPU in Intel's driver (~4.5 us a draw). What
changes between draws (`PS2X_RHI_STATS=1` now splits pipeline binds and descriptor writes): 450-650 pipeline binds a
frame, two thirds of them only the depth state (test / write / compare), the rest shader changes (the vertex kinds and
dual-source variants - `PS2X_HW_SPECIALISE=0` changes nothing); ~1,000 descriptor writes (two thirds constant buffers).
Mesa 25.2 here has extended dynamic state 1-3, shader objects and descriptor buffers.

- **Depth state as dynamic state** (core Vulkan 1.3): pipelines that differ only in depth share one VkPipeline (the
  device's cache keyed by everything else), and the depth state is set per draw when it differs from what the command
  buffer has. No change in gs_hw. Binds 450-650 -> 140-270 a frame, nearly all of them shader changes now.
  `PS2X_RHI_STATIC_DEPTH=1` = the depth state in each pipeline.
- **Vertex buffers copy what was written**: the renderer's dynamic vertex buffers are sized for the largest frame seen,
  and each frame's ring slice copied the whole capacity. Now the bytes the last update wrote (WRITE_DISCARD: the rest is
  undefined; constant buffers stay whole - their descriptors cover the buffer). Uploads 3.3-4.2 -> 1.4-2.9 MB a frame,
  the binding step 1.0-1.5 -> 0.4-0.7 ms. `PS2X_RHI_FULL_COPY=1`.
- New `VCS_PROFILE_RAW=<file>` (Linux): the samples dladdr cannot name, for llvm-symbolizer - gs_hw and the RHI live in
  anonymous namespaces. The main thread's busy samples now: Intel's driver 46%, libc (copies) 25%, ours 28% - of that,
  `RhiExecutor::draw` 39% (mostly its `m_meshes` / `m_textures` lookups: node-based maps of thousands), `prepareDraw` 25%.

Checked: Vulkan validation over the reference scenes (no errors; the no-depth shaders' unread vertex output is a
performance warning), scenes against section 26's: static ones and two of the three flyover shots identical (0.00).

**Result** (the same binary, both off vs on, two runs each alternated): the main thread's frame execution (`gl_ms`)
8.48 / 8.47 -> 7.09 / 7.49 ms; frames over 33 ms 86 / 89 vs 82 / 96 - at the laptop's state that day the hitch count did
not separate. With `PS2X_HOST_SPIKES=20`: main-thread frames over 20 ms 61 -> 24 (its own CPU 39 -> 14, the GPU fence
12 -> 7). What remains of the ~70-90 frames over 33 ms: frames where the game thread itself takes 24-26 ms (guest code),
texture bursts (~30: the GS stage's decode), and main-thread frames just over 16.7 ms.

**Next here**: the per-draw map lookups (a flat table, or the resolved pointer carried in the command), the constant
buffers' descriptor pushes (dynamic offsets in a set of their own: the SPIR-V's set for cbuffers, a set cache), and the
game thread's own slow frames (`VCS_PROFILE_SPIKES`).

### 28. Linux: the game thread's slow frames (2026-11-02)

After section 27 the cruise's slow frames on the laptop include 40-75 a run where the game thread itself takes 24-26 ms
(the CSV's `game_ms`, the host-tick wait not in it). Looked at with the profiler's spike mode and `VCS_PROFILE_RAW`:
- They are not waits for the render side: `PS2X_RENDER_THREAD_LOG=1` shows no fences and almost no full queues;
  `PS2X_WAIT_LOG=5` shows no scheduler idling during the drive. Their samples are a flat spread of game code (streaming in
  a new area, building texture uploads, game logic) with our host code on top - no single cause.
- **A profiler bug (Linux, fixed):** a host function dladdr cannot name - a hook in an anonymous namespace installed
  with replaceFunction - was taken for generated code with a 1 MB window, so the host code laid out after a hook was
  charged to the hooked guest function. `FUN_003298D8` (texture upload REFs) showed 5.3% of the game thread that way;
  its hook really costs ~270 cycles a call (~0.1 ms a frame). Generated functions are exported (-rdynamic), so an unnamed
  one is never generated code now. With that, the game thread's samples on the cruise: guest code 27%, our host code 37%
  - the VU0 natives `boxClip` / `matrixTimes3From` (~1,100 cycles a call, ~11 M calls each a run: ~0.8 ms a frame) and the
  RSL observer (`closeBlock`, `observe`, `flattenBlock`, `settleRegion`, the page snapshots) about equal - and waits.
- **Tried, not kept:**
  - `FUN_003298D8` natively while a material cache is filled (40-70k calls a second, all going to the original): the
    same bytes (3.2 M calls compared as the original returned; game-internals.md 6i has what it writes) but the cruise
    hitched more - frames over 33 ms 97-126 against 52-76 in four runs each way, both orders, isolated from the other
    change. Most likely the emulated EE clock: the recompiled code advances it as it runs, and ~200 guest instructions
    55k times a second no longer did, which moves timers / interrupts against the game's work. Average game time did not
    drop either.
  - The page snapshots skipped without write watch (they only serve the skip test, off on Linux): no change in game time
    or hitches.

**End to end, sections 26-27 together** (2026-11-03, the same binary with all their switches back vs the defaults, two
runs each alternated, the laptop cool): frames over 33 ms 61 / 61 vs 66 / 65, over 50 ms 12 / 12 vs 11 / 14; GPU frame
13.7 / 13.4 -> 11.9 / 11.9 ms; the main thread's frame execution 7.3 / 7.3 vs 7.4 / 7.1 ms; game thread 10.0 vs 9.8. So the
GPU saving holds; the main-thread saving of 27 (8.5 -> 7.1-7.5 ms) showed only with the laptop warm, when the CPU was the
limit; the hitch count follows the machine's state (133 the first morning, 120-230 warm, ~60 cool) more than these
changes.

**Next here**: the VU0 natives' remaining cost (register copies in and out, the per-operation VU float rules), the RSL
observer's per-block work, and natives that keep the guest clock's accounting (a native step must charge the cycles the
original would have run, or the game's timing moves - worth checking for the natives already in).

### 29. Optional PCSX2 PNG texture packs (2026-10-31)

The shared texture cache can replace a sampled texture with a PCSX2-named PNG (`docs/TEXTURE_PACKS.md`). Pixel
and palette hashing only runs for cache misses whose format/size occurs in the pack; matches/misses are memoized.
PNG decoding is on a worker, GPU images shared by pack identity, originals used while a decode is pending.

Windows/D3D11, same quick build, two sequential `bench.py` runs from empty isolated cards, 60 fps independent
timing, VSync off, 1x, MSAA/anisotropy off, default 512 MiB replacement cache. Off -> on, milliseconds:

| Spot | Game | Render worker | GS stage |
|---|---|---|---|
| Fort Baxter | 3.88 -> 4.03 | 1.55 -> 1.52 | 1.20 -> 1.30 |
| City 575,175 | 2.77 -> 2.98 | 0.50 -> 0.50 | 0.40 -> 0.47 |
| City -675,1425 | 5.03 -> 4.65 | 1.00 -> 0.82 | 0.80 -> 0.70 |
| City 1325,-325 | 3.62 -> 3.80 | 2.73 -> 2.88 | 3.05 -> 3.42 |
| City -1425,-575 | 4.05 -> 3.83 | 0.53 -> 0.50 | 0.47 -> 0.47 |
| AI drive | 5.50 -> 5.58 | 3.32 -> 3.32 | 2.95 -> 2.97 |

Both runs held 60.0 fps at all spots (16.7 ms). This is one capped pair with changing traffic and the first mission
running, not a measurement of maximum frame rate or a precise overhead claim. Outputs:
`build/bench/texture_pack_final/{0,1}/bench.json`. The original pre-change empty-card benchmark is discarded:
`rig.Game.new_game()` previously accepted the title flyover's area 0 and advancing frames before a player existed.

A 64 MiB cache stress capture exposed eviction on empty alternate VBlanks at original 30 fps: active HUD images
were repeatedly decoded (938 loads for 25 unique files over the scene sequence). Eviction now protects the current
and two preceding frames. The targeted five-location 64 MiB / bilinear check loads 25 distinct files, 58 image
loads including later reloads after changing views, no per-VBlank churn or failures. The active working set is
protected even when it exceeds the budget. All 160 PNGs decode in the standalone check; pack pixels total 805 MiB
at RGBA8 before mips, so loading on demand matters.

### 30. The player's mods while driving: larger streaming memory, VC cars, an HD pack (2026-11-03)

The player saw the game lose a lot of speed with their mods (FORK.md T10). `cruise.py run` (144 fps cap, D3D11, the
quick build, `build/rig/saves/fortbaxter`), each from its own card copy, the mods through test folders of junctions
(`build/bench_mods` all, `build/bench_cars` the VC cars only, `build/bench_tex` the HD texture pack only):

| Run | fps | p95 ms | p99 ms | hitches | excess ms / min |
|---|---|---|---|---|---|
| no mods, PS2 streaming memory | 142.3 | 7.2 | 9.3 | 20 | 160 |
| no mods, 30 MB streaming memory | 141.7 | 7.3 | 10.8 | 30 | 200 |
| the VC cars (Fixed Xbox Vehicles) | 132.5 | 11.4 | 16.4 | 160 | 854 |
| the HD texture pack | 134.5 | 10.7 | 15.7 | 207 | 1100 |
| the HD texture pack, its images made off the main thread | 140.6 | 7.4 | 13.3 | 31 | 227 |
| all the player's mods, the same | 131.1 | 12.2 | 16.4 | 163 | 880 |

- **The streaming memory itself costs nothing measurable** (the 30 MB run is within the runs' noise).
- **The HD pack's hitches**: its DDS images (with their mip levels, 16 with anisotropy) were created on the main
  thread when the GS stage first met the texture. The pack's ready images now go through the same early-creation path
  as decoded textures (`gs_hw.cpp` `makeReplacementEarly`, the helper threads; PS2RECOMP_PATCHES.md): 207 -> 31
  hitches, p95 10.7 -> 7.4 ms.
- **The VC cars** stay the cost: the main thread's frame phase rises to ~11 ms in traffic, ~7 us per draw against 4.5
  for VCS's cars - VC's models have many small materials (one draw and one texture bind each; the conversion already
  merges parts sharing a texture and colour). Next: fewer state changes per car (a texture atlas per converted car),
  measured the same way.
- Outputs: `build/cruise/t10_{nomods,30only,cars,cars_noaa,tex,mods}`, `t10b_{tex,mods}` (after the pack change).

### 31. PC mods: their conversion at boot, an HD weapon's frame cost (2026-11-03)

- **Conversion at boot** (src/game/vcs_stream.cpp): the mod loader converts PC `.dff` / `.txd` once, on the stream
  thread, before the game's first read, while the game waits. One file at a time, the player's mods took ~5.3 s
  (160 car files ~4.1 s, 22 weapon files ~1.2 s). Now they convert on every core first (`[mods] N PC file(s) ready on
  T thread(s)`): 194 files in **1.7 s on 16 threads**. The converters share no mutable state and the disc image is
  mapped memory, so concurrent reads are safe.
- **An HD weapon in view** (`HD Weapons`; Vic holding it, the free camera 1.7 m away, uncapped, D3D11, quick build):

  | Weapon | fps | frame ms | game ms | worker ms | GS ms | draws |
  |---|---|---|---|---|---|---|
  | VCS's minigun / HD (12.5k triangles) | 180.7 / 154.9 | 5.56 / 6.62 | 4.92 / 5.40 | 4.04 / 4.98 | 3.94 / 5.02 | 1767 / 2158 |
  | VCS's chainsaw / HD (12.2k triangles) | 188.2 / 134.6 | 5.32 / 7.48 | 4.66 / 5.80 | 3.80 / 6.32 | 3.82 / 5.86 | 1777 / 2668 |

  The cost is the strips, then the draws. The HD chainsaw is faceted: every triangle has its own normals and UVs, so
  no two triangles share a vertex. Its 12,220 triangles become strips of one triangle each, 61,098 strip vertices
  (5 per triangle); welding identical vertices first would save 6 %. The PS2 meshes are batches of ~70 vertices, and
  each batch is one native draw with its own mesh and constants: +891 draws, ~2.4 us each over the game thread
  (decoding), the GS stage and the worker. The VC cars pay the same way (section 30). Next, in the native path: one draw
  for consecutive batches of one mesh with the same state and constants (their meshes joined, or one buffer per
  element), measured here and on the cruise.
- Since then (2026-11-03): the rasterised depth on D3D11 (game-internals.md 6m) took most of the HD weapons' cost - it
  was GPU overdraw without early depth rejection: chainsaw 208 -> 189 fps, minigun 188 -> 172 (was 188 -> 135 and
  181 -> 155); the joined draws (section 32) the rest of the per-draw cost.

### 32. Native draws joined (2026-11-03)

On this machine (Ryzen 7 5800X, Radeon RX 7700 XT) the VC car mods cost the main thread, not the GPU: a profile of
MainThread on the cruise had ~40% in AMD's D3D11 driver and the kernel's submit / sync calls, the draw executor 4%. The
native draws are one per VU1 batch (~70 vertices); counted on the cruise, **50%** had the same state, texture, kind and
constants as the draw before - the batches of one mesh of one model. `gs_hw.cpp` (SubmitNative) now joins those: while
nothing else is recorded after a native draw, the next one of the same state and constants joins it, the strips
linked by two repeated vertices (degenerate triangles; the rasteriser does not cull); the joined mesh is cached by its
parts' keys, its creation reserved ahead of the draw and filled when the group ends (or left empty when cached).
`PS2X_HW_NATIVE_MERGE=0` draws one per batch.

| Cruise (ThinLTO) | fps | p95 ms | p99 ms | hitches | worst ms |
|---|---|---|---|---|---|
| VC cars, 144 cap, one draw per batch | 130.9 | 11.8 | 17.7 | 199 | 107.8 |
| VC cars, 144 cap, joined | **138.0** | **8.9** | **13.9** | **44** | **37.0** |
| no mods, uncapped, one per batch / joined | 192.1 / 199.2 | 8.4 / 8.2 | 10.5 / 9.3 | | 59.5 / 34.4 |

(No mods, uncapped, run to run: 179-200 fps either way.) Reference scenes with and without: the static ones identical
(loading 0.01, pause map 0.00), the moving ones within run-to-run noise; `fire_check` passes. The player, playing it:
"o jogo melhorou muito os engasgos, praticamente inexistentes".

### 33. Object distance (2026-11-03)

Graphics > Draw distance > Object distance (game-internals.md 9h) scales the entities' LOD and stream distances; the
city's sectors are not entities and cost the same. The cruise (no mods, uncapped, ThinLTO, one run each):

| Cruise | fps | p50 ms | p95 ms | p99 ms | hitches | worst ms |
|---|---|---|---|---|---|---|
| Object distance 100 % (the game's) | 189.0 | 4.86 | 8.26 | 9.86 | 39 | 36.0 |
| Object distance 400 % | 177.5 | 5.23 | 8.46 | 10.64 | 53 | 41.0 |

~6% at the maximum - more entities through SetupEntityVisibility and drawn (at Vice Point beach the visible list
grows from 1 to 17-20), and more models streamed (texture uploads the same, 168 a second).

### 34. Dense crowds (T2's population, 2026-11-04)

The player's route (Viceport docks -> Little Havana -> Downtown and back, every population slider at 100, uncapped,
ThinLTO; `build/analysis/t2_scratch/route_probe.py`) ran Little Havana and Downtown at 33-63 fps: the game thread 15-27 ms,
the render worker 8-16 ms but ~58 % idle. An inclusive profile (`VCS_PROFILE_INCLUSIVE=1`, new) put CWorld::Process at
~49 % of the samples: collision ~18 % (CPhysical::ProcessCollision `FUN_00348d40` -> `FUN_0034fd60` ->
`FUN_002b8ed0`), the random peds' ProcessControl ~16 %, the clump animation update ~8.5 %, the entity render setup ~10 %.
The simulation is the game's own single-threaded code (ordered collision responses, the allocator, animation callbacks):
not something to run on several threads as it is.
- **Crowd LOD** (far random peds every second / third frame, the animation advanced by the frames skipped; the
  root-only update out of the camera's cone): up to ~10 %, often within the noise - most peds were not far.
- **The crowd around the player** was the cost: AddToPopulation's ring out of view is a fixed 10-20 m, and with the
  larger caps it took most of them (92 of 152 within 30 m). Scaled with the population distance: Little Havana
  124-127 fps (7.2 ms game), Downtown 76 fps (12.2 ms), the rest of the route 124-240 fps.
- Next, if needed: the clump animation natively and on worker threads (`build/analysis/t2_agents/anim.md`: ~0.8-1.4 ms
  in the densest frames), PGO for the release.

## Next

- **The heavy views (~120 fps)**: the game thread's chain submission there (1.4-1.6 ms: the VIF1 chain copy, the walker
  and the RSL observer, the write-watch fold) and the render worker's first stage (6 ms: RSL replay copying recorded
  unpacks into VU memory - ~12% of its busy time while driving - and the native draws' setup). Step 5 of
  docs/FULLY_NATIVE.md (state and textures as item ops) takes the chain's share down; the replay's VU-memory round
  trip needs the native handlers to take their inputs from the recordings (as the world handler's mesh already does)
  - with care for the fields an UNPACK leaves alone, which later packets read.
- **Game thread** (guest code ~64-80% of it, flat: the top function 1.4%): deeper codegen (guest registers kept in host
  registers across calls). The kick's item resolve copies (~2% driving) and the write-watch fold (~2%) are the largest
  host-side items.
- GS stage: per-upload / per-CLUT-load bookkeeping; content-derived page generations (more texture-bind memo hits).
- **Stutter, what the cruise still shows** (section 21): the level swap's one ~40 ms game frame, half of it zlib's
  inflate of the new level (`src/game/vcs_inflate.cpp`, ~275 MB/s; a wider bit buffer would be faster, but must stop
  each call at the same symbol as zlib's loop, or the outer inflate's state differs per call); a texture burst's frame
  missing its 60 Hz slot (~33 ms) when the GS stage's decode (~3 ms for 140 textures) lands on an already full frame -
  the palette mapping could move to the helper threads with the creation, leaving only the texel fetch on the stage.
- **GPU at high resolutions** (`PS2X_GPU_TIMING=1`, 3x 16:9 MSAA 4x: ~8 ms a frame on a GTX 1080 Ti, so a GTX 1060 is
  near 16.7): the trails' blend back is two full-screen passes into the multisampled frame (1.2 ms), ~3 ms falls between
  the markers (resolves of the frame for the effects that read it, copies). Changes there change pixels: a scenes
  compare at 3x with MSAA for each.
- The script VM's dispatch (`FUN_002560D8`, 1.7%): natively it would be a guest tail call into the handler, but the
  original sign-extends the handler's byte result after `jalr` - the tail call skips that, so it needs a return through
  the original's tail (0x256164) or proof that every handler returns a clean byte.
- Playtest missions above 100 fps; sprint at 1/240 s (-11%).
- The RSL recordings' memory (an LRU bound).

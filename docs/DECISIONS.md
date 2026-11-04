# Decisions log

Newest at the bottom. Each entry: what, why, and what would change it.

## 2026-10-21 - Base toolchain: vendored PS2Recomp
- PS2Recomp (ran-j, GPL-3.0) is vendored into `third_party/PS2Recomp` at upstream
  commit in `third_party/PS2Recomp/UPSTREAM_COMMIT`, without its `.git`,
  `android/` and `vita/` folders. Vendored (not a submodule) because we will patch
  the runtime heavily and can't publish a fork from here. Keep local changes
  small and listed in `docs/PS2RECOMP_PATCHES.md` so they can be upstreamed.
- The project is GPL-3.0 as a result.

## 2026-10-21 - Distribution: source only, bring your own disc
- The repo never contains the ISO, the ELF, or C++ generated from the ELF
  (`.gitignore` blocks them). Users run the pipeline on their own disc.
- The built exe reads game files straight from the user's ISO at runtime.
- Only one executable build is supported (SHA-1 in `tools/vcs_disc.py`).

## 2026-10-21 - Performance is a hard requirement
- Upstream runtime uses raylib + a CPU software GS renderer, which the upstream
  README calls "very bad" for speed. That's fine for bring-up only.
- Target: runs full speed on old/modest hardware (dev box: i7-7700K + GTX 1080 Ti,
  treat it as the *high* end). So:
  - GS: hardware-accelerated renderer (Vulkan/D3D), not a CPU rasterizer and
    preferably not a heavy compute rasterizer either. paraLLEl-GS is an option
    for reference accuracy, but the end state is a native renderer fed directly
    from the game's draw calls (HLE of VU1 programs).
  - VU1: translate the game's microprograms to C++ ahead of time, never interpret.
  - Disc I/O: memory-map the ISO, no per-read file opens.
  - Avoid per-instruction runtime dispatch in hot paths (e.g. indirect jumps
    through lookup tables) where we can resolve statically.

## 2026-10-21 - Function map: Ghidra functions only, no label records
- Ghidra's export script also emits every branch-target label (57,897 of them)
  as a separate "function". With those, ps2_recomp produced 67k files / 572 MB
  with each code chunk duplicated. Without them: 9,584 files / 243 MB, 0 errors.
- ps2_recomp still discovers internal entry points itself and maps each to its
  owner function (the function starts with a `switch (ctx->pc)` that jumps to the
  resume label), so jump tables and thread resumes stay inside one C++ function.
- BlackLine's archived VCS map (`gta.csv`) is for the PAL build SLES-54622, so its
  addresses don't apply to SLUS-21590. Its function count (9,549) matches ours,
  which is a good sanity check that Ghidra's boundaries are complete.

## 2026-10-21 - Compiler: clang-cl + MSVC 2019 STL
- The machine's VS 2022/2026 installs have no C++ toolset; VS 2019 Build Tools
  (MSVC 14.29) do. We build with LLVM clang-cl 23 against that STL and the
  10.0.19041 Windows SDK. clang-cl also handles the huge generated files better
  than cl.exe. `tools/toolchain.py` sets this up (vswhere + vcvars64).
- Baseline CPU: AVX2 (upstream's `/arch:AVX2`). Revisit if we want pre-Haswell
  CPUs; the generated code mostly needs SSE4.1. (Revisited 2026-10-31: the build follows
  the PC's CPU - AVX2, AVX or SSE4; see the entry of that date.)

## 2026-10-21 - Graphics plan
1. Bring-up: upstream CPU GS backend (slow, but no extra work).
2. Accurate + fast enough: paraLLEl-GS (Vulkan compute, LGPLv3+) for exact
   output with supersampling, as hctp-recomp does.
3. End state for performance and real hi-res/widescreen: native renderer fed by
   high-level hooks on the game's VU1 render paths (VCS has only ~4.8 KB of VU
   microcode + 4 small DVP overlays, so the set of programs is small).
- 2026-10-22: step 3 is now planned in detail in NATIVE_RENDERER.md (SDL3 platform
  layer, own RHI with a D3D11 backend, GS HW backend port, then packet-level HLE).
  Its six decisions were approved by the user on 2026-10-22.

## 2026-10-22 - Movies skipped during bring-up
- `FUN_0021CA70` (from `main`) plays `STINGS.PSS` then `INTRO.PSS` via the movie
  player at `0x40D8B8`. Under the runtime's sceMpeg stubs it stalls after ~8
  stream reads. Skipped (returns 0) in `src/game/vcs_overrides.cpp` for now.
- Plan (Phase 4): native playback - demux the PSS from the disc with FFmpeg and
  draw it ourselves (cheaper than emulating the IPU, and allows clean scaling).

## 2026-10-22 - Iteration speed
- New bindings go in `src/game/vcs_overrides.cpp` (relink only) rather than the
  recompiler config (rebuilds all generated code).
- LTO off for development builds; ThinLTO for release builds.

## 2026-10-22 - Rule for hooks on recompiled functions
- A generated function can return to the EE scheduler in the middle of its body
  (thread switch at a checkpoint, e.g. inside a `DelayThread` poll loop). It is
  later resumed through the function table at the *resume address*, which maps
  to the original generated function, not to a wrapper registered on its entry.
- So a hook may do work **before** calling the original, or replace the
  function entirely, but must not rely on code running after the original
  returns. For "after" logic, hook the next function the caller invokes.

## 2026-10-22 - paraLLEl-GS: integrated, but off by default
- Ported hctp-recomp's paraLLEl-GS backend (runtime/gs/gs_gpu*.{h,cpp}) and hooked
  it into upstream's GS frontend (raw GIF per path, register writes, IMAGE uploads,
  readbacks, present at VBlankStart, frames read back into the raylib window).
- On the dev GPU (GTX 1080 Ti, Pascal) every run hung the GPU > 2 s and Windows
  reset the display driver (TDR, event 4101); NVIDIA's OpenGL driver then
  aborts the process (fail-fast code 7 in nvoglv64.dll). Independent of the
  supersampling rate. Likely the compute rasterizer relies on behaviour Pascal
  lacks (no independent thread scheduling) or on a content-specific hang.
- Because the target is *modest* hardware, a compute-heavy rasterizer that
  doesn't run on a 2017 high-end card is the wrong default. The CPU renderer
  stays default; `PS2X_GS=gpu` opts in to paraLLEl-GS for newer GPUs.
- Next: a conventional hardware-rasterized GS backend (GSdx-HW style) behind the
  existing `GSRasterBackend` interface - broad GPU support, upscaling-friendly.

## 2026-10-23 - Post effects: exact GS emulation at 640x448 first, resolution-aware reads for higher resolutions

The user asked for the post effects to stay as authentic as possible at the original resolution first, then at
higher resolutions and other aspect ratios. At 640x448 the emulated GS draws are the reference, so the first step
made them match the GS (pixel centres) instead of replacing them with approximations, and kept them correct at
higher internal resolutions by reading 1:1 copies at full resolution. Effect-level shaders (one pass per effect,
independent of the PS2 frame-buffer layout) come with the native frame (milestone 3 step 4), checked against this
emulation at 640x448.

## 2026-10-23 - Native HUD: the game names its primitives, the GS packets still carry them

Milestone 3 step 2 draws the HUD natively (docs/NATIVE_RENDERER.md "HUD layer"). Choices:
- **The game's own Im2D calls still run** and build their GS packets; the renderer takes the HUD's primitives out of
  the frame by matching them against the Im2D calls of the frame (vertex positions), then draws them at the window's
  size. Replacing Im2D outright would also need RenderWare's texture cache (VRAM placement, CLUT uploads) and its
  render-state translation reproduced; going through the packets keeps those exact and leaves a switch back
  (`VCS_HUD_NATIVE=0`) at no cost.
- **Textures from the GS backend's decoded cache**, not a decoder for RenderWare rasters: the HUD textures reach VRAM
  through the game's own uploads, and the cache keys them by content. A raster decoder becomes worth having when 2D is
  drawn without GS packets (milestone 3 step 4, a native frame).
- **Anchors from where an element sits** (grouped boxes, the 4:3 screen in thirds with a dead zone around the middle),
  not a table of HUD call sites: it covers text boxes, timers and counters the game positions itself, with nothing to
  keep in step with the code. Elements may change side as they grow (a help box past the middle stays left: the
  rule uses its middle and its left edge).
- The HUD placement **default stays "Keep 4:3"**; "Screen edges" is a choice in the menu (authentic by default).

## 2026-10-23 - Clipped world packets: correct picture first, then the C++ clipper

GPU clipping of the packets the VU sends through its clipper drew GON_C3's poolside ground black. The user chose the
correct picture as the default (clipped packets on the VU1, flyover 30 -> 25.5 fps) and milestone 3 to start with the
VU clipper. It is reproduced bit-exactly in C++ (game-internals.md 6h); its GIF packets go to the GS as the VU's
kicks would, so the picture is the VU's and the cost is close to GPU clipping. GPU clipping stays behind
`PS2X_HLE_CLIPPED=1`.

## 2026-10-22 - Performance status (i7-7700K)
- With GS work discarded (`PS2X_GS=null`) the game now runs at full speed
  (60 VBlanks/s, 15% idle) after two changes: hctp's VU1 throughput mode
  (13 -> 23 VBlanks/s) and batching IOP time (23 -> 60).
- With the CPU GS renderer it is 2.9 VBlanks/s: the rasterizer is ~95% of the
  frame. A GPU GS backend is the critical path.

## 2026-10-22 - VU1 microcode compiled ahead of time
- VCS uploads 22 distinct VU1 microcode blocks at run time (REF payloads from
  `.text`, the MPG VIFcode in the DMAtag). `game/vcs/vu1_uploads.txt` lists their
  ELF addresses; `tools/vu1_recomp.py` (vendored from BigDaddyJRich/hctp-recomp,
  GPL-3.0, plus an uploads-list input) reads the bytes from the user's ELF at
  build time and writes `generated/vu1/vu1_compiled.cpp` (never committed).
- Pieces are split at every upload boundary because VCS overlays parts of its
  big block with per-pass variants and a piece runs only while VU memory holds
  all of its bytes. Result: 99.97% of VU1 pairs run compiled.
- Verified with `PS2X_VU1_VERIFY=1`: registers, VU memory and XGKICK packets match
  the accurate interpreter; only cycle counts differ (throughput variant).
- Frame rate in game (i7-7700K): 11 -> ~35-50 VBlanks/s over this pass
  (compiled VU1, CLUT hash cache, content-keyed textures, upload fast path,
  batched timers). The EE thread is the single saturated thread. Next: VU0
  microprograms 0x0CF8 (sector box clip) and 0x0EB8 (~25% of the EE thread;
  they read MAC flags with pipeline timing, so they run on the exact path).

## 2026-10-22 - Native VU0 programs; no FP contraction
- VU0 programs 0x0CF8 (sector box clip) and 0x0EB8 (3x matrix product) ran
  ~160k times / 5 s on the interpreter's exact path (they read MAC flags):
  ~25% of the EE thread. They are now C++ (`src/game/vcs_vu0_native.cpp`),
  transcribed pair by pair with the flag latency spelled out, using the same
  per-lane maths as the compiled VU1 code. Registered by code hash.
- Verification (`PS2X_VU0_VERIFY=1`) first showed 1-ulp differences: the
  *interpreter* was wrong - /arch:AVX2 let clang fuse `acc + a * b` into FMA
  (one rounding). The PS2 rounds product and sum separately. The runtime and
  runner now build with -ffp-contract=off; after that, 4.2 million live calls
  matched exactly (VF, VI, ACC, VU0 memory), and compiled VU1 still matches.
- In game (i7-7700K): ~46-67 VBlanks/s (was 35-50 before, 11 at the start of
  the performance work). Remaining top costs: generated game code ~31%, T4
  texture swizzle (WriteP4) ~9%, DMA chain copy (memmove) ~7.5%, HW backend
  recording ~7%, scheduler checkpoints ~4%.

## 2026-10-22 - SPU2 emulated in the IOP; IOP time follows the wall clock
- The SPU2 lives in ps2xIOP (`core/iop_spu2.cpp`), behind the IOP's memory map
  and DMA channels 4/7, rather than as a high-level libsd replacement: VCS's
  CDSTREAM.IRX drives libsd directly, and its streaming depends on SPU2 IRQ
  addresses and DMA-complete callbacks, which an HLE layer would have to fake.
- The mix (48 kHz stereo, one frame per 768 IOP cycles) goes to a raylib audio
  stream through a ring buffer with an adaptive target fill (60 ms, +30 ms per
  underrun, max 250 ms). Drift is absorbed by resampling within +-0.5%. A first
  version resampled by up to +-8%: when the buffer ran low the music audibly
  slowed down, which the user heard as "audio slows down with the frame rate".
- IOP time was derived only from accounted EE cycles, so when the EE ran below
  full speed the SPU2 produced 70-90% of real-time audio and playback stuttered
  in step with the frame rate. On hardware the IOP/SPU2 keep real time whatever
  the EE does, so each IOP batch now brings IOP time up to the host clock (at
  most 50 ms at once; lag > 1 s is forgiven). Measured: 100% production and 0
  underruns at 20-60 VBlanks/s (slow host simulated with PS2X_SLOW_VBLANK_MS).
  Stream refills (DMA bytes, IRQs per second) are the same at 20 and 60
  VBlanks/s: CDSTREAM feeds music from the IOP side, not per game frame.
  `PS2X_IOP_REALTIME=0` turns it off.

## 2026-10-22 - Native port roadmap decisions (docs/NATIVE_PORT_ROADMAP.md)
The user's choices on the roadmap's open questions:
- Start with milestone 1 (automation / coverage rig), then milestone 2 (correctness sweep), and
  carry the roadmap out autonomously, committing at milestones and major changes.
- Movies: FFmpeg as the decoder.
- 60 fps: a settings-menu option, off by default (30 fps stays the authentic default). (Superseded 2026-10-30: 60 fps
  with frame-rate independent timing is the default.)
- IOP retirement (milestone 6): do it.
- Windows only for now; milestone 9 (portability / distribution) is out of scope until further notice. (Superseded
  2026-10-30: native Linux, see that entry.)
- Game knowledge goes into docs/game-internals.md (successor of ENGINE.md, each fact with how it was
  found); AGENTS.md is the working contract for agents.

## 2026-10-23 - Render submission on its own thread; frame-rate independence next
- The user's order: the render submission worker first, then frame-rate independence (a variable time step:
  "remove dependence on the framerate for gameplay"), not fixed-step interpolation. The worker comes first because
  with game code and render submission in series on one thread (~18 + ~17 ms driving) a variable step would barely
  raise the frame rate, and one large risky change at a time keeps bugs attributable.
- Design (runtime/ps2_render_worker.h): the game keeps its synchronous view - chains are copied when kicked and the
  DMA completes at once, as before - and the work runs behind it in kick order. Everything the game can read back
  fences (waits for the worker) instead of being made asynchronous; measured, the game only reads the GS CSR (~5 a
  frame), so fences cost little. Rare game-thread entries into the GS (library stubs, native GIF fast paths) fence
  or step aside rather than being ported to the queue. At most one frame of lead (latency stays one frame).
- Kept outside ps2_memory.h (a free-function module with its own buffer pool) so the change does not rebuild the
  generated code. `PS2X_RENDER_THREAD=0` is the switch back.

## 2026-10-23 - Profile-guided, ThinLTO build for play; plain build for development
- `tools/build.py --opt pgo` (ThinLTO + PGO) is what to play and measure with: game code ~20-30% faster
  (PROGRESS.md milestone 4). The profile comes from an instrumented build (build/pgo-gen) playing tools/bench.py and
  the reference scenes; it lives in build/pgo (derived from the user's own game, never committed) and is retrained
  with `--opt pgo --train` after large changes - a stale one still builds, changed functions just lose its benefit.
- `--opt none` stays the development setting (no ThinLTO relink); the choice is kept in the CMake cache until changed.
  (2026-10-27: a fresh build directory is now `lto`, with `--quick` for `none` - see that entry.)
- Calls between recompiled functions got an inline fast path in the runtime header rather than a recompiler change:
  same effect on every call site, no regenerated code, and `PS2X_FAST_DISPATCH=0` switches it off.

## 2026-10-25 - Independent timing up to 240 fps; the ped-movement branch below step 0.5 patched
- Found what broke below the 0.5 step (docs/PERFORMANCE.md 5, game-internals.md 4): FUN_001f1d38 turns a ped's
  animation movement into a velocity with `* 0.5` instead of `/ step` when the step is under 0.5 - a branch the PS2
  never takes (CTimer floors the step). One instruction patch (game/vcs/overrides.toml: the constant it compares with
  becomes 0.001) sends every real step down the `/ step` path; steps of 0.5 or more run exactly as before.
- The audit then matches 1/30 - 1/100 at 1/144, 1/200 and 1/240 (running, driving, falling; sprint -5% / -11% at
  1/200 / 1/240), so independent timing's ceiling goes from 100 to 240 fps (step floor 50/240). Caps offered: 30 / 60
  / 100 / 120 / 144 / 165 / 240 / display refresh / unlimited (both up to 240). Missions above 100 fps are not
  playtested yet; the four frame-counter blinkers outside the 2D pass blink faster as before.

## 2026-10-24 - Independent timing capped at 100 fps for now
- VCS clamps its time step to at least 0.5 (1/100 s; Vice City used 0.01). With the clamp lifted, the frame-rate
  audit shows the player running at a fifth of the speed at 1/120 s - code somewhere assumes steps of 0.5 or more.
  Rather than run the game fast (keeping the clamp above 100 fps) or let it break, independent timing keeps the
  game's 0.5 floor and caps the frame rate at 100. The cap setting offers 30 / 60 / 100 / display refresh (up to
  100) / unlimited (up to 100). Finding and fixing what breaks below 0.5 (then 120 / 144) is phase 3 work;
  `VCS_MIN_STEP` lowers the floor to test it.

## 2026-10-24 - Movies played natively; the intro skippable from the settings
- CMoviePlayer::Play is replaced at its entry rather than feeding the game's own libmpeg / IPU player: no IPU, no
  movie sound through the IOP, the picture drawn by our presenter at 4:3 like the PS2's TV. The game thread waits in
  Play as it did in the game's player.
- Skipping as on the PS2: the logos can't be skipped (the game passes a threshold of 1,000,000 frames), the intro can
  with Start / Cross. The user asked to keep skipping the intro possible: Display > Skip intro movies skips both
  movies; off by default (authentic). Test tools set VCS_SKIP_MOVIES=1 so their boot timelines stay as they were.

## 2026-10-24 - Sound without the IOP: Rockstar's sound code ported onto a native libsd and the existing SPU2 core
- The roadmap's "native mixer" is the SPU2 core we already had (ADPCM, ADSR, interpolation, reverb - in C++, checked
  against hardware behaviour); what was emulated around it was MIPS code: Sony's libsd and Rockstar's CDSTREAM sound
  engine. Those are ported function by function (src/sound/libsd.cpp, src/game/vcs_sound.cpp), so the sound is the
  same by construction and checkable write for write; a different mixer would only have added a second way to sound
  different. A higher-quality mixer can later sit behind the same libsd.
- The IOP's threads became steps on the EE thread (RPC work in the call, the music thread one step per semaphore
  signal, max count 1 as the IRX creates it). Disc reads complete at once; DMA transfers keep their duration (the
  game opens a radio station and sends its volume 3 samples later - it relies on the preload taking that long).
- Tables the module carries (112 stream names, SPU2 addresses) are read from the disc's own MODULES\CDSTREAM.IRX at
  start (checked to be SLUS-21590's), so nothing from the game sits in the source; only code constants (bank first
  sound numbers, slot addresses) are written out.
- Checked by record / replay (tools/soundcheck.py) rather than by ear alone: the emulated IOP's sound RPCs are
  recorded with their IOP times and replayed into the native driver; SPU2 writes and audio are compared.
- With streaming and sound native the emulated IOP is off by default (no modules, no IOP time); `PS2X_IOP=1`,
  `VCS_NATIVE_SOUND=0` or `VCS_NATIVE_STREAM=0` bring it back for checking. The emulated IOP's code stays.

## 2026-10-24 - RSL world draws: record and replay at the VIF level, on by default

Phase 2 of RSL_SUBMISSION.md for world sectors. The world draw's geometry is drawn at a MARK put in place of its REF,
from a recording of what the VIF did with the block the first time (UNPACK results, MSCALs, meshes) rather than from a
decoder of our own:
- **Exact by construction**: the recorded VU qwords are the interpreter's own UNPACK results, the MSCALs run through
  the interpreter's own MSCAL routine (`PS2Memory::vu1Mscal`) and the native world handler; a recording is refused when
  its result could depend on state from before the block. A decoder of the UNPACK formats, masks and modes would have
  been a second implementation to keep in step.
- **The MARK keeps the order**: the draw happens where its REF was, after its state flush and GS register writes, on
  whichever thread processes the chain (render worker or not).
- **Invalidation by content**: a block is hashed each time it is drawn (~5% of the chain's bytes; cheaper than copying
  it) - streaming replaces cells without telling us, and the hash notices.
- **On by default** after the verify run (0 of 174,301 replays differ) and the picture / tour checks; `VCS_RSL_WORLD=0`
  keeps the packet path. The timing gain is within noise (world geometry is small); the path is the point - model
  instances come next on the same mechanism.

## 2026-10-24 - RSL model draws: the same record / replay; marked bytes stay in the chain's copy

Model instances (`FUN_002F7D58` / `FUN_002F80A0`) use the world's mechanism, with two changes that apply to both:
- **The marked bytes are copied and skipped, not replaced** (revises "cheaper than copying it" above): the render
  worker processes a chain while the game builds the next frame, so reading a block from guest memory at the MARK
  could see a cell or model streamed out after the kick. The copy is what the guest held at the kick; copying costs
  little next to processing. It also gives a CALLed packet - a sub-chain of tags - as one flat stream without a second
  chain walker of our own.
- **Every CALL in the packet buffer is marked**, not only the ones the hooked draw calls wrote: a recording is the
  interpreter's own result for those bytes, so it is exact whoever built the packet; the hooks only label the
  counts. Packets rebuilt every frame drop to the packet path by themselves (bytes changed three hits running).
- On by default after the verify run (1,217,887 blocks, 0 differ), scenes and tour; worker -1.3 to -2.4 ms in the
  bench. `VCS_RSL_MODELS=0` keeps the packet path for models.

## 2026-10-24 - Game thread: the profile's targets before draw-call replay

The plan after phase 2 was to replay draws at the RSL draw call instead of building their packets. A drive profile
with call chains showed the draw calls' own writes are a small part of the game thread; a reallocating chain copy
(~10%), four VU0 programs still interpreted (~5%) and the state flush (2.3%) were larger and each could be made
native or fixed with an exact check against the original. Those went first. Native guest functions follow the rules
already used for VU0 programs: bit- / byte-exact against the original in a verify mode, a switch back, and the
original runs whenever the native version would have to call other guest code (the flush's plug-in callbacks).
Draw-call replay stays the way to remove the remaining copy and flush together.

## 2026-10-24 - Block validity by write watch, not by reading the bytes

Step 4 (FULLY_NATIVE.md) needs to know at the frame kick whether a world / model block still holds the bytes its
recording was made from, without copying or hashing it (the distinct blocks are ~85% of the marked bytes: a per-frame
check by content would cost as much as the copy). Chosen: the OS's write watch on guest RAM, folded into per-page
generations once per kick. Not page protection: the stream thread reads the disc straight into guest RAM, and a kernel
write into a protected page fails instead of faulting. Not hooks on the game's streaming / instancing functions: every
path that writes packet memory would have to be found and kept up. Exact by construction (any write to any of a
block's pages makes it dirty), checked with a decide-copy-compare mode (2.4M decisions, 0 wrong).

## 2026-10-25 - Draw items: one tag in the chain per draw, the block decided at the kick

FULLY_NATIVE.md step 4 asked for world / model draws that make no packets. Chosen: the draw call becomes an item (its
flush when all CNTs, its GS registers, its block), and the packet buffer keeps one 16-byte CNT per draw whose MARK /
NOP name the item and the frame. Not "no tag at all": 2D, effects, texture uploads and the setters' own uploads still
go through the chain between the draws (step 5), and the tag is what keeps each draw in its place among them. The
block's validity is decided at the kick, after the write watch's fold, exactly as for a marked block - not at the draw
call: generations there only cover writes up to the last kick, and a cell streamed in this frame and drawn this frame
would be replayed from an old recording. A block that changed is copied from guest RAM at the kick (`flattenBlock`,
the walker's rules; checked against the walker's copy), so nothing reads guest RAM later on the worker.
Draws stay packets where an item would not stand for the same bytes later or cannot be made exactly: while
`FUN_00240ED8` caches a sector (the copy is sent again in later frames: game-internals.md 6i), when the flush calls
plug-in callbacks (guest code: ~15% of draws while driving), for a model block not yet seen copied by the walker.
Speed was not the reason (measured even: the walker's share falls by about what the items cost); the reason is the
path - the draws no longer depend on the chain's bytes, which step 5 needs.

## 2026-10-25 - Draws whose flush calls guest code: captured, not ported

Step 5f could have ported everything a state flush may call (FUN_001CFAE0's float maths, the two immediate-geometry
callbacks) so that every draw is built natively. Chosen instead: those draws run the original and the region they
wrote is captured into items (the 5d mechanism, with CALLs taken as blocks). The chain loses their packets all the
same, nothing new has to be checked bit for bit, and the guest code keeps doing what it does. A port stays possible
later if their CPU time ever matters (small: ~2% of flushes).

## 2026-10-26 - Gaps: take what is between hook points, in place

Step 5h could have gone on adding writers to the captured list one by one (the census by origin named them). Chosen:
every hook point takes what was written since the last one, whoever wrote it, so the list never has to be complete.
Taken in place (item tag + NEXT over the rest; the cursor never moves back), because a gap is settled at arbitrary
hook points where a caller may hold the cursor - one such caller (`FUN_00240ED8`) was found the hard way, and verify
mode cannot see this class (it keeps the packets). The price is a NEXT per run (~32 a frame) and dead bytes in the
buffer; the chain's walk in 5g skips both.

## 2026-10-26 - No second way to start a frame (FULLY_NATIVE.md 5g dropped)

5g would have handed the item list to the worker without walking the chain. Measured after 5h: the kick takes 0.23 ms
a chain, 0.19 ms of it the items' resolve, which stays either way; the walk and copy of the ~300 remaining tags (the
root lists, the item tags, the runs' NEXTs) cost ~0.04 ms. The chain is kept as what orders the frame: one path, the
walker's checks (verify modes, the census) keep working, and nothing is drawn from the chain's own bytes any more.

## 2026-10-26 - zlib natively as an exact port, not a faster inflater

`inflate_fast` could be replaced by a faster decoder (wide bit-buffer refills, multi-symbol tables) with the same output.
Chosen: zlib 1.2.2's own loop, so every call leaves `next_in` / `avail_in` / `hold` / `bits` exactly as the guest's -
checkable call for call against the original (`VCS_NATIVE_INFLATE_VERIFY`) - with only the copies that cannot overlap
done as `memcpy`. A faster decoder would stop at other points and leave zlib's state different between calls (the
final output the same); the ~20 ms an area load costs now did not justify losing the per-call check.

## 2026-10-27 - The optimised build is the default; `--quick` for development
The user asked for installing to be as easy as possible. A fresh build directory now configures with ThinLTO
(`PS2X_LTO=THIN`, `--opt lto`), so `python tools/build.py --recompile` from the README gives the game to play;
`--quick` (= `--opt none`) is the fast-relink build for changing code. Not PGO as the default: training plays the
rig's card copies (build/rig/saves), which a new checkout does not have, and takes ~30 min more. The choice stays in
the CMake cache as before (one build directory: switching recompiles the game, and a non-sticky default would make
every quick build followed by a plain one cost a full rebuild); the build's last line names it so nobody plays a quick
build by accident. The build also writes `iso=` into the vcs.ini next to vcs.exe when there is none: the exe is a
windows-subsystem program, so started by a double click without a disc it fails with no visible message.

## 2026-10-27 - Full screen is borderless only; test runs never write vcs.ini

Full screen covers the monitor at the desktop's resolution with no display mode change. With a flip-model swapchain
Windows 10+ presents a window covering the monitor directly, so exclusive mode buys little, while a mode change costs
seconds per switch, breaks Alt+Tab and moves the other monitors' windows; the game's resolution is the Internal
resolution setting, not the monitor's mode. Exclusive full screen can come later as a third choice if someone needs a
lower monitor mode. Hidden test runs read vcs.ini but never rewrite it, so a scripted check (Alt+Enter, the settings
menu) cannot change the player's settings.

## 2026-10-27 - 4:3 screens sit inside the game's frame, not the window

The user's choice: in a ratio wider than the monitor (32:9 on 16:9) the pause menu stays 4:3 but sits in the game's
strip between the bars, at the strip's height, rather than over the whole window. Loading screens follow the same rule
(they are the same kind of frame: no 3D). The intro movies keep the whole window: they play before there is a game
frame to belong to. The menu switches to 4:3 as soon as it opens rather than after the 3D history runs out.

## 2026-10-27 - Ultrawide: fixed ratios plus Match window, Hor+ between 4:3 and 32:9

The user asked for 21:9 and 32:9. "21:9" monitors are 2.37 (2560x1080) or 2.39 (3440x1440), not 21 / 9 = 2.33. Chosen:
- The menu's 21:9 is exactly 21 / 9 (the label says what it is), and a Match window choice takes the window's own
  shape, so every monitor gets a picture without bars; it follows the window each host frame (resizing, full screen).
- Match window is clamped to 4:3 .. 32:9: narrower than 4:3 would cut the original horizontal view (we only do Hor+),
  wider than 32:9 is untested (triple screens would need checking: the view passes 140 degrees).
- No separate FOV setting: the game's horizontal FOV is kept for the middle 4:3 and the sides are added.

## 2026-10-26 - The trainer: the game's script commands, nothing saved, the game's input taken while open

The user asked for a trainer like RDR2's (docs/TRAINER.md). Chosen:
- Everything the game has a script command for goes through it (run on the game's own script VM), rather than
  writing the game's structures: the commands keep the game's own bookkeeping (wanted level and police, streaming,
  vehicle creation and ownership). Only values that drift every frame (health, ammunition, a clip, a vehicle's
  health) are written directly while their switch is on - a command per frame would cost a script each time.
- Switches are not saved in vcs.ini: the game starts authentic every time; the trainer is used, not configured.
- The menu takes the game's input while open (as the settings menu does): its keys are the game's D-pad, Start and
  Select. RDR2's trainer lets the game keep the keys it does not use; here every menu key is a game button.
- Trainer scripts must never be in a save: started only while the game runs, taken back out when it pauses.
- Models are streamed asynchronously with a time-out rather than `LOAD_ALL_MODELS_NOW`, which froze the game when the
  streaming memory was full.

## 2026-10-28 - Music / effects volume: the game's own levels, kept across loads

The user asked for music, sound effects and voices volume, with the music / effects changes shown in the game's own
menu. The game has exactly two levels (its pause menu's Music / SFX Volume), so those two settings *are* them rather
than extra gains on top: one value, two menus. On the PS2 a save carries the levels and a load restores them; here the
setting wins (every non-menu call of the game's setters gets the player's level), because a PC game keeps its settings
across saves and the newest save loads at every boot - the save's levels would otherwise undo a change made without
saving. The levels are percent of the game's 0..127 (linear, as the game's bars are); a pause-menu step is 16, so the
setting shows the nearest percent and the game keeps its exact level.

Voices volume was added the same day as a gain on the speech voices in the native driver (the game has no level for
speech) and **removed on the user's decision**: it felt inconsistent, since only speech played as effect sounds
followed it, while speech in cutscenes and on the radio is mixed into those streams. The code is gone (the driver is
as before); game-internals.md 9c keeps what was learnt.

## 2026-10-28 - Player settings: bindings in the game layer, the mouse as stick speed, defaults as before

The user asked for flexible settings players can change. Choices:
- **Where the bindings live**: in the game layer (src/game/vcs_input.cpp), feeding the runtime's pad backend through a
  reader hook; the runtime keeps its fixed mapping as the reference (`VCS_INPUT_MAP=0`), as the rules want for any
  replaced path. Injected (control-channel) keys go through the bindings too, so every scripted check exercises them.
- **Two slots per action**, any key / mouse button / wheel notch, Shift / Ctrl / Alt without a side (one slot holds
  "Shift" where the old mapping needed two). One key, one control: binding steals it from the other control, and the
  menu keys can neither take nor lose theirs - a silent double binding was judged more confusing than the steal.
- **Mouse camera**: the PS2 game has only a stick camera, so the mouse's speed becomes the right stick's tilt (not its
  distance per frame), which keeps the turn per hand movement the same at any frame rate; the stick's top speed caps
  flicks. True mouse look would mean writing the camera's angles in the game's memory - later, if the stick feel is
  not good enough. Off by default (it holds the cursor in the window).
- **Vibration**: the game's own motor levels (it already shook the pad; the stub dropped them), 100% = as on the PS2.
- **Defaults**: exactly the old fixed mapping and behaviour (keep running in the background, vsync off as the flip
  model presented before), so nothing changes for a player who opens no menu. The Authentic preset is the Display /
  Graphics defaults; presets never touch controls or sound.
- **Volume** squared (the slider's middle is about -12 dB), master after the mix so the game's own levels and the
  SPU2 output stay bit-identical for the checks (`PS2X_AUDIO_DUMP` records before it).

## 2026-10-27 - Per-frame mission meters: each instruction on the time step, not the scripts at 30 Hz

PHI_A2's heat bar (and meters in eleven more missions, game-internals.md 11f) change by a fixed amount every frame,
so they ran 4.8x fast at 144 fps. Two ways to fix it:
- Run the script VM at the PS2's 30 Hz above 30 fps (skip CTheScripts on frames in between, with the time step summed
  for them). Every per-frame script assumption would be as on the PS2 at once, cutscene camera moves and object
  animations included - but everything scripts move each frame (attached objects, cameras pointed per frame, cars
  held to a speed) would update at 30 Hz inside a 144 fps picture, script reactions would come up to 33 ms late, and
  the timers / WAITs would have to be fed the skipped time. Changes the whole game's behaviour for a few missions.
- Chosen: scale just the listed instructions by the frame's step over 1/30 s, the way the game's own timed opcodes
  (0x0031 - 0x0034) do it - what the mission would have used had the PS2 not been locked to 30. Every site is in a
  table with its mission, offset and decoded operands and is checked before it is touched, so a different build or a
  mod's MAIN.SCM leaves it alone; the survey that found them is written down so the next one is a table row. Visual
  per-frame animations are left at the frame rate for now (looks only).
Off in original timing at 30 fps (the game's own handlers, bit-exact); `VCS_SCRIPT_TIMESTEP=0` turns it off elsewhere.

## 2026-10-28 - Per-frame vehicle physics: scaled by the step, the brake's constants from guest RAM

A car running over a body jumped and a stolen bike flipped at high frame rates (game-internals.md 4): the push of a
body under a wheel (FUN_003F34F0) and ProcessWheel's rolling resistance act a fixed amount once a frame.
- Scaled by the frame's step over 1/30 s, as the mission meters (2026-10-27) and reVC's FIX_BUGS
  (`CTimer::GetTimeStepFix()`) do - only when the frame is shorter than the PS2's. Longer frames keep the game's
  amount: the PS2 game itself applied it per frame when it dropped below 30, so 25 fps plays as it did there.
- The push goes through ApplyTurnForce, so its hook scales the one call by return address; no recompile, and
  `VCS_PHYSICS_TIMESTEP=0` gives back the reference.
- The brake is built from immediates in the middle of ProcessWheel, where no hook reaches (rule 2: nothing after the
  original returns). Reimplementing ProcessWheel natively would mean the guest RNG and two guest calls; instead the
  four constants are patched to `lwc1` from guest RAM 0x7FF00.., which the ProcessWheel hooks write (scaled) before
  every call. Unscaled they are the game's exact floats, so 30 fps stays bit-exact. Those 16 bytes below the runtime's
  callback stacks are ours now (game-internals.md 4); the hooks are required by the patched code, not an option.
- CBike's lean `max(step, 0.5)` patched to 0.001 like the ped and cutscene ones (the same class; above 100 fps only),
  then the same in cars / bikes in water and a boat's rudder. Each patch was measured before and after; one that
  changed nothing measurable (the buoyancy's lift cut-off, game-internals.md 4) was taken back out.

## 2026-10-29 - vcs.exe at the top of build/, native by its own defaults (the user's request)

- The user found the exe "buried": `build/release/third_party/PS2Recomp/ps2xRuntime/vcs.exe`. The main build tree
  (build/release) now links it to `build/vcs.exe`, with its DLLs (staged next to the target, so they follow) and
  `vcs.ini` (CMakeLists.txt, `RUNTIME_OUTPUT_DIRECTORY`). Only that tree: PGO training's build/pgo-gen keeps its exe
  in its own folder, so the two never write the same file. build.py moves a player's vcs.ini from the old folder and
  deletes the old exe / pdb, so no stale build is left to start; clean.py never deletes the game files in build/.
- A double-clicked vcs.exe ran the software GS at ~5 fps: only run.py / scenes.py set `PS2X_GS=hw`, so every test
  ran a configuration players never got. The launcher now defaults to hw, and the tools no longer set it - they run
  vcs.exe with its own defaults, as a player does. Every other native path was already on by default (each switch is
  `=0` to go back to the reference): with no PS2X_* / VCS_* set, gameplay shows gs_draws 0, vu1_programs 0, the
  native sound driver, the emulated IOP off and the render worker on.
- The runtime made empty mc0 / mc1 folders in the working directory before loadELF set the card path; the launcher
  now sets the card folder before the runtime starts.

## 2026-10-29 - Mouse look on foot, driving keys (the user's request)

The user asked for the mouse to move the camera while walking, right click to lock on to peds, and W / S / Space as
accelerate / brake / handbrake while driving.
- **Mouse camera on by default** now: the user wants the mouse to steer the camera, and with it off a new player's
  mouse did nothing but click. It is a control binding, not a change to the game's look or rules, so the
  "authentic by default" rule does not hold it back; it stays a toggle (Keyboard > Mouse camera).
- **True mouse look on foot** (the "later" of the 2026-10-28 entry above): the follow camera's heading is written in the
  game at the pad read, and its source points turned with it - the camera otherwise pulls the heading back to where it
  stands (game-internals.md 12b). The pitch cannot be written that way (the camera rebuilds it from where it stands), so
  it goes through the stick's own pitch speed with the game's stick curve undone and the excess over its top speed
  carried to the next frames: the angle is exact, only very fast vertical flicks arrive a little late. Writing the
  pitch directly would mean moving the source points up / down and their collision with the world - not worth it for
  the ~0.1 s it would save on the fastest flicks.
- Every other camera (vehicles, sniper / first person, lock-on) keeps the stick path: each has its own code, and the
  stick is what they were made for. The stick path now aims past the game's dead third of the stick (FUN_002842e8),
  so slow mouse movements turn those cameras too - before, under a third of full speed did nothing.
- Mouse forward now looks up. The old stick path sent it the other way (the game looks up for a stick pulled back) while
  the menu said "forward looks up".
- **Right click** was already R1 (target / lock-on); checked that it locks on with fists and a pistol and that the mouse
  moving meanwhile does not break the lock. Nothing to change but the check.
- **Driving keys** are a section of their own with their own key group: a key may be W on foot and Accelerate in a car,
  which the one-key-one-control rule of the 2026-10-28 entry would have refused. While the player drives (ped +0x480
  set and state 0x37) the driving bindings apply and the on-foot bindings of the same keys do not (W must not also tilt
  the stick: on a bike that leans, in a helicopter it pitches). The menu keys stay exclusive across groups. Lean
  forward / back (bikes, aircraft) moved to Keypad 8 / 5, since W / S no longer do it in a vehicle. The gamepad keeps
  the game's own layout.

## 2026-10-30 - The trainer's free camera (the user's request)

The user asked for a free-cam mode in the trainer, and chose: the world keeps running while it flies (the player
stands), Hide HUD as a switch of its own, on / off from a trainer row and a hotkey (F6, gamepad Back + LB).
- **The camera written after the game's update, not a script camera.** GTA trainers usually move a script camera
  (SET_FIXED_CAMERA_POSITION + POINT_CAMERA_AT_POINT) every frame. Here that would be a script per frame through the
  trainer's queue, a frame late, and only between frames. Instead the free camera writes the active CCam's source /
  front / up / FOV right after the game's own update of it (in place of the height routine CCamera::Process calls
  next, FUN_002A2ED0; game-internals.md 12b); CCamera::Process does the rest, so culling, LODs and streaming follow
  with no extra work. A hook of the next call, not code after the original returns (AGENTS.md rule 2);
  `VCS_FREECAM=0` keeps the game's code untouched.
- **The game's update keeps running** (changed the same day): the first version replaced CCam::Process while flying,
  and a mission launched under it hung on "Loading..." (the user's report) - its cutscene ends only when the cutscene
  camera has played its path, which that update advances. Running it and overwriting afterwards fixes every cutscene,
  wherever the mission was started. Launching a mission from the trainer also turns the free camera off (the user
  was fine with that as the fix): one starts a mission to watch it.
- **Real time**: the camera moves by the host clock, so its speed is the same at 30 / 60 / 144 fps and under the
  trainer's Game speed (slow-motion shots fly normally).
- **Fixed keys** for flying (W / A / S / D, E / Q, mouse; the gamepad's sticks and triggers): the game gets none of the
  input meanwhile, so they cannot clash with its bindings; only the on / off key is a setting. Rebindable flying keys
  can come later if wanted.
- **Hide HUD** skips CHud::Draw (the hook vcs_hud.cpp already had, now installed with `VCS_HUD_RW=0` too): radar, clock,
  money, weapon, stars. Text drawn after it (zone names, help, subtitles) stays - hiding that too would mean picking
  which text is the HUD's.

## 2026-10-30 - Native Linux (the user's request); the Steam Deck postponed

The user asked for Linux to be as native as Windows, first with Steam Deck support, then (the same day) without it:
"just have strong support for linux for now". Their choices: a **plain host build** (the distro's own compiler and
libraries through the same `tools/build.py`; no container, Flatpak or Steam Runtime), tested on their Linux laptop
(i5 Skylake, HD 520, 7 GB) with the Windows PC kept as the reference. The working list is docs/LINUX.md.
- **clang only** (19+, for `preserve_none` in PS2_GUEST_CC): the same compiler as clang-cl, so the same code and
  numerics. GCC would need its own checks of every bit-exact path and lacks preserve_none.
- **Flags as clang-cl's**, written out: `-march=x86-64-v3 -ffp-contract=off -fno-math-errno`, and `-fno-strict-aliasing`,
  which clang-cl implies (MSVC compatibility) and plain clang does not - the runtime and generated code read guest memory
  through casts. No stack protector / CET, as `/GS-`.
- **Vulkan**, a new RHI backend (`src/rhi/vulkan`), rather than OpenGL or a translation layer: the RHI was shaped for it
  (immutable pipelines, explicit passes), and it can be checked against D3D11 on the same Windows PC
  (`-DVCS_RHI_VULKAN=ON`). D3D11 stays the Windows default and the reference. Vulkan 1.3 with dynamic rendering,
  synchronization2 and push descriptors; volk, VMA and the headers from source (no SDK).
- **Shaders stay one HLSL source**: glslang (a distro package) compiles them to SPIR-V through `tools/hlsl_spirv.py`.
  DXC was the plan, but no distro ships it here and a downloaded binary is a tool the user didn't install; glslang
  handles every shader, and the script fixes the one thing it gets wrong (dual-source output: Location 0 Index 1).
- **Dear ImGui through the RHI** (`render/ui_renderer.cpp`) for Vulkan instead of `imgui_impl_vulkan`: one pipeline
  works for every target format the backend draws to (the swapchain's BGRA and the RGBA captures), and it uses only
  the RHI. D3D11 keeps `imgui_impl_dx11` (the reference stays as it was).
- **Hidden test runs make no swapchain** on Vulkan: the back buffer is a texture. A FIFO present to a hidden Wayland
  window can wait forever, and tests take screenshots off-screen anyway.
- **A null backend** (`VCS_RHI=null`) to bring up and check the CPU side without the renderer.
- The Windows-only debug tools got Linux versions where cheap (crash handler on signals, a SIGPROF sampling profiler);
  `perf` / `gdb` cover the rest. The write watch has no Linux equivalent yet (correct without it, slower); whether it is
  worth userfaultfd is to be measured first.

## 2026-10-30 - Frame-rate independent timing shows every frame it renders

The user asked for more performance on both platforms and chose this first (with specialised GS pixel shaders next).
With frame-rate independent timing the game thread now queues one frame and the window shows every frame rendered
(PERFORMANCE.md 17: 30 -> 76 shown fps on the GPU-bound laptop). Original timing keeps the old pacing on purpose: there
game time is counted in frames, and holding the game thread back would slow the game down when the GPU can't keep up,
where showing fewer frames keeps it at speed.

## 2026-10-30 - Input delay first: host-paced game frames, the frame lead

The user felt more input lag after the show-every-frame change and asked to keep input lag to a minimum. It is measured
now (`PS2X_RHI_STATS`: input snapshot to GPU done) and the frame-rate independent pacing was rebuilt around it
(PERFORMANCE.md 19): the main thread's frames pace the game's, a backlog is drained, and the game's frame starts early
enough (the frame lead) to be ready just in time on input read just before. 60 cap: 50 -> 34 ms. When the GPU is the
limit the one frame waiting stays (dropping it cut 9 ms but cost 4% of the frames and visible judder): smoothness kept
there.

## 2026-10-30 - A launcher for releases (docs/LAUNCHER.md)

The user asked for a launcher, as other recomps have, to be included in releases: locate the game, build, launch;
Windows first; the Python scripts must stay usable on their own.
- **Distribution stays source-only.** A release is the source plus `VCSLauncher.exe`, python.org's embeddable Python
  and `deps/` (the open-source libraries CMake would download) - no game code: the game is still recompiled and built on
  the player's PC. `tools/release.py` checks every file before zipping (no disc images, no MIPS ELF, nothing from
  generated/ or build/) and refuses the release otherwise.
- **The launcher is a front end over `tools/`**, never a second pipeline: it runs `doctor.py`, `build.py`,
  `setup_vs.py`, `backup.py`, `report.py`, `update.py`, `rip_disc.py` and reads their `@vcs {json}` events. A player
  with the launcher and a developer with a terminal run the same code.
- **SDL3 + Dear ImGui** for its window (already dependencies, the look of the in-game menus, portable later), its own
  CMake project so CI builds it without a disc; one static exe.
- **Bundled Python and libraries**: Visual Studio's compiler is the only thing left to install, and the launcher runs
  Microsoft's installer for it (checked for Microsoft's signature first; one UAC prompt). The release builds with no
  git and no internet (`FETCHCONTENT_FULLY_DISCONNECTED`). Cost: an 82 MB zip instead of a few MB.
  From that zip in an empty folder the game built in 8 min 42 s on the dev PC (the README said ~15 min on a laptop).
- **Updates only when asked** (or at start when the player turns it on): the check is one request to GitHub's API.
- The launcher's own settings live in `build/launcher.ini`; the game's stay in `vcs.ini` (F2 in the game).
- Not done: code signing (SmartScreen warns about an unknown publisher; documented in the README).

## 2026-10-30 - 60 fps with frame-rate independent timing by default

The user asked for frame-rate independent timing and 60 fps to be the default for every player from their first
build. `display.timing` defaults to 1 and `display.fps` to 60 (src/main.cpp); everything else stays the PS2's (4:3,
original resolution, no anti-aliasing). This replaces "60 fps off by default" (2026-10-22): the one enhancement on by
default, the rest still opt-in.
- *Authentic (PS2)* sets original timing at 30 explicitly (it used to be "the defaults"); *Reset to defaults* now gives
  60 fps independent timing; the menu's help says so.
- vcs.ini only keeps values that differ from the defaults, so players who never touched the frame rate get 60 fps on
  their next build too; anyone who chose a frame rate keeps it.
- Test runs keep the old timing: hidden runs (toolchain.test_window) pin `VCS_TIMING=0` and `VCS_FPS=30` unless the
  test sets them, so the regression baselines, the bench numbers and the checks mean what they did. controls_check
  passes an empty `VCS_TIMING` (not pinned, ignored by the game) because it tests the presets. The PGO profile is still
  trained at original timing (bench / scenes); retrain at the new default if the gap shows.
- Checked 2026-10-30 in a hidden run: reset -> timing 1, 60; Authentic -> 0, 30; Widescreen, light -> 1, 60; a test
  run -> 0, 30.

## 2026-10-31 - CMake and Ninja from anywhere, else downloaded

A player's build stopped at *Configuring the build* with a traceback: with Visual Studio Build Tools only, cmake is
in Visual Studio's own folders, which the checks looked at and Windows didn't (it starts a child's program from the
launcher's PATH, not the one given to it). The user asked for CMake to be found whatever the setup.
- **Search, then download.** `toolchain.build_tools` takes the first CMake >= 3.21 and Ninja >= 1.10 that run (each
  one is started: `cmake -E capabilities`, `ninja --version`): the build environment's PATH first (what the player
  chose), then every Visual Studio install's bundled copy (vswhere -all and the default folders), Kitware's
  installer's folder, winget, scoop, Chocolatey, pip (this Python and the user's), CLion, Qt, the Android SDK and
  MSYS2's native environments (its POSIX ones refused: msys-2.0.dll / cygwin1.dll beside the program). With none,
  Kitware's / Ninja's own builds are downloaded once into `build/build-tools` - pinned versions (CMake 4.4.5, Ninja
  1.13.2: what the dev machine builds with) and SHA-256 hashes in toolchain.py, so a changed file is refused.
- **Versions are checked, not just names**: VS 2019 bundles CMake 3.20, below the project's 3.21 - found on the dev
  machine itself. A too-old one is skipped with a line saying why.
- **Always by full path**: `resolve` gives cmake its path, `CMAKE_MAKE_PROGRAM` gives CMake Ninja's. The chosen folders
  are not put on PATH, which would let a folder like Chocolatey's or pip's shadow clang-cl. A build tree whose CMake or
  Ninja has gone (`stale_tools`) is set up again with today's.
- `VCS_CMAKE` / `VCS_NINJA` (full paths) choose by hand, checked like the rest. doctor never reports CMake as missing
  any more: it says the build will download it; only a failed download stops a build (`no_cmake`).
- Not bundled into the release zip (55 MB more for the rare player with no CMake and no internet): Visual Studio's
  installer, which the launcher runs, brings CMake anyway.

## 2026-10-31 - Release zips dated a day before the commit; the build repairs future dates

A player in Brazil (UTC-3) couldn't build 0.0.1: Ninja re-ran CMake 100 times and gave up ("manifest 'build.ninja'
still dirty"). Zip dates have no time zone; Python's zipfile writes the packer's local time (Adelaide, UTC+10:30), so
every file came out up to 13.5 h in the future, newer than any build.ninja CMake could write. Both ends are fixed:
- **release.py** dates every file the commit's time minus a day, as UTC (`zip_time`; `SOURCE_DATE_EPOCH` when set):
  in the past in every zone, and one date for the whole zip. Not each file's own time made safe: nothing in the
  build needs those times, and one date makes the zip depend only on its content.
- **build.py / recompile.py** set files dated in the future to now before CMake runs, with a line saying so, rather
  than stopping with a message: the player can't fix the zip, and touching a file's date loses nothing. doctor only
  reports them (it changes nothing). A build tree that already looped recovers without being deleted (checked).
- Ninja's "still dirty" still gets its own failure (`file_dates`) for what the repair can't reach - a clock that is
  wrong, a read-only file.

## 2026-10-31 - The CPU baseline follows the PC that builds (AVX2, AVX, SSE4)
- A player's report (Xeon E5-2687W, Sandy Bridge: AVX but no AVX2): "recompiling MIPS code to C++ fails in 1 second",
  `recomp.log` empty. Every target was built with `/arch:AVX2`, so `ps2_recomp.exe` - and later `vcs.exe` - stopped at
  its first AVX2 / BMI instruction (`STATUS_ILLEGAL_INSTRUCTION`, 0xC000001D) before printing a line; the tools didn't
  say how it ended, and the doctor didn't look at the CPU.
- Players build on their own PC (source-only releases), so the build is compiled for that PC's CPU:
  `PS2X_CPU=AVX2|AVX|SSE4` (`ReleaseMode.cmake`; `/arch:AVX2` / `-march=x86-64-v3`, `/arch:AVX` / `-march=sandybridge`,
  x86-64-v2), chosen by `toolchain.cpu_level()` (Windows `IsProcessorFeaturePresent`, Linux `/proc/cpuinfo`) at every
  configure; a tree set up for another level is configured again (a full rebuild). `VCS_CPU=AVX|SSE4` builds an older
  CPU's game on a newer PC (how it was tested). Below SSE4.2 (Core 2, 2006-08) the build stops with `cpu_unsupported`.
- One binary with runtime dispatch was the alternative: it would need every hot path written twice and picked per call,
  for a build players make themselves anyway. AVX2 stays the default and what releases are measured with.
- Numerics are the same at every level: FP contraction is off everywhere (no FMA anywhere since 2026-10-22), and the
  vector paths (`ps2_vu1_compiled.h`, `__SSE4_1__` in `ps2_runtime_macros.h`) have per-lane fallbacks that do the same
  operations. `vu1v` (compiled VU1, native VU0) needs only AVX, so it is on `__AVX__` now: AVX builds keep it, SSE4
  builds take the per-lane version (which had never been compiled: `vcs_vu0_native.cpp` shuffled `__m128` itself).
  Checked by recompiler output and scene captures (PROGRESS.md, 2026-10-31).
- A tool that fails now ends its log with how it ended (`toolchain.describe_exit`: the NTSTATUS name, or the signal),
  and the launcher gets `illegal_instruction` / `missing_dll` instead of a bare `recompiler_failed`.

## 2026-10-31 - Free aim: the game's own, driven by the host (the user's request)
The user asked for free aim with a reticle, selectable in the settings, that feels as if the game had supported the
mouse all along; their choices: aim while the right button is held, no lock-on with the mouse when it is on, free aim on
the gamepad too (no aim assist), the mouse 1:1 in scopes and the wheel for weapons / zoom; no mouse steering. One rule
from them: Vic moves while free aiming exactly when he can move locked on with that weapon - nothing new is added.
- **The game's free aim, not a new one.** VCS has free aim (R1, then L3; game-internals.md 12c): a lock-on to an
  invisible dummy object with its own circle reticle, aim pose, camera and no random spread. Building on it keeps all of
  that (the reticle is the game's, scaled with the HUD; peds react as to any aim) and replaces only what feels bad: the
  pad read enters it in one report (R1 + L3), the mouse / right stick turn the dummy about Vic (writing it every frame,
  turning the lock-on camera by the same amount first so it does not lag), and the world update runs with the free-aim
  bit cleared, so the game treats it as a lock-on to an object: Vic moves as he does locked on with that gun (the game's
  free aim makes him stand still and puts the left stick on the aim). The camera and the HUD run with the bit set again
  (the centred free-aim camera and the circle, not the lock-on view and triangles). The dummy stays 12 m from Vic: the
  lock drops past ~30.
- **Shots through the circle**: the gun's line to the dummy and the camera's line through it cross only at the dummy,
  so near targets would be missed by the gun's line. The player's free-aim shot tests the camera's line instead (the
  line test's start redirected to a point of it level with the gun; GTA VC's PC version does the same with its
  third-person mouse aim), and the shot's end is put on it so the tracer agrees. The gun's own start stays for the
  muzzle effects.
- **Not an ImGui reticle** (the plan's first idea): the game draws its own circle exactly where the shots go, in the
  HUD's style and scale; a second one would only disagree with it.
- **Melee, thrown and scoped weapons keep R1** (lock-on is how fists fight; scopes are already free aim). Off by default
  for both devices: it changes how the game plays (easier than lock-on at range), so "authentic by default" holds it
  back, unlike the mouse camera. Mouse wheel is on by default: a binding, nothing in the game changes.
- Hip fire (the left button with a gun) also aims for the shot, held 0.6 s after it, so every shot goes through the
  circle as in a PC shooter.
- `VCS_FREEAIM=0` installs none of the hooks (the reference). The guest scratch for the shot's start sits at 0x60000 in
  the EE kernel's area, which nothing in the recompiled game uses.
- (later the same day, the user) Going into and out of free aim was too slow: the game blends the camera 400 ms in and
  350 ms out. Both blends are cut to 100 ms, only those two (the frames after free aim starts or ends): not an
  instant cut, which jumps the view by about 2 m, and the game's other blends (vehicles, cutscenes, the PS2
  lock-on) stay as they are.
- (the user, after that) A lag between letting go of the right button and the aim ending: the hip-fire hold (0.6 s
  after a left click) also held aims fired with the right button down - 15-17 frames after a shot. The hold is now
  for shots fired without the target button only, and letting go of it clears the hold: 2 frames from the release to
  the camera leaving the aim, shots or not (measured with the rig's `trace`).
- (the user, next) Leaving the aim felt quicker than entering it - the camera blends were the same (100 ms, measured),
  but entering also has Vic raising the gun. The exit blend is 200 ms now (the user's choice). And on release the view
  snapped to another angle for one frame: the follow camera's first update resets its heading; that frame's view is now
  turned back to the aim's heading in the hook after CCam::Process, before the frame's camera is built (the pad read's
  hand-back came a frame late). The hand-back needs the free camera's hook (VCS_FREECAM=0 leaves it out).
- (the user, next) With guns that stand Vic still the aim's end was instant, unlike the walking guns. The game does that
  on purpose: CCamera::CamControl jump-cuts from the lock-on camera to the follow camera when the weapon can't move
  while aiming (FUN_001F1698). Free aim answers that one question (its call in CamControl, only in the frames after the
  aim ends) with yes, so all guns end in the same blend; Vic still stands still with those guns while aiming, and the
  PS2 lock-on (free aim off) keeps its cut.
- (the user, next) A gun running dry in free aim switched to the lock-on and flicked in and out. The game puts the
  fists in Vic's hand when the gun is empty; free aim then let R1 through (fists lock on, as on the PS2), and retrying
  the free-aim entry (R1 + L3) while Vic was busy could leave the game a frame of R1 alone - a lock-on to a ped - before
  giving up and trying again. Now: once a free aim has ended because the gun went, R1 stays up until the button is let
  go (a gun back in hand, e.g. by the wheel, aims again); an entry the game answers with a ped lock is let go at once,
  and every retry waits 8 frames with R1 up. Starting with fists or melee is the PS2's R1 as before.
- (2026-11-01, the user) Jive Drive's shooting from the passenger seat still aimed with the left stick. That is the
  game's own free aim, which the mission starts itself (SET_PLAYER_DRIVEBY_MODE; game-internals.md 12c) - the only
  mission that does. With free aim on for either device the host turns its dummy in the car too: the mouse and the
  right stick, no button (the game aims all the time there), the left stick centred so the two don't fight. Kept from
  the game: the dummy at the gun's range, the world direction while the car turns, 20 deg up / 5 down; the shots go
  down the camera line as on foot. Both devices' sticks / mice turn it whichever setting is on: there is no button to
  tell the devices apart, and the left stick no longer aims. Free aim off is the PS2's left stick.
- (2026-11-01, the user, next) With the right stick aiming in Jive Drive's car, Circle (fire) was hard to reach. In
  that car aim only, the gamepad's R1 button fires: it is sent as Circle, and not as R1 (the keyboard's R1 keys are
  left as they are). Nowhere else changes: on foot R1 is the target button, and in the car
  with free aim off it is R1 (controls_check checks both).
- (2026-11-01, the user, next) Phil's shooting range also used the left-stick aim, and broke when the right button was
  pressed: the range forces the game's free aim on foot (SET_PLAYER_FORCE_FREE_AIMING, bit 53), and our own free aim's
  entry (R1 + L3) and exit (R1 let go) ended it - Vic back on the follow camera with no circle for the rest of the
  round. A free aim a script forces is now taken over like the car's (one path, `scripted` in vcs_aim.cpp): the mouse /
  right stick, no button, the left stick centred (Vic stands still there anyway), R1 left to the game (it does nothing
  there). The game's on-foot limits are kept: 110 deg either side of Vic's heading, 40 up, 20 down (the game applies
  them only while its own stick moves the dummy, so the host clamps). Asked, the user chose the gamepad's R1 firing
  there too (the same reach problem as the car): it fires in any free aim a script starts, and is R1 everywhere else.

## 2026-11-01 - The launcher draws itself (docs/LAUNCHER.md)

The user asked for the launcher to become a high-performance application with its own native UI and beautiful
animation - "not a sloppy web application", a purpose-built PC utility - multi-platform in design, Windows / D3D11 for
now, built the way uppies (D:\aislop\uppies) is. The user approved the plan and chose: rethink the flow, a custom title
bar, disc art behind Home later.

- **Own window, renderer and UI, no third-party code.** SDL3 + Dear ImGui go: ImGui's look is a tool panel, its
  SDL_Renderer path polled while idle, and SDL made the launcher an 81 s build. uppies' design instead: a platform layer
  (Win32), one instanced draw of SDF primitives (D3D11), immediate-mode widgets with retained animation state. 1 MB, 17 s.
- **Not the game's RHI.** It links ImGui and serves the game's presenter; the launcher wants exact control of
  presentation (latency waitable, tearing, device loss) and must stay a project CI builds with nothing downloaded. Its
  own five-call backend interface takes the same draw list on another API later (Vulkan on Linux), as uppies plans.
- **DirectWrite, not FreeType** (uppies' choice): nothing to bundle or license, Windows' own rasterisation. The atlas,
  cache and layout are platform independent; FreeType is the Linux rasteriser.
- **Three modes instead of pages**: Setup (a guided sequence: one step at a time, the rail filling in), Home (one big
  button over the sunset), Task (the running tool takes the window). Saves / Settings / Help are sheets. Every feature
  of the 2026-10-30 launcher kept its place (the parity list in the plan; LAUNCHER.md "The player's flow").
- **Custom chrome on a normal window**: WM_NCCALCSIZE / WM_NCHITTEST rather than a borderless popup, so snapping, the
  system menu, the min / max animations and the shadow stay Windows'.
- **Zero cost when idle**: the frame loop sleeps in the OS; tool output and process exit wake it (Process's reader
  thread and a thread-pool wait); clocks on screen ask for a frame once a second; nothing animates continuously except
  while a task runs (spinners, the bar's sheen) - never while the game plays (the game wants the GPU).
- **Motion is optional**: the launcher's own *Reduce motion* makes everything snap. (Later the same day, the user:
  panels opened instantly with it off. Windows' "Animate controls and elements inside windows" was off on the PC and
  also turned motion off. Now the launcher's setting alone decides; Windows' only gives its value until it is set.
  Then the user found reduce motion on after updating a release folder, whose launcher.ini didn't have the setting yet:
  that starting value was Windows' again. It is now simply off until the player turns it on, Windows' ignored.)
- **Tests stay hidden**: `--screenshot` renders offscreen on a fixed clock (same arguments, same picture), with
  `--frames` for transitions; `--hidden --quit-after` runs the real window loop unshown and reports its cost.

## 2026-11-01 - The launcher in light and dark, hot pink, no purple

The user asked for a light and a dark mode following Windows' (with System / Light / Dark in Settings), hot pink as
the primary colour - pink and white in light, pink and "a really nice black" in dark - no purple anywhere, other
colours sparingly. The dusk-violet palette and the sunset orange went: dark is neutral blacks and greys
(`#0A0A0B` behind, `#17171A` cards) with hot pink `#FF2E8B`; light is blush white behind (`#FFF4F8`), white cards,
warm greys for text and a deeper, slightly redder pink `#EC1A62` (the user's choice; white text on it stays readable). Teal stays for done, amber and red
for attention - signals only; decoration (the backdrop's sea lines, the playing indicator, the saves icon) is pink.
Colours are fields of a palette struct, so the two blend field by field: switching crossfades instead of jumping.
Overlays (hover tints, hairlines) are `theme::ink(a)` - white on dark, a deep pink-brown on light - and shadows
`theme::shade(a)`, softer and pinker on light. Light mode's scrim washes the content out (a translucent blush)
instead of darkening it. Screenshots default to dark (`--theme`), so docs and tests give the same picture on any PC.
The app icon was redrawn in the same colours.

## 2026-11-01 - Home's backdrop moves (the one exception to an idle launcher)

The user wanted Home's backdrop more sophisticated and "more alive". It became Vice City at dusk - the striped sun
behind palms and art deco towers, a sea that reflects them, stars at night - and it moves continuously but slowly:
stripes sliding into the horizon, clouds, swaying palms, windows, a beacon, rolling water lines, a wavering
reflection. A launcher that draws nothing when idle was a design rule (docs/LAUNCHER.md); this breaks it on Home
only, within bounds: 30 frames a second (motion this slow needs no more; 15 when the window isn't focused), never
while the game runs (it wants the GPU), never with reduce motion on (the scene stands still), never on the other
screens, never minimized. Cost: ~0.16 ms of CPU a frame, about 0.5% of a core. All with the renderer's primitives
(palms are chains of quads, each overlapping the next by 0.7 px so antialiased edges leave no seams; the reflection is
drawn opaque, sunk toward the sea's colour, so overlapping pieces don't show darker).

Next (the user): the palms went, the far row got windows of its own (smaller and dimmer), and no tower has a rounded
top any more (a parapet instead). Then jets (the user): a speck crossing high up every ~30 s - each flight's height,
slant, direction and pace from its number, so nothing is stored - laying a trail of soft dots that starts thin behind
it and widens, sinks and fades over 32 s; white on the light sky (stronger there, or it vanishes), sunlit pink at night
with a blinking strobe. They first all seemed to come from the right (the user): a flight's traits were hashes of
neighbouring numbers, which came out alike. Each trait is now a hash of the flight and the trait mixed apart; slots
(24 s, a random moment in each, some left empty) pair up flying opposite ways in a random order, and each flight also
has its own distance (a far one smaller, fainter, slower), trail life (22-40 s) and patchiness.

Then (the user) the sun's slices looked choppy, and the streaks across it drifted off and never came back. The slices
are the disc drawn through clip rectangles, and the renderer clipped by shrinking each quad to the clip: a clip edge at
a fraction of a pixel snapped to whole pixels, so slices sliding a few pixels a second stepped. The vertex shader now
takes the clip out to whole pixels and the pixel shader multiplies by the part of the pixel inside it - a moving clip
edge is antialiased (checked: an edge pixel's value now changes a little every frame), and a clip on pixel edges, all
the UI's, draws exactly as before. The slices' top edge sits on a whole pixel, where the disc's solid top meets the
first slice, so the two never leave a half-covered seam. The streaks are now scheduled like the jets: one every ~10 s
at a random place around the sun, fading in, drifting right for 35-55 s and fading out.

Then (the user) the jets' starting points and angles more random and varied. A flight's line is now a random point of
the upper sky (10-90% across, 8-32% down) and an angle up to 24 degrees either way; it enters and leaves where that
line meets the window's edges (a little outside them) or the skyline, so jets come in from the left, the right or down
from the top, climbing or descending, and its pace is in pixels a second (a short diagonal is over sooner). Trails
crossing the words on the left are dimmed to 30%, and fade out under the title bar, so a jet from the top seems to come
out from under it.

Two frame-loop faults turned up on the way: a sleep that ended on its timeout set `needsRedraw`, which also books a
settle frame, so every timed wake drew two frames (a timed wake now draws one); and minimized, a frame asked for at a
time already past made the loop wake at once, again and again - a core spinning behind the game while the "Playing"
clock was due (minimized, those requests now wait for the restore).

## 2026-11-01 - Pricedown in the launcher only when the player has it installed

The user asked for Pricedown (GTA's lettering) on the launcher's prominent elements, from a zip of Typodermic's free
desktop release (`Pricedown Bl.otf`). Its licence (Typodermic Desktop EULA v260817, in the zip) forbids distributing the
font (4.1), bundling or embedding it in a desktop application or other software (4.3) and including it in open-source /
publicly available software (4.6); the zip's guide says apps and software need another licence. So the font is not in
the repository or the exe: the launcher asks DirectWrite for an installed "Pricedown" (each player may install their own
copy, 2.1) and falls back to Bahnschrift. Used for three things only: the name on Home, Home's big button, the build
percentage (GTA's money counter is Pricedown). A pre-rendered image of the lettering (fixed artwork, 2.2) was not
chosen: digits for the percentage would be a reusable glyph set (4.5). A licence for embedding would change this.

## 2026-11-01 - vcs.exe runs with the UTF-8 code page (non-ASCII folders)

A player's report (issue #27): the built game stopped at start with "No mapping for the Unicode character exists in the
target multi-byte code page". Reproduced with the game in `build\run\nonascii\Игры é\game`: the folder comes from
`GetModuleFileNameW` (wide), and the first `std::filesystem::path::string()` on it throws, because MSVC's STL converts
to the system's ANSI code page (cp1252 here), which has no Cyrillic. A second case fails the other way: a data folder
with Cyrillic letters (a Windows user name, so `%LOCALAPPDATA%`) arrives through `getenv` as `?`s, and the game "cannot
write" its cache. The game and the runtime pass paths as narrow strings in many places (`loadELF(std::string)`, fopen,
the ini, the card), so converting each to wide calls was not the fix.

vcs.exe now has its own manifest (`src/vcs.manifest` through `src/vcs.rc`, the linker's default off with
`/MANIFEST:NO`): the same `asInvoker` as lld-link's default plus `activeCodePage` UTF-8 (Windows 10 1903 and later), so
the ANSI code page of the process is UTF-8 - `path::string()`, `getenv`, `argv` and the CRT's narrow file functions
all speak UTF-8 and round-trip any name. A resource, not `/MANIFESTINPUT`: lld-link merges manifests only when LLVM
was built with libxml2 (else it calls mt.exe), which a player's clang may not be. Older Windows 10 ignores the setting
and behaves as before.

## 2026-11-01 - The launcher on Linux: native X11 and Wayland, Vulkan, FreeType (docs/LAUNCHER.md "Linux")

The user asked for the launcher to support Linux as well - native, fast, high performance, drawing with Vulkan - and
approved the plan with three choices: **both window systems natively** (picked at startup), **our own icons on both
OSes**, and **CI on Linux now, a Linux release later**.

- **Same launcher, other files.** The launcher was layered for this (2026-11-01 above): `screens/`, `ui/`, the draw
  list, the glyph atlas and the model are shared; Linux adds `platform/platform_linux.cpp` + `platform/linux/` (X11,
  Wayland, D-Bus), `render/vulkan/`, `render/font_freetype.cpp`, `model/process_posix.cpp`, `model/system_linux.cpp`.
  The shared interfaces became UTF-8 (`std::string` argv, `const char *` titles and font names); only the Windows
  files widen. One `ui.hlsl` for both renderers: glslang compiles it (`tools/hlsl_spirv.py --target-env vulkan1.1`),
  its frame constants are push constants under `VCS_SPIRV`, and a flipped viewport keeps its D3D clip space.
- **No toolkit, no SDL** (the 2026-11-01 reasons stand: idle cost, build time, control). Xlib and libwayland-client
  directly, each with what makes it feel native: `_NET_WM_MOVERESIZE` / `xdg_toplevel.move` / `resize` so the window
  manager moves, snaps and tiles; fractional-scale + viewporter (exact scale); cursor-shape (the compositor's theme);
  XDND and `wl_data_device` for dropped discs; XSETTINGS and the portal for dark / light, animations, double-click.
  Wayland is chosen when `WAYLAND_DISPLAY` is set, X11 otherwise (`VCS_LAUNCHER_VIDEO=x11|wayland`).
- **Vulkan opened at run time** (`dlopen("libvulkan.so.1")`, our own table of entry points): a PC without a Vulkan
  driver gets a message instead of a launcher that doesn't start, and nothing links against the loader. Vulkan 1.1 is
  the floor (old GPUs, VMs; lavapipe in CI). Integrated GPUs are preferred: a launcher shouldn't wake a laptop's dGPU.
  Present wait (`VK_KHR_present_wait`) gives DXGI's one-frame latency where the driver has it.
- **Our own icons, both OSes**: Linux has no Segoe Fluent Icons, and bundling an icon font would be a third-party asset.
  `render/icons.cpp` draws each icon from signed-distance shapes on a 24-unit grid into the glyph atlas (one glyph to
  draw, overlapping strokes never darker, 1-pixel strokes snapped to pixel centres at small sizes). Windows' icons
  change with it (same design language, slightly different drawings).
- **System libraries only**: FreeType + fontconfig, libdbus-1, Xlib / Xcursor / Xrandr, libwayland-client / -cursor,
  xkbcommon - every desktop has them, so the launcher stays "no third-party code" in the repository and builds offline.
- **File dialogs through the desktop portal** (the desktop's own dialog, right on Wayland and in sandboxes), else
  zenity, else kdialog. **Packages through `pkexec`** (`build.py --install-deps`: the desktop's own password prompt, the
  logic in Python per AGENTS.md); without a polkit agent the launcher opens a terminal with the same command.
- **Hidden runs on Linux draw offscreen**: presenting to a window that is never shown can block forever under Wayland
  (FIFO waits for a frame callback); the frame loop is paced at the display's rate instead.
- Update checks and copying a DVD are hidden on Linux until there are Linux releases and a Linux `rip_disc.py`.

## 2026-11-01 - Card names the host can't store are %XX; the card is zipped before every change

Players lost saves after saving (game-internals.md 10f): VCS names a slot file after the last mission, and "1 Kill Phil:
Part 2" became an NTFS stream of an empty file after the game had deleted the old slot. And the game deletes a slot
before writing it, and its whole save folder after a failed write, so any crash or host error mid-save loses saves.

- **Names**: the characters Windows refuses (`<>:"/\|?*`), control bytes, bytes >= 0x7F (not UTF-8: they would throw
  under the UTF-8 code page), `%` itself, trailing dots / spaces and device names (CON, COM1...) are stored as `%XX`;
  the game sees the name it wrote. The same encoding on Linux, so a card folder copied between systems keeps working;
  a name Linux stored before is still found. Readable in Explorer ("1 Kill Phil%3A Part 2"), ASCII, reversible. The
  alternative - Unicode look-alikes (U+FF1A for `:`) - reads nicer but is a guess per character and not ASCII.
- **Recovery**: on Windows the card layer turns an empty file whose stream holds 900+ bytes back into the save it was
  meant to be, at the next listing (smaller streams are Windows' own, e.g. a download's Zone.Identifier). Players who
  lost a save this way get it back by starting the new build. If they have saved in that slot since (a new game), the
  recovered save goes to the first free slot rather than hide behind the newer one; the new save is never touched
  (recovery never writes over an existing file).
- **Backups**: before the first delete / write to an existing file / rename / format in 10 s, both cards go into
  `<data>/backups/auto-<date>.zip`, tools/backup.py's own format, so the launcher's Saves sheet lists them and Restore
  works unchanged; newest 10 kept (a card is ~0.5-1 MB, stored). In vcs.exe, not the launcher: players start vcs.exe on
  its own too, and only the card layer knows when a change comes. Saving into an empty slot changes nothing old and
  makes none. `PS2X_MC_BACKUP=0` turns it off.

## 2026-11-02 - Stutter work: the music stop no longer holds the level swap; textures made off the main thread

The user asked for stutters first (issues #16, #29), the game to behave as the original (PERFORMANCE.md 21).

- **The level swap's music stop happens at once.** Crossing between Beach and Mainland the game stops the radio stream
  and waits until it says stopped; CDSTREAM's music thread only sees the stop at its next refill IRQ, up to 448 ms - the
  PS2 stood just as long (game-internals.md 9a). Only that wait changes: the commands, their order and every reply the
  game reads are the IOP's; the stream just ends that much sooner (it is being stopped anyway, and the swap restarts
  the radio after the load - the stations it picks are the same as with the wait). Kept as `VCS_MUSIC_STOP_WAIT=1`;
  the sound replay comparison keeps the IRQ timing, since it checks the driver against recorded IOP times.
- **Textures are made by helper threads on D3D11** and, with anisotropic filtering, their mip chains built on the CPU:
  what the GPU gets is the same (static reference scenes identical), only who does the work and when. Vulkan keeps
  creation on the main thread: its first contents go into the frame's set-up commands, not thread-safe.
- **The stutter rig flies over the roofs** rather than drives the roads: the AI drivers the rig can make stood more
  than they drove, and the route has to be the same every run to compare builds. Streaming, texture bursts and level
  swaps come from the camera crossing new ground at speed, which the flight does.

## 2026-11-02 - Linux: Vulkan makes textures off the main thread when asked, not by default

The user asked whether the Windows stutter work helps the Linux port (PERFORMANCE.md 25). Everything but the texture
helper threads is portable code and was already in. Vulkan now supports free-threaded creation (staged first
contents, recorded at the next submit), but it stays off by default there: Vulkan's main thread only records a copy
per new texture (~8 ms a 350-texture burst; D3D11's creation was 52 ms), and on the 2-core laptop the two helpers
slowed the GS stage's decode more than they saved. `PS2X_HW_EARLY_TEXTURES=1` turns it on; a 4+ core Linux machine
should decide the default. The cruise takes a copy of the player's card (`build/rig/saves/user`) where the rig's
Fort Baxter card is missing; never the live card, and no new game (its first mission keeps the player out of the car).

## 2026-11-02 - Vulkan drops the GS shader's depth output; D3D11 keeps it until checked

The user's city hitches on the Linux laptop are partly the GPU (PERFORMANCE.md 26). The GS pixel shader's depth output
dates from the GL renderer, where NDC lost precision; with D3D / Vulkan's 0..1 depth the rasterised value is the same,
and dropping the output brings early depth rejection back (~1.5 ms of a ~10.7 ms GPU frame on the HD 520). Reference
scenes on Vulkan show no change beyond run-to-run noise. D3D11 is the reference renderer and its GPU has the headroom,
so it keeps the shader depth until a scenes compare on Windows says the same; then both can share the default.

## 2026-11-03 - Linux releases: an AppImage, glibc 2.35 and up, shipped with Windows

For the Linux launcher's first release the user chose **an AppImage** (not a tarball), **support for older
distributions**, and **Linux released at the same time as Windows**.

- **One AppImage, the release inside.** `tools/release.py --platform linux` makes `VCS-Recomp-<v>-x86_64.AppImage`:
  the launcher, its desktop entry and icon, and the release's files (the source, the scripts, `deps/` - no Python or
  FFmpeg: the system's) in `usr/share/vcs-recomp/release`. An AppImage is a read-only image, and the game is built
  inside its release folder (`generated/`, `build/`), so the launcher copies those files to a folder of its own at
  start - `$XDG_DATA_HOME/VCSRecomp/install` (`VCS_INSTALL_DIR`), next to the saves - when it is missing or holds
  another version (`tools/update.py install`). Only files whose bytes differ are written: after an update, Ninja
  rebuilds only what changed. The type 2 runtime is static (no libfuse2 on the player's system).
- **Updates replace the AppImage file** (`$APPIMAGE`, set by the runtime): `update.py apply` downloads the new one,
  checks its SHA-256 and the AppImage magic, and renames it over the old; the next start puts the files in place. No
  AppImageUpdate / zsync: the same update.py and the same GitHub release as Windows.
- **glibc 2.35 is the floor** (Ubuntu 22.04, Debian 12, Mint 21 and newer): CI builds the Linux launcher on
  ubuntu-22.04 with the C++ runtime linked in (`-static-libstdc++`), and release.py refuses a binary that needs a newer
  glibc (`--allow-new-glibc` for a test). 22.04's wayland-protocols is too old for fractional-scale and cursor-shape:
  CI unpacks a pinned 1.45 (`VCS_WAYLAND_PROTOCOLS_DIR`); 22.04's wayland-scanner reads it. The launcher builds with
  any clang or GCC (`toolchain.launcher_compiler_defines`), not only the game's clang 19.
- **The game on those older distributions**: they ship no clang 19, so the package step adds LLVM's own repository
  (apt.llvm.org, as it documents: its key - fingerprint checked when gpg is there - in /etc/apt/keyrings, a sources
  entry for `llvm-toolchain-<codename>-20`) before installing clang-20 / lld-20 / llvm-20. That root work is
  `tools/linux_packages.py`, standalone and short (it runs as root through pkexec or sudo); build.py and doctor.py
  only decide what it should do.
- **Shipped together**: on a `v*` tag each platform's job makes its file and uploads it; `publish` needs both and puts
  them on one GitHub release. A failing platform publishes nothing.
- Tools pinned by version and SHA-256 (no "continuous" builds): appimagetool 1.9.1, the type 2 runtime 20251108,
  wayland-protocols 1.45.

## 2026-11-02 - Fog: the game's own fog range as settings, not a screen-space overlay

The PC mods in `Mods/` (Proper Shaders VC, SkyGFX) add fog / atmosphere. A first attempt drew a coloured gradient over
the finished picture by screen height (presenter, `present.hlsl`): it fogged the sky, the HUD's band and the near ground
alike whenever the camera looked down, ignored depth, and its density defaulted to 0, so switching it on did nothing.
Removed.

The game already fogs every surface per vertex from its depth (game-internals.md 6l). `FUN_002F6008(start, end)`, the
RSL fog-range setter, is a leaf the camera calls once per frame; the fog settings scale its two arguments
(`src/game/vcs_fog.cpp`). That moves the fog in every renderer path at once (VU1, CPU HLE, native shaders, clipper),
keeps the time cycle's colour and weather (foggy weather starts the fog behind the camera), costs nothing, and at 100 %
passes the game's values through untouched - the authentic default, with `VCS_FOG_HOOK=0` as the switch back.
"Fog distance" stops at 100 %: the game's fog is total exactly at the far plane (2000), so going further would show
the draw distance's pop-in. A height / volumetric fog like Proper Shaders' needs the depth buffer in a post pass; that
is a separate step (docs/MODS.md).

## 2026-10-31 - PCSX2 PNG packs as optional host textures

The user supplied `SLUS-21590/replacements` and asked to use its PCSX2 textures in the native port. Accept those
filenames directly: PCSX2 pixel/palette hashes and format bits, including the older TCC bit. Keep the original
disc, raster uploads, renderer and fallback intact; match at the shared texture cache so the world, HUD and menus
all benefit. The option is off by default and applies after restart. Decode PNGs asynchronously, share one GPU
copy per pack image, and bound the replacement cache by bytes (512 MiB by default). Keep GS logical sizes separate
from physical replacement sizes, including non-integer scales and region wrap. Start with the format actually
provided (full-texture PNGs); DDS, archives and separate region/mipmap files are outside this first implementation.
The pack remains external, ignored by git and rejected by the release guard. See `docs/TEXTURE_PACKS.md`.

The user subsequently requested `textures/SLUS-21590/`: automatic discovery checks `textures/` beside the exe and,
for the default source build, in the checkout root before the older bare serial folders. The pack-check tool uses
this layout by default too. Tests were skipped at the user's request for this folder-discovery change.

## 2026-11-02 - Texture packs: DDS first, on upstream PR #8 (FORK.md T1)

Upstream PR #8 (rofhima13) reads PCSX2 PNG packs: PCSX2's names and hashes, an async loader, a GPU budget. The user
wanted DDS instead, believing the PS2's own files are DDS; they are not (the GS's paletted formats in `.XTX` / TXD),
but DDS is still the better pack format, for other reasons: block-compressed and mip-mapped as the GPU samples it,
nothing decoded on load, 4-8x less GPU memory than a PNG, and PCSX2 reads it under the same names - the user's HD
pack is 6279 BC7 DDS files. So the PR was taken as it is (its commits, its author) and DDS added beside PNG rather
than replacing it: existing PNG packs keep working, a name in both formats takes the DDS. DDS headers are read by our
own code (a header and a copy of the levels: no new library), BC1-BC5 / BC7 and 32-bit RGBA / BGRA; sRGB variants
are sampled as stored, like PNG values (PCSX2 does the same). A block-compressed DDS on a GPU without BC sampling
keeps the original texture. A dump (`VCS_TEXTURE_DUMP=1`) writes uncompressed DDS rather than PNG: lossless, no PNG
encoder to vendor, and texconv / Compressonator take it straight to BC7.

## 2026-11-02 - A mod loader, PC Mod Loader's way (FORK.md T1b)

The user asked for texture packs switchable in F2 and then for "a mod structure like Mod Loader" (the PC plugin; a copy
is in `Mods/ModLoader/`). Both are one feature: a texture pack is the first kind of mod content. Choices:

- `modloader/`, not `mods/`: on Windows `mods/` is the existing `Mods/` (PC mods kept for reference); and the name is
  the one GTA players know. Beside vcs.exe, or the checkout's root when vcs.exe is in `build/`.
- Mod Loader's own `modloader.ini` format (priorities 1-100 with 50 the default and 0 off, ignore wildcards,
  IgnoreAllMods, the profile name) rather than one of our own: what players already know, and the PC docs apply.
  The per-mod on / off switches live in vcs.ini (`mods.<name>`) like every other setting, so the menu is the registry's.
- Load mods and Replace textures default on: installing a mod is the opt-in, and with no mods nothing changes (the
  authentic default holds). Test runs are the exception - hidden runs load nothing unless asked (`VCS_MODS`,
  `VCS_TEXTURE_REPLACEMENTS`, `VCS_TEST_MODS`), so reference captures never pick up a player's mods.
- Live: indexing a pack takes ~20 ms, so a switch re-indexes on the main thread and the GS stage swaps the pack at its
  next frame (GPU textures of the old pack dropped there). New mod folders need a restart: the settings registry is
  built at start-up, and a menu row per mod is worth that.
- PC `.dff` / `.txd`: VCS has neither format (RSL models, XTX textures); the mod loader will take VCS's own files
  next (T1c), and importing PC formats is a research task of its own (T9; docs/MODS.md).

## 2026-11-02 - Mod priorities: sliders in F2, kept in modloader.ini

Two mods with the same textures (the user's HUD Remastered and HD Texture Pack share 152) need the player to say which
wins, in the game. A slider (1-100) beside each mod's switch; the value stays in `modloader.ini` - Mod Loader's file,
which the game now makes in `modloader/` when it is missing, visible and editable - rather than vcs.ini, so there is
one source: the menu writes the file, Rescan reads an edited one. The settings registry got two small things for it:
`Spec::saved = false` (a value kept elsewhere: `read` gives it, onChange stores it) and a Toggle's `partner` Integer
drawn as a slider in the same row. At equal priority a DDS still beats a PNG, so a PNG mod has to be set above a DDS
pack it shares textures with.

## 2026-11-03 - Game files from mods: entries over the stream, not rebuilt archives (FORK.md T1c)

The player's idea - change only the parts of an IMG, from folders - is how it is done: the stream layer, already native,
lays a mod's bytes over each disc read that covers the entry, so no archive is rebuilt and mods stay small and
stackable (priorities). GTA3PS2.IMG has no directory on the disc, so names go through the game's own keys (~CRC32 of
the upper-case model name): `admiral.mdl` works without a name list; entries without a known name use their streaming
index (`#1234.bin`, as `tools/img.py extract` writes them). Larger entries are refused for now: the game allocates the
entry's recorded size.

## 2026-11-03 - PC VC cars: converted to VCS's own formats, in the mod loader (FORK.md T9)

The player wants VC PC car mods to work as they come (`.dff` + `.txd`). Of the routes (docs/MODS.md "T9 findings"),
converting into VCS's `ldm` / `tex` resources was chosen over drawing PC models on the host: the game's own paths draw
the car (paint, damage parts, reflection, LOD), nothing in the renderer changes, and T1c already carries the files.
The conversion was written in Python first (`tools/dff2mdl.py`, `tools/txd2xtx.py`: easy to inspect, the reference),
then ported to C++ (`src/mods/rwconv.cpp`) so the mod loader converts while the game loads - no step ahead of time, no
Python needed to play. The two are kept byte for byte equal (`tools/rwconv_check.py`: 236 / 236 files of the player's
mods), so either can be used to look into a result. Choices inside:
- The VCS car of the same name is the template: handling, collision, wheels, sounds and model info stay VCS's (the
  SA PS2 tutorial's advice too: keep the original collision).
- A `.txd` is converted only when a `.dff` of its name is in the same mod: PC mods' other dictionaries (`hud`,
  `generic`, `particle`) hold VC's textures under names VCS uses for its own.
- Textures are halved to 128 pixels and quantised to 4 / 8 bit: the format every texture on the disc has (VCS's own
  vehicle textures are 128 at most); unpaletted ones would weigh far more in the PS2's memory.
- Python 3.12's `sum()` compensates rounding: the tool sums left to right itself, so its output does not depend on the
  Python version, and the C++ matches it.

## 2026-11-03 - Larger VC models: more memory for the game, not smaller models (FORK.md T10)

Converted VC cars are 4-5x VCS's (VC's Xbox models have about four times the triangles; VCS's own strips are no
tighter than ours), and VCS streams everything into a fixed 5.4 MB heap. Shrinking the mods (fewer triangles, smaller
or 4-bit textures) would trade away what the player installs them for. The player's call: give VCS more streaming
memory instead (T10, issue #15: a larger EE RAM, the streaming heap built there). Until then the conversion stays
lossless (only shorter strips); textures stay 128 px, 4 or 8 bit like the disc's.

## 2026-11-03 - Streaming memory past the PS2's 32 MB, the game's own layout kept (FORK.md T10)

- The host holds 64 MB of EE RAM, but the game is told 32 MB (`GetMemorySize`, the main thread's stack top): its main
  heap, stacks and every address it derives stay where the PS2 has them, so the authentic setting runs the same code
  on the same addresses. Only the streaming heap moves, to `0x2000000`, where nothing of the game's was.
- The heap is built by the game's own `FUN_002bb768` (a tail call from the hook, rule 2 of AGENTS.md): its allocator,
  defragmenter and out-of-memory path are the game's, unchanged. The 5.4 MB the game no longer takes stay free in the
  main heap (24.2 -> 18.6 MB in use): slack for the rest of the game.
- CStreaming's cap of 51 resources is lifted only on the larger settings and only for the request's own check: with
  more room the cap, not the bytes, would decide what stays. The make-room loop (bytes) is the game's.
- 30 MB at most: 64 MB minus the guest's 32 minus a 2 MB reserve. Restart to apply (the heap is built once at boot).

## 2026-11-03 - PC weapon mods: VCS's HUD icons stay, VCS's place in the hand (FORK.md T11)

- A VC weapon's `.txd` carries VC's HUD icon under the weapon's name, drawn in VC's style; VCS's HUD is VCS's, so its
  icon stays (authentic by default). VC's icon is kept in the dictionary as `<name>_pc` (a model that textures itself
  with it still finds it); an option to show the mod's icons can come later.
- A VC weapon goes where VCS holds its own: first it went in where VC has it, and the player saw every weapon wrong in
  Vic's hand (2026-11-03: grenades inside the hand, the chainsaw's handles beside his hands, the pistol into his
  palm). VCS's weapon models are VC's own meshes moved and turned in the hand, and VCS's animations hold them there.
  So each weapon has a transform from VC's place to VCS's, fitted once from VC's original models onto VCS's
  (tools/weaponfit.py, from the player's own VC PC files), carried as a table (dff2mdl.WEAPON_PLACES,
  rwconv.cpp kWeaponPlaces) and applied to any mod of that weapon: VC mods sit where VC's originals do. The
  alternative, fitting each mod onto VCS's model, was tried and rejected: different shapes (an M4 onto VCS's longer
  M16) have several equally good fits 13 cm apart. The table holds numbers, not game data.
- A texture whose sides are not powers of two is resampled nearest to the powers of two below: the halving after it
  averages, and the C++ and Python converters give the same bytes with integer maths.

## 2026-11-03 - Draw distance: the objects through the game's own scale, the population apart (FORK.md T2)

- Object distance writes CRenderer::ms_lodDistScale (0x4887F8), the factor VC's renderer already has for this: the
  camera's LODDistMultiplier keeps its own rules (70 / FOV, more above 55 m, at most 2) and the setting multiplies the
  result, as VC PC's draw distance slider does. Nothing else reads the scale; 100 % writes the game's 1.0.
- 100-400 %: below 100 nothing is gained (authentic is the default); 400 % already draws crowds the length of Vice
  Point beach for ~6% of the frame (PERFORMANCE.md 33).
- The population is not tied to it: CCamera::Process copies GenerationDistMultiplier before the scale, so cars and peds
  are made and removed as on the PS2. Scaling those distances spreads the same counts over a larger area (emptier
  streets); that step (T2 step 5) raises the counts with it, separately.
- The city's detail is another mechanism (cells, game-internals.md 9h) and gets its own setting, not this one.

## 2026-11-04 - T2 as a beta; distant traffic drawn, not simulated (FORK.md T2)

- The player's call: the population and distance work goes into the fork as a **beta**, every setting at the game's own
  value by default and its section marked "(experimental)" - not held back until all of T2 is done. What is left (the
  extra population's cost, LOD as Project2DFX) goes in follow-up PRs.
- Dense streets cannot all be simulated at 70 fps (each car / ped runs the PS2's code, ~0.085 ms of the game thread,
  PERFORMANCE.md 34): beyond the simulated traffic, cars are **drawn, not simulated** (DISTANT_LIFE.md) - proxies on the
  path network, drawn by repeating a real car's draws in the native renderer (its own models, textures and colours;
  no converted assets, nothing extra in guest memory).
- The proxies' cars come from cars the game renders; one car of each model near the camera is added to the vehicles'
  visible list when it is off screen (its own draws culled natively), so proxies never depend on what is on screen.

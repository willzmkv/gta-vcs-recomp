# Fully native: the steps left (2026-10-24)

The roadmap's "native" (NATIVE_PORT_ROADMAP.md "What done means", point 2): in a normal frame nothing draws through the
emulated GS or VU1, no IOP code is interpreted, audio comes from a native mixer - the emulated paths kept as
switchable references. This is the working list for the rest of it; update a step's status here as it moves and
write the details where they belong (game-internals.md, NATIVE_RENDERER.md, RSL_SUBMISSION.md, PROGRESS.md).

## Where it stands

**Native, 2026-10-26** (the roadmap's point 2): in a normal frame nothing draws through the emulated GS (`gs_draws`
0) or VU1 (`vu1_programs` 0), no IOP code runs, and the sound comes from the SPU2 core behind the ported libsd
(DECISIONS.md 2026-10-24). Checked over the reference scenes, the tour subset and all 95 missions of the smoke test
(`tools/missions.py`, 45 s each from a fresh load): every mission ran (no crash, no hang), GS-path draws 0 in all of
them after MAR_A1's second world image was accepted (game-internals.md 6d), and `vu1_programs` 0 in every perf sample
of every mission (the tool records the worst value, `max_vu1_programs`). The frame reaches the renderer as draw
items in the chain's order; what the chain still holds is the frame's structure (the root lists' fixed tags in
`.data`, the items' tags, NEXTs over runs taken in place). The emulated GS, VU1, VIF packet path and IOP stay as
switchable references.

- **VU1**: no VU1 program runs in a normal frame (game-internals.md 6d; `vu1_programs` 0 in the reference scenes and
  the tour subset).
- **IOP**: streaming and both sound engines on the host, the emulated IOP off by default (milestone 6).
- **3D geometry**: world sectors and model instances drawn natively, from recordings at the RSL draw calls
  (RSL_SUBMISSION.md phase 2); the packet path is the switch-back.
- **Counter**: the draw-source counter (NATIVE_RENDERER.md "Draw sources") says what still goes through the GS path -
  perf line `gs_draws` / `gs_path2` / ..., the rig's `gsdraws`, tour / missions records.

## The steps

| # | Step | Size | Status |
|---|---|---|---|
| 1 | The last GS draws | S-M | done 2026-10-24; the 3 render-target reads 2026-10-26 (`gs_draws` 0) |
| 2 | Loading screens in the native layer (there is no title menu) | S | done 2026-10-24 |
| 3 | Textures and render targets without GS memory | L | done 2026-10-24 (3a-3d) |
| 4 | Draw-call replay (3D without building packets) | M-L | done 2026-10-25 (4a-4c; draws whose flush calls plug-in callbacks stay packets) |
| 5 | An empty chain | L | done 2026-10-26 (5a-5f, 5h; 5g measured and not needed) |

### 1. The last GS draws

About 15-20 draws a frame still go through the GS front end, all PATH2 (VIF DIRECT, the game's 2D / effect packets):
the reflection map built in FBP 420 (a 128x128 sample of FBP 140 plus a sun sprite), coronas / light sprites,
colour-filter quads, fades and the depth-tested self-copy over the sky. Make each one a native draw (from its call
site or as a recognised packet, as the post passes did), checked against the emulated picture (scenes at 1x). The
ones that sample a render target stay tied to GS memory until step 3; the counter shows which.

Done when: `gs_draws` reads 0 (or only render-target reads waiting for step 3) in the reference scenes, the tour
subset and the missions.

**Done 2026-10-24** (NATIVE_RENDERER.md "PATH2 packets recorded whole"): every PATH2 packet that draws was Im2D's
(one PACKED ST / RGBAQ / XYZ2 tag) or an A+D effect strip; both are recorded whole now, checked exact (5M+ packets at
4:3 and 16:9, HUD-layer membership included). GS-path draws 15.8-18.5 -> 3.0 a frame (tour subset max 3.0). The three
left read render target 140 (the reflection map's two copies into FBP 420, the self-copy over the sky) and set state
before their PRIM: step 3. Missions not re-run for this step.

### 2. Loading screens and the title menu

The last 2D outside the native HUD / menu layer (PROGRESS.md milestone 3 step 2: "Loading screens and the title menu
over the flyover: not looked at yet"). Same method as the menus: their Im2D / sprite calls drawn in the native layer
at the window's size, `VCS_..._NATIVE=0`-style switch back, scenes compared at 4:3 / 16:9.

Done when: the loading screens and the title / start menus draw from the native layer, sharp at 1080p, the static
`loading` scene unchanged at 1x.

**Done 2026-10-24** (game-internals.md 6f "Frames outside gameplay"): `LoadingScreen` (FUN_0021E170) opens a context
like the menu's; the static `loading` scene 1.12 against the reference, sharp at 1080p, `VCS_LOADING_NATIVE=0` = before.
There is no title menu: the game boots into the opening sequence (Start does nothing there) and the frontend menu is
the pause menu, native since milestone 3.

### 3. Textures and render targets without GS memory

RSL_SUBMISSION.md phase 3. Decode the game's rasters (`.XTX`: PSMT4 / T8 + CLUT, game-internals.md 7) into GPU textures
when they are loaded or streamed, keyed by raster; native draws bind them directly and the game's VRAM uploads stop
mattering. Render targets read as textures (reflection map, bloom, frame copies, the radar) become native targets -
this is also what lets the radar and reflection map draw at the window's resolution (PROGRESS.md milestone 3 step 4).
Revisits DECISIONS.md "Textures from the GS backend's decoded cache".

Done when: a normal frame binds no texture decoded from GS memory; the reference scenes match at 1x.

**What GS memory still does (found 2026-10-24).** Render targets already live on the GPU (`m_rts`: a draw that
samples one reads the target directly); the emulated GS memory carries the textures. VCS streams them: every bind
(`FUN_002F62B8`, game-internals.md 6i "Texture streaming") uploads the raster's mips and its CLUT from RAM into a
fixed VRAM slot. The renderer queues those uploads (lazy uploads), keys its GPU textures by the upload's content and
decodes a texture from emulated VRAM only on a cache miss - but every bind's CLUT load read the palette back from
VRAM, which wrote the queued uploads into it (swizzled): ~25% of the render worker's busy time while driving.

Phases:
- **3a - palettes from the upload, no VRAM in steady state** (done 2026-10-24): CLUT loads read the queued upload's
  data; uploads without data no longer force the queue. Swizzled VRAM writes while driving ~245k -> ~7 per 5 s; render
  worker idle 27% -> 41% of the drive. Details below.
- **3b - the uploads' own cost** (done 2026-10-24): the allocations, copies and the serial hash around each streamed
  upload. Details below. Left: the copy into the queue and the hash itself (`UploadImage` ~3.8% of the worker). The
  copy goes only if the queued upload can point at the pixels where they arrive - the chain's copy, alive while the
  chain is processed, with the uploads still queued at its end copied then - which needs the image continuation to
  reach the GS without being re-tagged into a buffer of its own (a GS front-end entry point: a full rebuild).
- **3c - textures decoded from the upload data** (done 2026-10-24): the cache-miss decode reads the upload's data
  instead of VRAM. A normal frame now needs no emulated VRAM for textures; the loading screens' art still goes
  through it. Details below.
- **3d - identity by raster, not by VRAM address** (done 2026-10-24): textures named by the game's raster (its RAM
  address), so draws made without packets (steps 4 and 5) can name their texture; render targets named by what they
  are; the reflection map at the window's resolution (the radar already was: it has no target of its own). Details
  below.

**3a done 2026-10-24.** `HwBackend::LoadClut` looks for the queued upload that last wrote every 256-byte block the
CLUT load reads; when one whole upload at the CLUT's base, in its format, covers the pixels, the palette is loaded from
its linear data (`GSCpuBackend::LoadClutFromRect`: the same CLD rules and CSM1 / CSM2 entry order as the VRAM load)
and nothing is written. Upload calls without data (the header packet's IMAGE tag, its pixels in the next DIRECT) no
longer force queued uploads out. Checked with `PS2X_HW_CLUT_VERIFY=1`: 1.5M loads on the drive, 1.2M over the
reference scenes, all the same as from VRAM. Reference scenes ok; tour classes unchanged, frame time median 11.7 ->
11.1, p90 19.1 -> 15.6 ms. `PS2X_HW_CLUT_FROM_UPLOAD=0` = from VRAM.

**3b done 2026-10-24.** A worker profile with call chains on the drive put the rest of the texture streaming in
allocations and copies around each packet: the GIF arbiter copied every packet into a new buffer before processing it,
`submitVif1DirectData` built a new buffer to re-tag each texture's pixel DIRECT, and the upload queue allocated (and
freed) a buffer per upload; the content hash was one serial multiply chain. Now: a PATH2 packet that would be drained
at once into an empty arbiter queue is processed in place (same order, same path tag); the arbiter's, the re-tag's and
the queue's buffers are reused; the hash runs four lanes. Worker profile on the drive: busy ~59% -> ~50% of samples
(allocator and `memset` gone from the top, `memmove` 4.6% -> 3.7%). A/B against the 3a build, two alternating rounds:
standing worker 3.7 -> 3.4 and ~4.3 -> 3.8 ms; the drive 10.6 -> 8.8 ms in one round, level in the other (route
variance). Reference scenes ok, tour classes unchanged.

**3c done 2026-10-24.** On a texture cache miss, `decodeFromQueuedUpload` takes the queued upload that is the latest
writer of every block the texture reads (the CLUT's rule) and reads each texel's bits out of it at the texel's own VRAM
address: `GSMem::PixelBits` gives a pixel's first bit and width in VRAM with the same swizzle tables as the reads and
writes, the upload's pixels are indexed by theirs, so a texture read in another format than it was sent in (T8 sent as
CT32) decodes the way the GS would. The texels then go through `DecodeTexture`'s own conversion (CLUT / TEXA,
`GSCpuBackend::DecodeTextureFromTexels`). Checked with `PS2X_HW_DECODE_VERIFY=1` over the reference scenes: every one
of ~4,100 decodes the same as from VRAM. Over the scenes 4,124 decodes came from uploads and 4 from VRAM - the loading
screen's art (T8 512x512 at TBP 8960), whose upload arrives in pieces and is written directly; while driving none from
VRAM, and 0-36 uploads a 5 s window written into it. No texture in these runs was read in another format than it was
sent in, so that case is handled but not yet exercised. Reference scenes ok; tour unchanged (the known fall-through
point at (-925, 1175) classed "water" this run - the same fall, landing in a water state). `PS2X_HW_DECODE_FROM_UPLOAD=0`
= from VRAM.

**3d done 2026-10-24.**
- **Render targets by what they are**: the game names its buffers (`gsHwSetTargetRole(fbp, fbw, name, windowScale)`,
  `src/game/vcs_display.cpp`): frame 140, shown picture 0, Z buffer 280, reflection map 420 / 128 wide, bloom 420 /
  452, trails 384. `PS2X_HW_RT_LOG=1` lists the targets with their names, sizes and scales every 5 s. The GPU targets
  stay keyed by FBP inside the renderer (the game reuses VRAM, and a draw names only its FBP).
- **The reflection map at the window's resolution**: with "Internal resolution: Match window" it takes the frame's
  scale (1080p: 384 x 309 texels instead of 128 x 128) - it is only ever sampled (vehicles' paint), never copied 1:1;
  kept at 1x when the GS bloom shares its memory (`PS2X_HW_POSTFX` without b) and with
  `PS2X_HW_REFLECTION_SCALE=0`. At 1x nothing changes. (2026-10-30: the scale goes by the FBP, not the draw's width -
  the native bloom's write-back into FBP 420 at width 4 had flipped the target back to the integer scale every frame;
  PERFORMANCE.md section 15.) The reflection is faint by design (the game blends it in at
  ~15%, FIX 19), so the picture changes little. (2026-11-06: in fact it changes nothing. Both the copy into the map and
  the paint's read of it go through `fetchTexel`, one target pixel per PS2 texel, so only the map's 128x128 PS2 texel
  centres are ever read. Car paint at Match window differs by a mean 0.13 / 255 with and without the scale.
  game-internals.md 6g has the reflection map and why it flickers.)
- **The radar** needed nothing: a frame dump at Fort Baxter shows it drawn straight into the HUD layer from 128x128 T4
  tile textures under a scissor - at the window's resolution already (game-internals.md 6f corrected: the 128x128 at
  FBP 420 had been taken for the radar's; it is the reflection map).
- **Textures by raster**: each upload's pixels are a REF to the raster in RAM (`NOP; DIRECTHL n`, game-internals.md 6i).
  The RSL chain observer records where each such payload lands in the chain's copy (packet buffer and CALLed packets
  alike); the VIF interpreter reports each DIRECT's data (`ps2chain::notifyDirect`), and the one at a recorded offset
  tells the renderer whose pixels follow (`gsHwSetUploadSource`). A queued upload, and the upload info a texture is
  keyed by, carry that address; recordings of CALLed packets keep it for their replays. A texture made from such an
  upload is named by the raster (`gsHwTextureForRaster(address)` -> its key) - in any format it was sent in (T4 as
  CT16, T8 as CT32: then the queued upload that last wrote all its blocks names it). Over the reference scenes 4,112
  textures made, all named; while driving all named (~725 rasters). Reference scenes ok, tour unchanged.
  `VCS_RSL_RASTERS=0` = no names. A first attempt put a VIF MARK before the raster REF's payload - it landed inside the
  DIRECTHL's data and broke the stream; found at once by the RSL counts (replays collapsed), replaced by the offset
  lookup.
- Not done here: the GPU targets themselves are still found by FBP when a draw samples one; a draw made without
  packets will need its target by role (steps 4 / 5).

### 4. Draw-call replay

RSL_SUBMISSION.md phase 2's planned form. The world / model draw hooks replay their recordings directly instead of
writing packets, so the game thread no longer builds, copies (~3.5% of it while driving) or flushes (1.3%) those
draws; the worker gets the draws as items in chain order. The recordings' validity (content hashes) has to be
decided at the draw call - the packets' bytes are not copied any more (DECISIONS.md 2026-10-24 on why the copy was
kept).

Done when: world and model draws make no packets; verify mode against the packet path 0 differ; drive game thread
measurably lower (A/B, alternating builds).

**Sizing (2026-10-24, `VCS_RSL_MEASURE=1`, driving).** Per frame the chain copy is 4.6 MB, of which the marked world /
model blocks are 2.9 MB (740 blocks) and the distinct ones 2.4 MB (470): checking each distinct block's bytes once a
frame would read nearly as much as copying them. Validity without reading the bytes is the prerequisite - for skipping
the copy now, and for draws made with no chain at all (step 5).

**4a - write watch (done 2026-10-24).** Guest RAM is allocated with the OS's write watch (`runtime/ps2_write_watch.h`,
Windows `MEM_WRITE_WATCH`): every write is noted - guest code, host threads, the kernel reading the disc into it (the
stream thread does: page protection was ruled out for that reason) - at no cost until asked. At each VIF1 chain kick
the written pages are folded into per-page generations: 0.18 ms per frame, ~470 of 8,192 pages written per frame. The
RSL observer keeps, per marked block, the pages it is made of (a world REF's payload; a CALLed packet's tags and
payloads) and their generations at its last copy: 84% of copied blocks were unchanged since their previous copy. Write
watch costs guest writes nothing measurable (A/B, alternating: not slower).

**4b - unchanged blocks not copied (done 2026-10-24).** At the kick a world or model block may go uncopied when its
pages are unchanged since a copy the worker has confirmed its recording for (the worker hands confirmations - recording
key, chain - to the game thread at the end of each chain; the game thread keeps per block the copy since which its
pages are unchanged). The walker then writes only the MARK qword with the length word `kSkippedLength` and, after a
CALL, goes on at its return address as if the packet had returned. The worker replays the block's recording by key;
should it be unusable there (VIF mode / cycle differ), the block is rebuilt from guest RAM - unchanged by
construction (a small chain walker for CALLed packets). Results:
- Checked with `VCS_RSL_SKIP_VERIFY=1` (decide, copy anyway, compare the copy's hash with the recording the skip would
  have replayed): 1.1M decisions on the drive, 1.27M over the reference scenes - 0 wrong.
- Per frame while driving the chain copy fell from 4.6 MB to 2.1 MB (marked blocks 2.9 MB -> 0.37 MB); ~206k blocks
  per 10 s replayed without a copy, none rebuilt from guest RAM.
- A/B, same build, `VCS_RSL_SKIP=0` vs on, two alternating rounds: drive 50.1 / 50.4 -> 54.1 / 53.5 fps, render worker
  9.1 -> 8.5 ms; the game thread unchanged (the copy saved is spent again on the fold and the snapshots' bookkeeping).
  Reference scenes ok, tour unchanged.
- "Other call" packets (FUN_002F80A0's shared state packet, the per-frame ones) are skipped too (2026-10-25). At first
  skipping them lost state - VU1 programs ran again because stage tables were missing - and about a quarter of the
  skips decided for them never reached the worker as skip marks. The walker and the observer's open-block tracking
  agreed after all (every chain: skips decided == skip marks written); the marks were lost in the worker. The MARK
  handler's path for a block it records no longer ("changing", `rec.skip`) returned `12 + size` bytes to skip while
  the caller adds the MARK qword's 12 bytes itself: 12 bytes too many swallowed the next tag's VIFcodes - with them,
  when it followed directly, the next block's skip MARK. Before other calls were skipped this only cost the next block
  its recording. Fixed; `VCS_RSL_SKIP_KINDS` (a bit per kind) now defaults to all.
  - On the way the walker gained a safety net (`VifBoundaries`, ps2_memory.cpp): it follows VIF command lengths through
    what it copies and puts a MARK only at a command boundary, and gives a block no length when its payload ends inside
    a command. In VCS it never refuses one (`PS2X_CHAIN_LOG_REFUSED=<n>` logs refusals).
  - Checked: `VCS_RSL_SKIP_VERIFY=1` 759k decisions driving and 1.30M over the reference scenes, 0 wrong; with every
    kind skipped no VU1 program runs in gameplay (`vu1_programs` 0). Reference scenes ok, tour unchanged (15 ok, 2
    relocated, 1 falling as before).
  - Timing: A/B `VCS_RSL_SKIP=0` vs on (all kinds), two alternating rounds under background load: stand unchanged,
    drive game thread 19.3 / 18.7 -> 22.7 / 20.6 ms and fps 43.5 / 44.9 -> 40.7 / 44.9 - within this machine's noise,
    not a measured loss; a profile of the drive puts the walker's work and `skippable` at ~1-2% of the game thread.
    Measure again on a quiet machine.
- **The recordings' memory (2026-11-10, GitHub issue 115).** Nothing ever dropped a recording, and their keys never
  stop coming: a model streamed back in sits at another address, the game's own packets at other places in its buffer
  (PERFORMANCE.md 31: 2.3 GB an hour on a route across the map). They now have a limit - `VCS_RSL_RECORDINGS_MB`, 256;
  the least recently used go first; 0 is no limit, as before. The skip makes dropping one a matter between the two
  threads: the game thread may skip a block on the strength of a confirmation it holds, and the worker's fall-back
  for a block without a recording reads guest RAM while the game thread is already in the next frame. So the worker
  first withdraws its confirmation (an entry with the chain `kWithdrawn` in what it hands over at each chain's end);
  the game thread applies it at its next kick, before that kick's skip decisions, and names the batch in the chain's
  table (`ChainTable::withdrawn`); the worker frees the recording at the end of that chain - unless a chain from
  before the withdrawal used it meanwhile: then it stays and is confirmed again. The game thread's maps go with it
  (the confirmation erased; page snapshots and "flattenable" notes no chain asked for in 1,800 chains dropped once
  there are more than 16,384).
  - A skipped block still always finds its recording: 12.3M / 15.5M blocks skipped in 15 minutes with the limit at
    256 / 64 MB (8,792 / 30,827 recordings dropped), none rebuilt from guest RAM.
  - With the limit at 24 MB - less than one place in the city needs, so recordings are dropped all the time (19,527
    and 16,553 in 6 minutes): `VCS_RSL_SKIP_VERIFY=1` 4.87M decisions, 0 wrong; `VCS_RSL_WORLD_VERIFY=1` 7.32M
    recordings the same as the packet path, 0 differ.

**4c - draw calls as items (done 2026-10-25).** The world / model draw functions (`FUN_002F7EB0`, `FUN_002F7D58`,
`FUN_002F80A0`) are replaced whole when their state flush is native (game/vcs_rsl_native.h): the flush (when it is all
CNTs - else its tags stay in the buffer, before the item), the queued GS registers and the block become an **item**, and
the packet buffer gets one 16-byte CNT whose two VIFcodes - MARK `0x8000 + index`, a NOP holding `0xD4` and the frame's
number - keep the draw's place in the chain (DECISIONS.md 2026-10-25). The GS packet and the CALL / REF are not
written. At the kick, after the write watch's fold, each item's block is decided as a marked block is: skipped when
unchanged since a confirmed copy, else copied from guest RAM by `flattenBlock` (the walker's rules: tags' upper halves,
payloads, one nested CALL; the pages it is made of and its raster uploads noted as the walker's observer notes them).
At the MARK the worker runs the item's VIF bytes (the flush's UNPACKs / MSCALs, the GS registers' DIRECTHL) and replays
or processes the block - as the packet path, through the same interpreter and recordings.
- **What stays packets**: draws whose flush calls plug-in callbacks or `FUN_001CFAE0` (guest code; ~15% of draws
  while driving, ~1% standing in the city); a model block until the walker has once copied it as `flattenBlock`
  copies it; and every draw while `FUN_00240ED8` caches a sector (game-internals.md 6i: the sector's packets are copied
  into the heap and sent again on later frames - found because item tags from old frames turned up in later chains,
  drawing the wrong blocks and, for a cached sector drawn as an item, recursing into itself). Item tags carry the
  frame's number; a tag of another frame is counted and not drawn (0 in the runs below). `FUN_002F80A0` with its
  shared state packet (bit 31) keeps the flush, GS packet and that CALL in the buffer and makes the block an item.
- **Checked**: `VCS_RSL_ITEMS_VERIFY=1` (the original writes its packets; they are compared with what the item stands
  for, and every block the walker copies with what `flattenBlock` copies) - driving, 3.0M draws and 1.4M blocks,
  0 differ. `VCS_RSL_SKIP_VERIFY=1` with items: 1.04M skip decisions, 0 wrong. `VCS_RSL_WORLD_VERIFY=1` with items
  (each recording against the packet path): 375k, 0 differ. Reference scenes: static ones identical; tour subset
  classes unchanged (15 ok, 2 relocated, 1 falling).
- **Counts** (city spot (1325, -325), per 10 s): ~820k items made (74k world, 423k model, 327k model-b), 2k draws left
  to the packet path for their flush, 2k while caching; at the kick 60% skipped, 40% copied; all seen by the worker,
  none unplaced.
- **Measured** (A/B, same build, `VCS_RSL_ITEMS=0` vs on, three alternating rounds, `--opt none`, uncapped): driving
  110.5 -> 111.3 fps, the game thread's chain submission 1.12 -> 1.02 ms, render worker 4.38 -> 4.29 ms; standing within
  noise. The walker's share of the game thread falls (its copy, `skippable`) by about what the items cost to make
  (`itemDraw` ~0.5%): the copy savings were taken by 4b already. What 4c buys is that these draws no longer depend on
  the chain's bytes - the ground step 5 builds on.
- `VCS_RSL_ITEMS=0`: the draws' packets as before.

### 5. An empty chain

RSL_SUBMISSION.md phase 4. What is left in the VIF1 chain after steps 1-4 - 2D (Im2D: already drawn natively, still
sent), effects, the Z clear, texture uploads - moves to its call sites. Then a normal frame sends nothing through
VIF1 or the GS front end; the emulation stays as the switchable reference.

Done when: in gameplay, cutscenes and driving the frame's chain is empty (or not kicked), pictures as the packet
path's, tour and missions clean.

**What is left in the chain (2026-10-25, after step 4c).** `VCS_RSL_CENSUS=1` (top-level tags by DMA id, first
VIFcode and data region; items counted as one tag) and `VCS_TRACE_RSL=1` (bytes built per function per frame; it runs
with items off, so the draws' own tags are charged to the flush there):

| per frame | city spot (1325, -325), standing | AI drive |
|---|---|---|
| top-level tags / payload qwords | ~1,000-2,000 / 28-52k | ~1,000 / 29k |
| items (world / model draws) | ~280 tags | 120-320 |
| texture uploads: REF + DIRECTHL to raster data (model heap, cells) | ~310 tags, ~42k qwords | ~150, ~23k |
| state: CNT / REF + FLUSHE (UNPACKs of matrices, constants; context packets) | ~800 tags, ~4k qwords | ~400, ~2k |
| GS register packets: CNT + DIRECTHL | ~190 tags, ~3.2k qwords | ~110, ~2.8k |
| standalone MSCALs (setup programs), CALLs (context / cached packets) | ~100 / ~125 | ~45 / ~56 |
| 2D: CNT + DIRECT (Im2D) | ~54 tags, ~1.3k qwords | ~56, ~1.2k |

Who writes them (bytes per frame, standing / driving): Im2D `FUN_002FB690` 41 / 185 KB (103 / 797 calls); the GS
queue senders `FUN_002F7908` 25 / 21 KB and `FUN_002F7B30` 7.6 / 6.9 KB; texture binds `FUN_002F62B8` 19 / 44 KB and
`FUN_002F6A78` 3 / 7 KB (their uploads through `FUN_003298D8`); the texture matrix `FUN_002F7090` 5.5 / 21 KB;
`FUN_001CEF18` 2 / 16 KB; unidentified so far: `FUN_0040C430` (5 calls, 17 KB), `FUN_0032D480` (16 calls, 14 KB),
`FUN_0040F6C8` (1-20 calls, up to 6.6 KB), `FUN_00266868` (7 calls, 3 KB), `FUN_002F3DD0`, `FUN_002A92C0`; immediate
geometry `FUN_002F8250` (30 calls driving); the frame's camera packet (`FUN_002F53D8` / `FUN_002F9FA0`), VU1 code
uploads (MPG REFs into `.text`) and the root lists' fixed tags.

**The plan.** Each part the way step 4c went: a hook replaces the emitter when its effect can be stated exactly, the
effect travels as an item op in chain order, a verify mode lets the original write its packets and compares, and a
switch brings the packets back. Two rules learnt in 4c hold throughout:
- **Copied packets.** The game copies packet ranges and sends the copies again on later frames: `FUN_00240ED8` a
  sector's whole draw, `FUN_002409C8` each material's setup (texture bind + GS packet) - game-internals.md 6i. An
  item tag inside such a range would be sent again without its item. Every emitter below stays packets while a cache
  is being filled (`g_caching`, extended to `FUN_002409C8`'s material cache), or the cached block itself becomes a
  recorded block whose replay the worker owns.
- **Validity at the kick.** Anything an item takes from guest RAM by reference (a REF's payload, a raster, a CALLed
  packet) is decided after the write watch's fold and copied there if it changed - never read later on the worker.

Order (by what it removes, then risk):
- **5a - state ops** (M). Item ops without a block: VU memory writes (an UNPACK's result: address, fields, data -
  written as the recorder writes them), MSCAL of a setup program, GS register writes (A+D pairs, sent as the
  DIRECTHL they replace). Sources: the GS queue senders (`FUN_002F7908`, `FUN_002F7B30`), the setters (`FUN_002F7090`,
  `FUN_001CEF18`, `FUN_002F9FA0`, `FUN_002F9CE8`), the flush's CALL / REF cases (render-context packets, the table at
  `0x742AB0`, the instance matrix REF) as item blocks with the 4c rules, and the camera packet. Removes the CNT / REF
  FLUSHE, CNT DIRECTHL, CNT MSCAL and most CALL rows. Prerequisite for 5c (2D needs its GS state in order).
- **5b - textures** (L). The biggest volume: an upload becomes an item op "raster R (RAM address, psm, size, mips,
  CLUT) into VRAM slot (TBP, TBW)"; the raster's pages decide at the kick whether its bytes are copied (write watch,
  as blocks) or known; the worker applies it as the upload it replaces - VRAM written where something still reads VRAM
  (render targets sharing memory with textures: FBP 420's reflection map and bloom), else straight into the texture
  cache keyed by raster (step 3d's names), so an unchanged raster is neither copied nor swizzled nor decoded again.
  Checked against the VRAM path (`PS2X_HW_DECODE_VERIFY`-style, every texture a frame binds). This is also where
  `FUN_002F62B8`'s per-bind CLUT re-uploads stop costing (game-internals.md 6i "Texture streaming").
- **5c - 2D** (M). Im2D as items from `RwIm2DRenderPrimitive` (`FUN_002FBE08` / `FUN_002FBE68`) instead of
  `FUN_002FB690`'s CNT + DIRECT: the primitive's vertices (the 16-byte Im2D format) and the GS state from 5a; the HUD
  layer already draws them natively from the packets (NATIVE_RENDERER.md "HUD layer", "PATH2 packets recorded
  whole"), so this moves where the draw comes from, checked vertex by vertex against the packet path
  (`PS2X_HW_PACKED_VERIFY`'s comparison).
- **5d - immediate geometry and effects** (M-L, unknown until read). Identify `FUN_0040C430`, `FUN_0032D480`,
  `FUN_0040F6C8`, `FUN_00266868`, `FUN_002F3DD0`, `FUN_002A92C0`, `FUN_002F8250` (decompile: `tools/eedecomp.py` needs
  a JDK 25 for the installed Ghidra since its last update; the recompiled code's disassembly comments work meanwhile).
  Prebuilt blocks become item blocks (4c); per-frame vertices item data; their VU1 programs are native already.
- **5e - frame structure** (S-M). VU1 code uploads as "code image" ops (the native handlers check code by generation
  in VU1 code memory: the op keeps that memory as the upload would), the camera / root lists' fixed tags, the Z clear
  (the zclear program's strips as one native clear by target role), the three render-target reads (the reflection
  map's copies into FBP 420, the sky's self-copy) as native passes by role (step 3d).
- **5f - flushes with plug-in callbacks** (M). ~15% of draws while driving keep their packets because the flush calls
  guest plug-in code (skinned peds, vehicles). Read which plug-ins (render context list `+0x114`, game-internals.md
  6i) and port their callbacks' packet output natively, or let the callback write and take its bytes into the item
  at the next draw call (the 4c verify mode's way of reading what the original wrote).
- **5h - gaps** (M, added 2026-10-26). Whatever the remaining writers put between two hook points, taken as a captured
  region at the second one - no list of emitters to keep complete.
- **5g - no walk** (S). When a frame's chain holds only item tags and fixed tags, the kick hands the item list to the
  worker without walking or copying the chain (the write watch's fold stays for the blocks' validity); the walker and
  the VIF interpreter remain the reference path (`VCS_RSL_ITEMS=0`).

**5a done 2026-10-25 - state ops.** An item may hold VIF bytes alone (`hasBlock` false). The GS queue senders
(`FUN_002F7908`, `FUN_002F7B30`), the texture matrix (`FUN_002F7090`), `FUN_001CEF18`, `FUN_002F9CE8` and the native
state flush when it is all CNTs (a standalone flush, `FUN_002F7388`'s, or one inside a draw left to the packet path)
write no packets: their VIF bytes - exactly what the walker would make of the CNTs they replace - go into the open item
or a new one with its tag; ops in a row share an item while nothing else is written to the packet buffer between them
(and a draw right after them joins it). Their effects on guest memory (the GS queue, the dirty word, `FUN_001CEF18`'s
return value) are kept. Packets stay packets while a sector or a material is being cached (`FUN_002409C8`'s material
cache: from `FUN_00408F30` called at `0x240D14` to `FUN_002F7B30` called at `0x240D1C`, game-internals.md 6i).
- Checked: `VCS_RSL_ITEMS_VERIFY=1` now also compares every op's original packets with the op (driving: 2.67M draws
  and ops, 0 differ; 563k blocks, 0 differ). Reference scenes: `loading` identical, `pause_map` within the run-to-run
  noise it has between any two captures (1-2 levels on ~7-10k pixels); tour subset classes unchanged.
- Counts (city spot, per 10 s): ~450k ops making items of their own, ~52k merged into the item before them; no draw
  merged yet - a texture bind's upload packets sit between the setters and the draw (5b). Census at that spot, same
  window, ops off / on: top-level tags 1,055 -> 848 a frame; CNT FLUSHE (the GS senders' and setters' CNTs start with
  FLUSHE) 236 -> 59, CNT MSCAL 68 -> 46, items 158 -> 222. CNT DIRECTHL (~90) is not theirs and stays.
- Measured: A/B, same build, `VCS_RSL_OPS=0` vs on, three alternating rounds, `--opt none`, uncapped: driving 104.1 -> 113.9 fps (game thread 8.49 -> 7.72 ms, chain submission 1.15 -> 1.03, worker 4.64 -> 4.37); standing even (183 / 182 fps). The machine's run-to-run noise is about 5%: the drive's gain is consistent across every column but should be taken as 'up to ~9%'.
- `VCS_RSL_OPS=0`: the setters' packets as before (items for draws stay).

**5b done 2026-10-25 - texture uploads as reference ops.** `FUN_003298D8` (game-internals.md 6i "Texture uploads")
is replaced when the destination's transfer header already exists (its tree lookup, read-only, natively; a first
upload to a destination runs the original, which makes the node): its two REFs become **reference ops** - the two
VIFcodes and the payload's address and size - in the open item or a new one, whose tag goes into the slot the caller
reserved (the rest of the slot gets empty CNTs; the cursor is not moved). At the kick, after the fold, every item with
references has its VIF bytes rebuilt: its inline bytes and each payload copied from guest RAM where the reference
stands - exactly what the walker copies for a REF - with each raster upload's place noted, so textures keep their
names (step 3d) when the worker sends the pixels. A texture's mip levels share one item.
- **A bug on the way**: an open item of inline ops that then took references kept `refAt` 0, so its uploads copied
  the frame's first references - wrong pixels under the right names, texture-cache misses on every bind and the GS
  stage decoding all the time (driving fell to 36 fps). The tag verify could not see it (it checks what the game
  writes, not how items resolve). Fixed; each reference now carries its item and the kick checks it (counted:
  "references not their item's", 0).
- Checked: `VCS_RSL_ITEMS_VERIFY=1` (the tags the original writes for each upload against the two REFs the op stands
  for) driving 4.85M draws / ops, 0 differ; texture binds 99.998% found cached (as before); reference scenes as before
  (static identical, `pause_map` within its noise; moving scenes looked over - same textures); tour subset unchanged.
- Census at the city spot: chain payload ~22k -> ~7.9k qwords a frame (the pixels and headers are in the items).
  Left as packets: uploads while a sector or a material is being cached, and a destination's first upload.
- Measured: not settled. The cleanest A/B (uploads off / on, three alternating rounds, per run) gave driving 107.1 -> 103.4 fps and standing even; the kick then copied each payload twice (into a buffer, then into the arena) and now builds the item's bytes in place - not re-measured, because the machine was under changing background load for the rest of the session (runs with uploads off fell to 66-74 fps as well). In one hour of runs ~half of the runs with uploads on had windows of 40-100 ms frames (game thread and render worker waiting together; the GS stage not saturated, no texture misses, no drains, no stale items); in the 27 runs after it none did, with or without instruments - most likely the same background load, not proven. To measure on a quiet machine: alternating runs of the same build (stand, then the bench drive), `VCS_RSL_UPLOADS=0` vs on. `VCS_RSL_SLOW_LOG=1` logs kicks whose resolve takes > 3 ms and chains the worker takes > 15 ms on; `PS2X_RENDER_THREAD_LOG=1` now also counts GS stage drains.
- `VCS_RSL_UPLOADS=0`: the uploads' REFs as before.

**5c done 2026-10-25 - Im2D as ops.** Not a rewrite of `FUN_002FB690` (game-internals.md 6i "Im2D's packet writer"):
the original writes its CNT + DIRECT as before, and at the next of our hook points - every hooked RSL writer, the
texture binds that reserve upload slots, the state flush, the frame kick - the packet becomes an op when the cursor
moved by exactly its length since the call (nothing else written in between): its VIF bytes into the open item (it
usually follows the GS registers' op) or a new one, and the cursor goes back to the packet's start. Its only caller
never reads the packet again; `VCS_RSL_ITEMS_VERIFY=1` checks that every captured packet still holds the same bytes at
the kick.
- Checked: driving, 803k packets captured, 0 changed afterwards; 1.52M draws / ops 0 differ (the other checks);
  reference scenes - `loading` and `pause_map` (drawn with Im2D) identical, the HUD scenes as before; tour subset
  unchanged.
- Counts (driving, per 10 s): ~770k Im2D packets as ops, none kept as packets. The census's CNT DIRECT row (~55 tags
  a frame) is gone.
- Measured: A/B `VCS_RSL_IM2D=0` vs on, three alternating rounds under varying background load: driving 95.2 -> 99.3 fps (chain submission 1.64 -> 1.18 ms, worker 6.04 -> 4.97), standing 172 / 171 fps; in the one quiet pair of runs both read the same (submission ~1.1 ms driving). No regression; the gain, if any, is within this machine's noise.
- `VCS_RSL_IM2D=0`: the packets as written.

**5d done 2026-10-25 - the other writers, captured.** Step 5c's capture generalized to every packet writer the census
and the trace still showed (game-internals.md 6i "The other packet writers"): Im3D's submit `FUN_002F8250`,
`FUN_0040C430`, `FUN_0040F6C8`, `FUN_00266868`, `FUN_002F3DD0`, `FUN_002A92C0`, `FUN_002F5520`, `FUN_002ABF68`,
`FUN_0032D480`, `FUN_0032D7C8`, `FUN_002F0360`, `FUN_002F39D8`, `FUN_0032CD60`, `FUN_0032D880`, `FUN_002F9FA0` and the
camera packet `FUN_002F53D8`. None is rewritten:
- An emitter's entry notes the cursor and the **guest stack pointer**. A hook deeper on the stack runs inside the
  emitter (several call the GS register sender or the flush): it neither settles the capture nor makes ops of its own
  - its packets become part of the emitter's region. The capture settles at the next hook point on the emitter's
  level or above, or at the kick.
- A region of CNTs and REFs becomes an op: CNTs inline, REFs as reference ops (step 5b). When it ends in something
  else - packets other code wrote after the emitter returned (a CALL from the game's own flush, the camera packet's
  CALL) - **its front** becomes the op: the region's start gets the item's tag and a NEXT to the first packet that
  stays, the cursor untouched.
- Checked: `VCS_RSL_ITEMS_VERIFY=1` driving - 766k captured regions unchanged at the kick, the other checks 0 differ;
  reference scenes as before (static identical); tour subset unchanged.
- Counts (driving, per 10 s): Im2D 708k whole; `FUN_0040F6C8` 7k whole + 11k front; `FUN_002F3DD0` 4.7k and
  `FUN_002A92C0` 3.7k front; the frame-level writers once a frame each. Kept: Im3D (`FUN_002F8250`, 24k) and
  `FUN_002F9FA0` - both start with the flush, whose CALLs to render-context packets stay in the chain (5f). Census
  while driving: ~1,090 -> ~740 top-level tags a frame.
- Measured: A/B with all captures off (`VCS_RSL_IM2D=0`) vs on (steps 5c + 5d), three alternating rounds on a quiet machine: driving 99.2 -> 101.3 fps (game thread 8.91 -> 8.69 ms, worker 4.76 -> 5.13), standing 179.2 -> 165.3 fps (game thread 4.85 -> 5.44 ms). The standing difference did not hold up: a standing-only A/B (none / Im2D only / all, three rounds each) gave 4.58 / 4.28 / 4.65 ms with one setting ranging 3.82-5.43 ms between runs (what is spawned at Fort Baxter varies), and no capture code shows in the profile. Read as even within this noise; a longer standing measurement would settle it.
- `VCS_RSL_IM2D=0`: no captures (Im2D and these writers write their packets as before).

**5e in part, 2026-10-25 - VU1 code uploads and the double buffer in recordings.** The frame's structure, dumped
(game-internals.md 6i "The frame's fixed structure"): the static root list, the render-context packets the flush
CALLs - BASE / OFFSET, MPG REFs re-sending the draw programs' microcode every frame, the context's matrices and an
MSCAL - and the camera packet. Those context packets were the chain's main "other call" blocks, and none could be
replayed: the recorder refused BASE / OFFSET / ITOP and MPG. Now the recorder takes them as **raw ops** - the VIFcode
(and for MPG the microcode, through a new `VifRecorder::mpg` callback of the runtime) - and a replay sends those bytes
through the VIF interpreter itself, in order: TOPS / DBF, ITOPS and the VU1 code memory (with its decode cache's
invalidation, only when the code differs) end as the packet path leaves them. So VU1 code uploads are "code image" ops
of the recordings, and the context packets are replayed - and, unchanged, skipped (step 4b) - like any other block.
- Checked: `VCS_RSL_WORLD_VERIFY=1` (each block recorded again by the packet path and compared op by op, raw ops
  included) at the city spot, 1.79M recordings, 0 differ; reference scenes as before; tour subset unchanged.
- Counts (city spot, per 10 s): "other call" blocks 0 not replayable (15.6k before), ~437k replayed, ~400 recorded
  (100k before - the refused ones were recorded again and again).
- Measured: A/B `VCS_RSL_RAW=0` vs on, three alternating rounds on a quiet machine (`--opt none`, uncapped): standing 195.3 -> 208.1 fps (game thread 4.54 -> 4.16 ms, render worker 2.13 -> 2.01), driving 110.1 -> 115.0 fps (game thread 8.11 -> 7.70 ms). The game thread gains as well: an unchanged context packet is now skipped at the kick instead of copied.
- `VCS_RSL_RAW=0`: these blocks refused as before.
- **Left in 5e**: the root list itself (static tags in `.data`, walked every kick), and the three render-target reads
  (the reflection map's two copies into FBP 420, the sky's self-copy - the only GS-path draws left, `gs_draws` 3). The Z
  clear needs nothing more: the zclear program runs natively.
- **The render-target reads, looked into (2026-10-25, not done).** They already run on the GPU (each samples target
  140 directly, `tex=rt`); what is not native is their way in - the GS front end, register by register. Their packets
  are not where step 1 placed them: `PS2X_HW_PACKED_LOG=<n>` (new: the PATH2 packets `submitPackedKick` refuses, by GIF
  tags; A+D packets only when they carry a vertex) shows, refused in gameplay, only A+D state packets, image transfers
  and PACKED sprites (PRIM 0x116 - textured, UV - with RGBAQ / UV / XYZ2 / UV / XYZ2, and an untextured RGBAQ / XYZ2 /
  XYZ2 one); no A+D packet with vertices. Taking those sprites as whole packets (through the front end's own
  `writeRegisterPacked`) moved neither `gs_draws` nor the packed draws at the city spot, so they are not these three
  draws; that change was taken out again. Next: find their route from the draw itself (a stack at the backend when a
  GS-path draw samples target 140), then record that route whole - or, as planned, draw the copies as passes by target
  role. No frame-rate gain to expect: three draws a frame.
- **The render-target reads, done (2026-10-26).** A log at the front end's own draw (`PS2X_HW_FRONT_DRAW_LOG=<n>`: each
  draw it makes from GS primitives with the GIF packet it came from, PATH1 aside) found them: A+D packets after all -
  vertex runs (UV / RGBAQ / XYZ2) with **no PRIM of their own** (PRIM sent with the state in an earlier packet), and
  one that writes XYOFFSET_2 / TEST_2 / SCISSOR_2 **before** its PRIM (game-internals.md 6f "The render-target reads'
  packets"). `submitPackedKick` took A+D runs only when they began with PRIM. Now it also takes a run that continues
  the current PRIM (the vertices already in the GS vertex queue go first, without a kick, as the front end has them)
  and a packet whose leading writes are pure state registers (checked over the whole packet first, then written
  through the normal register path; TEX0 / TEX2 excluded - their writes may load a CLUT). One more PATH2 draw in the
  flyover was an Im2D **line strip**: the packed path and `recordPacked` take lines too (two vertices, as `Submit`).
  (The earlier `PS2X_HW_PACKED_LOG` run missed these packets: its first n were spent before gameplay.)
  - Checked: `PS2X_HW_PACKED_VERIFY=1` (each packet also drawn one `Submit` per primitive, every vertex and state
    compared) standing 1.88M packets and the reference scenes 5.6M, all exact.
  - `gs_draws` 0.0 in the reference scenes (gameplay, cutscene, flyover) and the tour subset (mean 0.0, max 0.0):
    **no draw goes through the GS front end in a normal frame**; pictures as before. The front end's register path
    still applies the packets' state writes (A+D state packets, the leading writes above), as the native draws need
    them; that is register state, not drawing.
  - `PS2X_HW_PATH2_CONTINUED=0`, `PS2X_HW_PATH2_LEAD=0`, `PS2X_HW_PATH2_LINES=0`: each kind through the front end.

**5f in part, 2026-10-25 - the plug-in callbacks.** The draws left to the packet path were those whose state flush
calls plug-in code. Logged (`VCS_RSL_PLUGIN_LOG=1`, game-internals.md 6i "The plug-in callbacks"): only `+0x50`
callbacks, four functions, two of them thin wrappers around writers already known - `FUN_001CEF18` (3 qwords) and
`FUN_0040F6C8` (9 qwords, behind a flag it clears). The native flush now writes those two in the original's place
(after the context CALLs, in the list's order) and clears the flag as the original does; a draw whose flush has only
these becomes an item like any other.
- Checked: `VCS_RSL_NATIVE_VERIFY=1` driving, 1.62M flushes the same as the original's bytes, 0 differ;
  `VCS_RSL_ITEMS_VERIFY=1` 1.68M draws / ops, 0 differ; reference scenes as before; tour subset unchanged.
- Counts (driving): the flushes with the two callbacks now written natively were ~50k per 5 s (the log, first callback
  per flush); left to the original now, per 10 s, ~36k of ~870k: ~15k for `FUN_001CFAE0` (bit 0x4, float maths) and
  ~21k with the other two callbacks (`0x2A9538`, `0x2F4080`), whose packets 5d's captures already take in part.
- Measured: A/B `VCS_RSL_PLUGINS=0` vs on, three alternating rounds on a quiet machine: standing 205.1 -> 208.4 fps (game thread 4.27 -> 4.17 ms, render worker 2.17 -> 1.88), driving 117.5 -> 120.5 fps (game thread 7.58 -> 7.33 ms).
- `VCS_RSL_PLUGINS=0`: those flushes left to the original as before.
- Left then: `FUN_001CFAE0` (bit 0x4, float maths), the two remaining callbacks, and Im3D (`FUN_002F8250`, kept by
  5d's capture: its region starts with the flush's context CALLs).

**5f done, 2026-10-25 - captured regions take CALLs; draws left to the original are captured.** Rather than port the
rest of what a flush may call (FUN_001CFAE0 and two geometry callbacks, each to be checked bit for bit), the draws
whose flush calls them now run the original and are **captured** like the 5d emitters: the region the draw wrote -
the flush's packets, whatever its callbacks wrote, the draw's own REF / CALL - becomes items at the next hook point.
- **CALLs as blocks.** A captured region is cut at each CALL into pieces: a piece's CNTs and REFs as before, then the
  CALL's own bytes and the CALLed packet as the piece's block (copied at the kick by `flattenBlock`, or skipped and
  replayed when unchanged - exactly as a model item's block). Only targets the walker has already copied as
  `flattenBlock` copies them (`g_flatOk`, now kept for "other calls" too) are taken; at any other CALL the region's
  convertible front is taken and the rest stays packets. The first piece's item carries the region's tag (or joins the
  open item); the others are **continuation items** (`Item::continued`) - no tag of their own, the worker runs them
  right after the item before. Im3D's regions (context CALLs, then its geometry) become ops this way.
- **Draws in regions.** Every draw made during a capture - the captured draw itself, or draws an emitter makes inside
  it (game-internals.md 6i "Emitters draw models") - has its tag in the region, in order: a world draw's REF becomes
  a world block, a model draw's CALL a model block; the draw leaves this frame's draw list when its tag is taken, so
  the walker's in-order matching stays in step.
- **Inside a draw** is decided by stack depth *and* return address (game-internals.md 6i "Inside a draw, by return
  address"); stack depth alone took the caller's next draws in too.
- Checked: `VCS_RSL_ITEMS_VERIFY=1` driving - 1.51M captured regions unchanged at the kick, 0 changed; 3.38M draws /
  ops, 0 differ; 640k blocks (other calls' now included) copied as the walker copies them, 0 differ.
  `VCS_RSL_WORLD_VERIFY=1` driving, 1.54M recordings, 0 differ, no stray item tags. Reference scenes: static ones as
  before (the loading bar's edge a few pixels further), moving ones within their usual spread; tour subset unchanged
  (15 ok, 2 relocated, 1 falling, as before).
- Counts (driving, per 10 s): Im3D 16.4k regions as ops (all kept before); the 18.6k draws left to the original all
  captured whole; model items 90k (31k before - the draw-list matching was out of step before, see game-internals);
  model draws not known 4 (77k). Kept per frame: the camera packet's END. Census: 742 top-level tags a chain, 780
  payload qwords - 288 of them item tags, 278 empty CNTs (filler in slots the game reserved).
- Measured: A/B against step 5f's first part, five alternating rounds (60 fps cap, `--opt none`; two rounds had load spikes on one side, so per-round medians): game thread even - standing 4.33 -> 4.32 ms, driving 8.68 -> 8.50 ms; render worker while driving 6.0 -> 5.0 ms, lower in every round (Im3D and the captured draws reach it as items instead of packets). A first version that matched draw tags only for the captured draw itself was 0.5-0.9 ms slower standing: the walker's matching out of step, no model items at all there.
- `VCS_RSL_DRAW_CAPTURE=0`: draws left to the original keep their packets. `VCS_RSL_CAPTURE_CALLS=0`: other CALLs end
  a region (as before). `VCS_RSL_KEPT_LOG=1`: where each region not taken whole stops (first 4000).
- Not ported, and no need now: `FUN_001CFAE0` and the two geometry callbacks run recompiled inside captured draws;
  their output reaches the chain as items. A native port would only save their CPU time (small).

**5g, measured: not needed (2026-10-26).** `VCS_RSL_WORLD_LOG=1` now times the kick (first tag to hand-over): 0.23 ms
a chain driving / standing, of which 0.19 ms is the items' resolve (block copies decided after the write watch's fold
- a kick without a walk keeps it). The walk and copy of the ~300 remaining tags cost ~0.04 ms a frame; a second way
to start a frame is not worth that (DECISIONS.md 2026-10-26).

**5h done, 2026-10-26 - gaps: everything between two hook points.** A census by origin (`VCS_RSL_CENSUS=2`: every hook
point notes the cursor, each tag left in the chain is named after the last hook point before it) showed what was left
after 5f, driving, per frame: ~250 empty CNTs (upload slots whose REFs became ops), ~80 texture-bind register packets
(game-internals.md 6i "The texture binds' own packet"), ~50 tags around the sector caches, ~20 others. Rather than
one capture per writer:
- **Gaps.** At every hook point, the bytes written since the last one (by any code) are parsed as a captured region
  and taken into items - not while a sector / material cache is being filled, not from a cursor outside this frame's
  half. A gap is taken **in place**: its start gets the item's tag and a NEXT to its end, the cursor never moves back
  (a caller may hold it: game-internals.md 6i "Callers hold the cursor across calls" - the first version rewound and
  lost the frame's alpha, effects, HUD and post passes).
- **Runs.** Any region (emitter, draw, gap) is now taken in runs: a tag the walker must still read (a CALL of a packet
  not known yet, a REF outside main RAM, an item's own tag) stays, the parse goes on after it, and each convertible run
  becomes items in place. Before, the first such tag kept the rest of the region. Draws whose tag stays keep their
  draw-list entry; the others leave it one by one.
- **The texture binds are captures** (`FUN_002F62B8`, `FUN_002F6A78`): their reserved upload slots are never read half
  filled; inside a bind is decided by return address (its own code, `FUN_003298D8`), as for draws - by stack depth
  alone a bind swallowed its caller's next calls (Im2D among them).
- **Draws' kept flush tags** (a flush with an instance-matrix REF or context CALLs, written before the item's tag) go
  to the next gap.
- Two fixes found on the way: the frame kick settled the last capture after the draw list was handed over (a region
  taken there could not drop its draws); the material cache's window ended before its last packet was copied
  (game-internals.md 6i "The material cache's last packet").
- Checked: `VCS_RSL_ITEMS_VERIFY=1` driving - 2.15M captured regions unchanged at the kick, 0 changed, and 0 tags
  reading inside a taken region (new: the walker checks every REF / CALL / NEXT target against the frame's taken
  regions); 3.34M draws / ops and 631k blocks, 0 differ. `VCS_RSL_WORLD_VERIFY=1`, 1.40M recordings, 0 differ. Scenes as
  step 5f's (static ones identical but for the loading bar's edge), tour subset unchanged.
- Counts (driving, per frame): 303 top-level tags (742 after 5f), 245 of them item tags and ~32 the in-place runs'
  NEXTs; ~16 the root lists' fixed tags in `.data`; 2-3 packets left in the buffer (the material cache's GS packet,
  sent at the top level as well as copied, and an upload header now and then). Payload 232 qwords (780), mostly the
  root lists'.
- Measured: A/B against 5f, three alternating rounds on a quiet machine (60 fps cap): even - game thread standing
  4.21 -> 4.06 ms, driving 6.34 -> 6.42 ms; render worker 2.03 -> 1.97 / 4.58 -> 4.59 ms. The gain of an all-items chain
  is 5g's (no walk).
- `VCS_RSL_GAPS=0`: no gaps (the binds stay captures); `VCS_RSL_RUNS=0`: a region stops at its first kept tag (as
  before). Debug: `VCS_RSL_GAP_LOG=1` (frame 400's gaps), `VCS_RSL_GAP_FNS=,<hex>,...` (gaps only at those hook points;
  0 = the kick), `VCS_RSL_CENSUS=2`.

Expected gains, honestly: the game thread's share of handing frames over (the walk, copy, fold and bookkeeping:
~5% while driving) shrinks with every part; the render worker gains where an op replaces VIF parsing and VU-memory
round trips (5a, 5b), and most from 5b (texture bytes). Each part is measured A/B as 4c was.

## Not "native" but part of "done"

Completeness (milestone 2: every mission to the credits), the open fall-through spot (-925, 1175), the stale
regression baseline for the loading / pause-map scenes (needs the user's picture sign-off), 60 fps fixes (milestone
4), game code under ~6 ms (milestone 7), settings / controls polish (8), Vulkan / Metal and Linux / macOS (9).

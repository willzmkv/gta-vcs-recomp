# Native renderer plan (D3D11)

Status: **approved** (2026-10-22). Stages 0-6 done (world, skinned and rigid lit models native); stage 7 left
deliberately on the VU1 (small: see there); stage 8 partly (internal resolution works with native draws and can be
set in vcs.ini; widescreen, MSAA and anisotropic filtering are not done - reasons in stage 8).

Overall, VU path (stage 3) -> native (stage 6), reference script, i7-7700K / GTX 1080 Ti:

| phase | game fps | render submit ms/frame |
|---|---|---|
| flyover 10-44 s | 18.2 -> 28.3 | 36.7 -> 12.1 |
| cutscene 55-86 s | 18.8 -> 30.0 (cap) | 38.1 -> 11.4 |
| gameplay 100-125 s | 29.9 -> 29.9 (cap) | 10.6 -> 5.3 |

Goal: get from today's emulated GS (raylib + OpenGL 3.3) to a native renderer that
draws the game's models directly, on D3D11, as fast as possible. Windows only for
now, but nothing Windows- or D3D-specific may leak outside two small directories so
macOS (Metal) and Linux (Vulkan) can be added later.

Non-goals for this plan: macOS/Linux backends, movies, a new frontend/UI. (Linux got its Vulkan backend later, below:
"Vulkan backend"; docs/LINUX.md.)

## Why (where the frame goes today)

Title flyover, i7-7700K, `PS2X_GS=hw` (see PROGRESS.md, performance table):

| part | ms / frame |
|---|---|
| render submit: VIF1 unpack, compiled VU1, GS recording | ~34 |
| game code (EE) | ~15 |
| OpenGL execution (main thread) | ~2.5 |

- Swapping OpenGL for D3D11 alone moves the 2.5 ms, not the 34 ms. It is worth doing
  (tooling, driver stability, presentation control, a base for the native path), but
  it is not the speed-up.
- The speed-up is skipping VIF/VU1/GS for model draws: decode the game's prebuilt
  model packets once into GPU buffers and draw them natively (stages 4+).
- The emulated GS path stays forever as the fallback for anything not converted
  (HUD, post effects, loading screens) and as the reference to compare against.

## Target architecture

```
 runtime (third_party/PS2Recomp/ps2xRuntime)       game-specific (src/game)
   EE / VIF / VU1 / GS frontend  ──────────────┐     HLE packet handlers
                                               ▼            │
 src/render      GS HW backend: recorder (game thread) ◄────┘ native draw cmds
                 executor (main thread, API-agnostic, talks only to rhi::)
                                               │
 src/rhi         rhi.h (interface)  ── rhi_d3d11/ (only place with d3d11.h)
                                               │
 src/platform    platform.h (window, events, input, audio, time)
                 platform_sdl3/ (SDL3; only place with SDL / windows.h)
```

Rules:
- `windows.h`, `d3d11.h`, `dxgi*.h` only under `src/rhi/rhi_d3d11/` and
  `src/platform/`. SDL headers only under `src/platform/`. Everything else includes
  `rhi.h` / `platform.h`. (This also ends the raylib/`windows.h` name clashes.)
- Shaders are written in HLSL (SM 5.0), compiled at build time to bytecode headers
  (`fxc`/`dxc`). Later ports cross-compile the same source (Slang or
  DXC -> SPIR-V -> SPIRV-Cross for MSL); keep shaders free of D3D-only intrinsics
  where a portable form exists.
- The runtime reaches the platform and RHI through the host-backend seam it already
  has (`ps2_host_backend.h`); raylib is removed from it. Patches go in
  PS2RECOMP_PATCHES.md as usual. (Upstream's Vita build stops building; we don't
  ship it.)

### Platform layer: SDL3

- Window (resizable, fullscreen, hidden for `--hidden` runs), event pump, keyboard,
  mouse, gamepads (SDL's controller database, better than raylib's), audio stream,
  high-resolution timer.
- Dear ImGui through `imgui_impl_sdl3` + the RHI's ImGui renderer (`imgui_impl_dx11`
  on Windows). The F3 perf overlay moves from raylib `DrawText` to ImGui.
- Not SDL3's GPU API: it has no D3D11 backend.

### RHI: our own, thin

Own interface rather than bgfx / sokol_gfx / Diligent: GS emulation needs
render-target aliasing, sub-rectangle copies between targets, and texture readbacks,
which those libraries handle poorly or not at all. The interface stays small
(a few hundred lines) and shaped so it maps to Metal and Vulkan later:

- Resources: buffers (vertex, index, constant; immutable / dynamic per frame),
  2D textures (sampled, render target, depth), samplers.
- Pipelines: immutable objects combining shaders + blend + depth/stencil + raster
  state, created from a hashable key and cached (D3D11 state objects underneath).
  GS state (blend ABCD/FIX, ZTST, ATST, colour mask...) maps to a key.
- Explicit render passes: begin(colour target, depth target, load/clear ops),
  draws, end. D3D11 doesn't need them, Metal/Vulkan do; designing them in now is free.
- Copies: texture region -> texture region, buffer uploads, texture readback
  (staging, async with a fence/query, sync variant for screenshots and dumps).
- Swapchain: DXGI flip model (`FLIP_DISCARD`), vsync on/off, tearing when allowed,
  resize, exclusive-less fullscreen. Frame latency waitable object.
- Debug: D3D11 debug layer behind an env var, object names, PIX/RenderDoc markers.
- Features we rely on, checked against D3D11 FL 11_0: dual-source blending
  (the HW backend uses `SRC1_ALPHA`), 32F depth, R8G8B8A8 targets, point/line lists
  (GS points/lines are 1 px, no point size needed).

## Stages

Each stage ends with the game fully playable and a commit; nothing is a big-bang
switch. Sizes are relative (S < M < L < XL).

### Stage 0 - Baselines (S) - done 2026-10-22

- `python tools/scenes.py capture OUT` runs the game from an empty memory card
  (`VCS_DATA_DIR`) along a fixed script driven over the control channel: loading
  screen, three flyover shots, intro cutscene, gameplay with HUD after skipping the
  cutscene (Cross), the pause map, gameplay again. It saves screenshots, the perf
  overlay numbers per phase and a whole-run perf CSV (`PS2X_PERF_LOG`).
- `python tools/scenes.py compare REF NEW` reports per-scene mean difference and
  writes diff images. Static scenes (loading, pause map) must match (tolerance 2.0);
  two runs of the same build match exactly (0.00). Moving scenes drift with timing
  between runs (up to ~15 mean on the flyover road shot) and only fail when black
  or missing.
- `python tools/scenes.py explore OUT --every N --press T:KEY,...` was used to find
  the timeline (screenshots every N s plus a contact sheet).
- References live in `build/scenes/` (game imagery, never committed). Baseline
  before stage 1: `build/scenes/base_raylib_a` (raylib + GL, `PS2X_GS=hw`).
  Flyover 10-44 s: 17.9 game fps, render submit 37.5 ms, game 14.5 ms, GL execute
  3.2 ms per frame, host 59.6 fps; gameplay: ~29-30 game fps, render ~16.5 ms.
- `PS2X_HW_DUMP` render-target dumps were left out of the script: the frame number
  they key on drifts between runs; use them by hand when a scene differs.

### Stage 1 - SDL3 platform layer, raylib removed, still OpenGL (M) - done 2026-10-22

- SDL3 window with a GL 3.3 context; the HW backend's GL executor keeps working
  unchanged. The final present becomes a small GL blit (temporary, deleted in stage 3).
- Port the raylib call sites: `Kernel/Stubs/Pad.cpp` (input, ~46 calls),
  `ps2_runtime.cpp` (window, loop, present, ~20), `ps2_perf.cpp` (overlay, ~12),
  `ps2_audio_out.cpp` (audio stream), `ps2_debug_panel.cpp` (rlImGui ->
  imgui_impl_sdl3/opengl3), `ps2_control.cpp` (key names, screenshot).
- Control channel keeps working: screenshots, injected keys, `--hidden`.
- paraLLEl-GS (`PS2X_GS=gpu`): keep the readback path only; drop its Vulkan
  window present (`gs_vulkan_present`), which is built on raylib's GLFW window.
- Done when: raylib and rlImGui are gone from the build, stage 0 captures match,
  audio/input/saves behave as before.

Result:
- `src/platform/`: `platform.h` (window, events, keyboard / mouse / gamepad snapshot
  readable from any thread, audio pull callback, 60/s frame pacing, Dear ImGui frame,
  PNG writer), `platform_sdl3.cpp` (SDL 3.4.18, static, FetchContent),
  `gl_present.{h,cpp}` (the stage 1 GL stopgap: letterboxed frame quad, textures,
  off-screen capture). The runtime gets it through `PS2X_HOST_PLATFORM_TARGET`.
- Scene captures vs the raylib baseline: static scenes 0.03 (loading) and 0.17
  (pause map: the REC marker's font); moving scenes show the same pictures a second
  or two apart. Flyover 10-44 s: 17.9 game fps, render 37.4 ms, GL 3.2 ms, host
  59.9 fps - unchanged. Audio: 0-2 underruns per run, as before.
- Behaviour changes: Escape no longer closes the window (raylib's default exit key);
  L2 / R2 on a gamepad trigger at 30% travel (raylib needed ~65%); screenshots scale
  the overlay instead of laying it out again; window size still 640x448, screenshots
  at least 960x672 (bigger windows give bigger, letterboxed shots).
- The paraLLEl-GS Vulkan window present (`gs_vulkan_present.h`) was never
  implemented in this tree, so nothing had to be dropped: `PS2X_GS=gpu` reads frames
  back as before (still not to be run on the GTX 1080 Ti, TDR).

### Stage 2 - RHI + D3D11 device and presentation (M) - done 2026-10-22

- `rhi.h` + `rhi_d3d11`, swapchain and present on the SDL window (no GL context).
- The CPU renderer and paraLLEl-GS readback frames are shown by uploading into an
  RHI texture; ImGui draws through `imgui_impl_dx11`.
- `PS2X_GS=hw` (still GL) temporarily presents by reading back into the D3D11 path,
  or stays on a GL window for this stage only - whichever is less throwaway.
- Done when: CPU renderer, perf overlay, debug panel and screenshots work on D3D11.

Result:
- `src/rhi/rhi.h` (textures, buffers, samplers, immutable pipelines with blend /
  depth / dual-source factors, explicit passes, copies, readback, swapchain, the
  Dear ImGui renderer hook) and `src/rhi/d3d11/rhi_d3d11.cpp` (FL 11_0, flip-model
  `FLIP_DISCARD` swapchain, `PS2X_D3D_DEBUG=1` for the debug layer, PIX markers via
  `ID3DUserDefinedAnnotation`). Only that file includes D3D / DXGI headers.
- `src/render/presenter.{h,cpp}`: letterboxed picture + Dear ImGui into the back
  buffer, off-screen captures; `shaders/present.hlsl` compiled by fxc at build time
  (`src/render/CMakeLists.txt`, `vcs_shader()`). The platform window has no GL any
  more; ImGui uses `imgui_impl_sdl3` (input) + `imgui_impl_dx11` (through the RHI).
- Chosen stopgap for the still-OpenGL HW renderer: a hidden GL 3.3 context
  (`platform::glInit`, only with `PS2X_GS=hw`) and `src/render/gl_bridge.cpp`, which
  reads the display texture back through two pixel-pack buffers (shown one host
  frame later) and uploads it. A synchronous `glReadPixels` first cost ~4% game fps
  on the flyover; the asynchronous one is within run-to-run noise.
- Scene captures: static 0.02 / 0.02 vs the raylib baseline; flyover 17.2-18.0 game
  fps (baseline 17.9-18.2; runs vary by ~1 fps), main-thread GL time 3.7 ms (3.2
  before: the readback). Resizing, minimise / restore, hidden runs, the F3 overlay
  and `PS2X_GS=cpu` checked. `PS2X_GS=gpu` not run (TDR on the dev GPU).
- Display latency with `PS2X_GS=hw` is one host frame (16.7 ms) higher until stage 3.

### Stage 3 - GS HW backend executor on the RHI (M) - done 2026-10-22

The backend is already split: the game thread records API-neutral frame command
lists (`Frame` in `gs_hw.cpp`: draws with `HwState`, texture creates/deletes, RT
uploads/drops, clears, depth fills), and only the main-thread executor touches GL.

- Move the recorder to `src/render/` (or keep it in place, patched) unchanged; write
  an RHI executor next to the GL one and switch with an env var during the port.
- Port the GLSL shaders to HLSL (vertex: PS2 pixel coords -> clip; pixel: TFX
  modulate/decal/highlight, TCC, alpha test via `discard`, fog, colour clamp,
  `fbmsk`; blit for display and RT copies).
- Mapping notes: `glBlitFramebuffer` -> `CopySubresourceRegion` or shader blit;
  `glGetTexImage` -> staging readback; texture origin is top-left in D3D (the
  presenter contract is already "row 0 at the top"); GL's -1..1 clip depth vs
  D3D's 0..1 (vertices already carry 0..1 depth: check the projection);
  pixel centres are at .5 in both.
- `PS2X_HW_SCALE` upscaling and `PS2X_HW_DUMP` keep working.
- Done when: captures match the GL executor within tolerance on all reference
  scenes, then the GL executor and the stage 1 GL blit are deleted and `hw`
  (D3D11) becomes the runtime default.

Result:
- `RhiExecutor` in `gs_hw.cpp` runs the recorded command lists on the window's
  device; shaders `src/render/shaders/gs_draw.hlsl` (draw, with a dual-source
  variant) and `gs_util.hlsl` (blits, fills), exposed by `render/gs_shaders.h`.
  The recorder is unchanged except one line of behaviour (flat colours, below).
- D3D11 differences handled:
  - Y: render targets keep PS2 rows top-down; the vertex shader flips into NDC;
    scissor and copy coordinates stay as they were.
  - Flat shading: GL used the last vertex, D3D's `nointerpolation` the first; the
    recorder now writes the last vertex's colour to every vertex of a flat
    primitive (the GS rule), so the shader doesn't care.
  - D3D binds a depth buffer only with an equally sized colour target: when they
    differ, the smaller one grows (colour keeps its contents; depth is recreated
    cleared, as the GL version did when it grew).
  - Partial colour clears and the depth fills (VCS's Z clear through a colour
    draw) are scissored fill draws; depth fills use a depth-only pass.
  - A draw that samples its own render target reads a copy (D3D unbinds a bound
    target from the shader instead of reading it). VCS does this.
  - The displayed picture is drawn with alpha 1 by the blit shader (GL masked a
    clear).
- Parity (`PS2X_HW_PARITY=1`, kept for one commit, 2e85a69): the old GL executor ran
  the same frames in a hidden GL context and the displays were compared every 30
  host frames over the reference script: 80 of 116 identical, worst mean difference
  0.43/255, differences only on triangle edges (top-left fill rule now in PS2
  orientation, GL's was mirrored) and one horizon row.
- Reference scenes: static 0.02 / 0.17 vs the raylib baseline. Flyover 10-44 s:
  17.7-18.6 game fps over four runs (baseline 17.9-18.2); main-thread GPU submit
  0.4 ms (GL execution was 3.2 ms; 3.7-3.9 with the stage 2 readback). The extra
  frame of display latency from stage 2 is gone.
- Checked: `PS2X_HW_SCALE=2`, `PS2X_HW_DUMP` / F12 (render targets and display as
  PPM), hidden runs, resize. vcs.exe no longer imports opengl32.dll.

At this point the game runs on D3D11 with no OpenGL or raylib left. Frame rate is
about the same as before (the game thread's render submit is still the cost).

### Stage 4 - Packet-level HLE groundwork (L) - done 2026-10-22

Where to hook: the VIF1 DMA chain, not the recompiled game functions. Per
game-internals.md, each frame's chain CALLs prebuilt model packets in the model heap
(0x1200000-0x17FFFFF), with per-instance data (matrices, lighting) uploaded
before each CALL and VU1 programs uploaded by REF+MPG. Game code stays untouched;
recognised packets are drawn natively, everything else falls through.

1. **Survey tool**: per frame, for each CALL target record the VU1 program it runs
   (MSCAL address + which upload is resident), the VIF unpack layout (formats,
   addresses, counts), the preceding uploads, and the GS state it ends up with.
   Output a coverage report: share of vertices / GS time per (program, layout).
   This decides the order of stages 5-7 and documents each program in game-internals.md.
2. **Dispatcher**: in the DMA/VIF1 path, at a CALL into a packet whose program has
   a registered handler (`src/game/`, registered like the native VU0 programs),
   the handler consumes the packet and emits a native draw; VIF/VU1/GS work for it
   is skipped. `PS2X_HLE=off` / per-program allow lists for A/B comparison.
3. **Native draws in the same frame**: a new command type in the HW backend's frame
   list, so native and emulated draws keep their order and share the same render
   targets, depth buffer and upscale factor. Blending with HUD, post effects and
   framebuffer reuse then works without special cases.
4. **Mesh cache**: decode a packet once into an immutable vertex/index buffer keyed
   by packet address + content hash. Open question: invalidation when streaming
   reuses model-heap memory - find the model load/free functions and hook those,
   or fall back to a cheap hash check per CALL.
5. **Shaders** replicate what the VU1 program does: transform with the uploaded
   matrices, the game's projection and GS XY offset, lighting, fog factor, vertex
   colour scale (0x80 = 1.0), UV scaling; textures still come from the GS backend's
   texture cache (PSMT4/T8 + CLUT already decoded there).
- Done when: dispatcher, survey report and A/B switch exist, and one simple program
  is converted end to end with matching captures.

Result (what was built differs from the plan above in one way: the hook point is the VU1
program start (MSCAL), not the DMA CALL - most world geometry arrives through REF tags, not CALLed
packets, and the VU memory at MSCAL holds everything a draw needs):
- `PS2X_HLE_SURVEY=1` (runtime `ps2_hle_survey.{h,cpp}`): per VU1 program / stage variant / UNPACK
  layout, calls, GS vertices and VU1 time; CALLed packets recorded as segments of the flattened
  chain; sample dumps for reverse engineering. `tools/vudis.py` disassembles VU code.
  Finding: VU1 program 0x0030 (a dispatcher into stage routines, docs/game-internals.md) makes ~95% of
  all GS vertices.
- MSCAL hooks (`ps2hle::setMscalHooks`): a game handler runs before each VU1 program and may skip
  it; `ps2hle::gs()` + `GS::submitNative` record a native draw with the frontend's current GS state
  (PRIM written as the skipped kick's GIFtag would).
- `GSNativeDraw` / HW backend: native draw commands in the same frame list as emulated draws
  (order, render targets, depth, texture cache, blend / alpha-test logic all shared); static vertex
  data cached on the GPU by content hash (MeshCreate / MeshDelete, LRU of 12000).
- `src/game/vcs_hle_world.cpp`: the prelit world pipeline (stage table 0x19, routines 0x0da /
  0x2700-variant / 0x2b6, both entries 0x0e9 = clip test first and 0x0de = sector already known
  inside) reimplemented in C++ and checked against the VU1 with `PS2X_HLE_VERIFY=1`: 607,917
  fast-path packets / 29.2M vertices bit-identical and every cull decision right - after matching
  the VU's round-toward-zero arithmetic. Native drawing (default; `PS2X_HLE=0` = VU1 path):
  culled packets are skipped, the rest drawn by `vs_world` (gs_draw.hlsl), which does the VU's
  transform / 12.4 snapping / STQ / colour / fog on the GPU; packets the VU would send through its
  clipper are clipped by the GPU at the clip test's near / far planes (SV_ClipDistance).
- Flyover 10-44 s: **17.9 -> 27.1 game fps**; render submit 37.5 -> 14.5 ms per frame; median frame
  33.7 ms = the game's own 30 fps cap (it now waits for VBlank 7 ms a frame); VU1 programs per frame
  1455 -> 400; GS-emulated vertices 287k -> 66k.
- Oddity found on the way: the VU path's clipper output (GIF tag 50: context 2, no fog) did not
  appear on screen - interiors where the camera sits inside the room (the intro cutscene's office)
  were black. Native GPU clipping draws them (with context 1 and fog, as the unclipped path
  would). Not investigated further in the emulated path.
  **Superseded 2026-10-23** (game-internals.md 6h): the clipper path does not use tag 50 (that is the second lit
  pass's), and the VU clipper's output now draws the office correctly. GPU clipping drew the GON_C3 poolside ground
  black, so clipped world packets are back on the VU1 by default (`PS2X_HLE_CLIPPED=1` = GPU clipping) until the
  VU's per-triangle clipper is reproduced (milestone 3). Cost: flyover 30 -> 25.5 game fps.
  **Done (milestone 3, 2026-10-23):** `src/game/vcs_vu_clipper.cpp` reproduces the clipper bit-exactly (1.2M + 2.8M
  GIF packets checked against the VU's kicks); clipped world packets are drawn from its GIF packets (PATH1) - render
  time within 0.5 ms of GPU clipping, the right picture (game-internals.md 6h).
- Remaining VU1 work: the lit-model pipelines (stage tables 0x16 / 0x13: peds, vehicles, props with
  normals; ~10% of vertices) and sprites / particles (0x02c0).

### Stage 5 - World sectors (L) - prelit world pipeline done 2026-10-22

- Biggest vertex share; mostly MODULATE, alpha test GREATER 8, fogged, PSMT4 +
  CT32 CLUT, vertex colours from V4-5 unpacks (game-internals.md, GS usage / level data).
- Includes the water UV scroll pass and the three per-frame sector passes
  (`FUN_0023E748`).
- Done when: captures match, render submit time on the flyover drops measurably
  (target: most of the world's share of the 34 ms).

### Stage 6 - Peds and vehicles (L) - skinned / lit models (table 0x16) done 2026-10-22

Result: the lit pass (docs/game-internals.md "Lit models") is reproduced in C++ (`vcs_hle_world.cpp`:
4-bone skinning, clip test on skinned positions, transform, fog, 4 directional lights + ambient)
and checked bit-exact against the VU1 (`PS2X_HLE_VERIFY=1`, together with the world: 1,448,812
packets / 65.3M vertices) except two lanes the lit transform's software pipeline leaves
inconsistent in the runtime's VU1 core (STQ.w, ignored by the GS, and the fog value that is
computed from it); they are not compared. Native drawing: skinning stays on the CPU (its result is
written back to VU memory, as a later pass over the same buffer reads it), the skinned vertices go
to the GPU per draw (transient, not cached) and `vs_lit` does transform, fog and lighting.
Cutscene 55-86 s: 28.1 -> 29.9 game fps (the cap), render submit 17.3 -> 11.8 ms, VU1 programs
565 -> 238 per frame. `PS2X_HLE_LIT=0` keeps lit models on the VU1.
Rigid lit models (vehicles, props: the world pipeline with lighting stage 0x4ff) followed the same
way: bit-exact reference, native through `vs_lit` with the world's folded matrices and a cached
mesh (also covered by `PS2X_HLE_LIT=0`).


- Skinning, the lighting dispatch program (0x0418, JALR table), vehicle env maps /
  specular, damage states, shadows if they go through the same programs.

### Stage 7 - Remaining programs (M-L) - left on the VU1 for now

Survey with everything above native (`build/scenes/survey4`): what still runs on the VU1 is
program 0x02c0 (sprites / particles / text: ~15 ms of VU1 per second, ~0.5 ms per game frame) and
the second lit pass (stage table 0x13: 0x0da, 0x4ff, 0x2b6, then 0x5b1 / 0x2b2 - probably an
environment-map pass over the skinned buffer; ~140 calls/s). Most of the remaining time the survey
attributes to 0x0030 is now the native handlers' own CPU work (1.4-3.3 us per packet). Both stay
on the exact VU path until a profile says otherwise; the method for adding them is the one above
(survey sample -> vudis.py -> C++ reference checked with PS2X_HLE_VERIFY -> vertex shader).


- Particles, water, shadows, sky, 2D sprites drawn through VU1. Whatever the
  coverage report shows is left; anything rare can stay emulated.

### HUD layer (milestone 3 step 2, 2026-10-23)

The HUD, text and radar are drawn over the shown picture at the window's size instead of into the PS2 frame buffer:
- **Which draws.** `src/game/vcs_hud.cpp` collects the Im2D primitives of the HUD context (CHud::Draw to the frame
  kick), groups them into elements and anchors each one (game-internals.md 6f). The renderer asks it about every 2D
  primitive into a 640-wide target while 3D is shown (`gsHwSetHudClassifier`, by vertex positions); a named one is
  squeezed towards its anchor (widescreen) and recorded as a `HudDraw`, not a `Draw`.
- **Menus** go the same way: from the menu draw (FUN_003b53f8) to the frame kick, the 2D primitives are named too,
  always anchored in the middle (never to the screen edges), also in frames without 3D (the pause menu; the
  squeeze stays for 3D frames). `VCS_MENU_NATIVE=0` = menus into the frame buffer.
- **Which frame.** The executor keeps a layer per frame buffer: HUD draws go to their buffer's layer; any other
  drawing into the buffer (the next frame's scene) makes it stale, and the next HUD draw starts it afresh. A plain
  copy of a buffer takes its layer along: VCS copies its finished frame (FBP 140) into the displayed buffer (FBP 0)
  at the start of the next frame. The copy also closes the source's layer (the next HUD draw replaces it): the pause
  menu draws nothing else into FBP 140, so its layer grew by a frame's menu every frame before.
- **Drawing.** `composeDisplay` makes the picture at the output size (`gsHwSetOutputSize`: the window's letterboxed
  area, set by the runtime each host frame), blits the displayed buffer into it (bilinear, as the presenter scaled it
  before) and draws the displayed buffer's layer on top with the same draw shaders, textures and blend modes: target
  size in PS2 pixels = output / (640x448), scissor scaled likewise, its own depth buffer cleared to 0 (the game
  clears Z to 0 just before the HUD), alpha not written (the shown picture stays opaque).
- **The PS2's pixels, drawn larger** (lines seen at 1080p fullscreen, fixed 2026-10-23). The layer keeps what
  640x448 would have shown:
  - Coverage: every layer vertex moves to the pixel boundary the PS2's coverage gives (x' = ceil(x - 0.5)), its
    triangle's STQ / colour / Z / fog re-evaluated there; triangles that then cover nothing are dropped. Without it
    sub-pixel gaps between pieces got pixels of their own: the radar map showed in a line left of its circle (map
    scissored from pixel 27, corner mask from x 27.4).
  - Sampling: a textured quad (two triangles over one rectangle, CLAMP) samples only the texels inside its own
    rectangle (REGION_CLAMP). The fonts are atlases with cells side by side; at 1080p a glyph's edge pixels sample
    within half a texel of the cell border and bilinear filtering pulled in the next cell's outline (lines under the
    clock's digits, beside its colon).
  - Widescreen (2026-10-27): the squeeze towards an element's anchor is applied in the layer after the snap, to the
    vertices and the scissor alike and unrounded (`DrawCmd::hudSqueeze`). Squeezed before the snap, the layer
    rounded to PS2 pixels of the squeezed frame - 6 screen pixels each at 32:9 - and the radar's scissor and corner
    mask landed on different ones: a column of map outside the circle (the user's report).
- `VCS_HUD_NATIVE=0` = the HUD drawn into the frame buffer as before (with the anchoring still applied);
  `PS2X_HW_HUD_LOG=1` logs the layers every 2 s; `PS2X_HW_OUTPUT=WxH` forces the output size (a full-screen window
  in a test run, with `PS2X_CAPTURE_SIZE`).
- Textures: the GS backend's decoded texture cache (the same textures the emulated path samples) rather than a
  decoder for RenderWare rasters (DECISIONS.md).

### Post effects (milestone 3 step 3, started 2026-10-23)

The user's priority: authentic at the original 640x448 first, then at higher resolutions and other aspect ratios.
VCS's effects (game-internals.md 6g: frame copies, self-copy, bloom, trails, motion blur) are GS draws that read
render targets; at 640x448 the emulation of those draws is the reference, so the work so far makes that exact and
then makes it hold at higher resolutions:
- **GS pixel centres** (`gsPixelCentreOffset`, `PS2X_HW_PIXEL_CENTRE=0` = before): every draw (GS path and native
  draws: `rtOrigin.z` in the vertex shaders) moves by +0.5 pixel and the scissor with it, so each target pixel
  samples coverage, colour and texture coordinates where the GS samples (pixel X at X). Before, the per-frame copies
  averaged neighbouring texels and the picture was softer than the PS2's. The HUD layer's pixel snapping and the
  widescreen scissor squeeze follow the same rule.
- **Copies at full resolution** (`rtFull`, `PS2X_HW_RT_FULL=0` = before): a draw that reads a render target 1:1 (one
  texel per pixel, no rotation; checked per draw from its vertices) reads it at the target's resolution
  (`sampleRtFull`; wrap / clamp still in PS2 texels). At scale 1 it fetches the same texels. Before, every copy
  brought the picture back to 640x448, so a higher internal resolution was lost (the shown buffer is a copy).
  Resampling reads (bloom / trails downsamples and their blends back) keep PS2-texel sampling: they look as at
  640x448 at any scale.
- Checked: 640x448 before / after (sharper, matching the GS's sampling; static regions of a frame identical with and
  without `rtFull`), 3x internal (fence and wire at full detail instead of a 640x448 picture scaled up), HUD at
  1080p, reference scenes; game fps unchanged.
- **Native bloom and trails** (`nativePost`, `PS2X_HW_POSTFX=0` = as recorded, `=b` / `=t` one of them): the two
  chains are recognised in a frame's commands (bloom: downsample into a 256-wide buffer, threshold, n x (clear, 4-tap
  cross), add back; trails: downsample into a 512-wide buffer, warped blend back at FIX ~8, tint add) and drawn into
  buffers of our own with the same draws, shader and blend states (draw() overrides: target, texture, vertices,
  scissor scale). At 4:3 they give the emulation's picture (static regions as close as two runs of one build). In
  widescreen the buffers are w = display aspect / (4/3) times wider: positions in a buffer scale by w and texture
  coordinates into one become u + (w - 1) * s * x (s = buffer texels per target unit at 4:3), so the taps and the
  warp, in texels, cover the same screen distance as at 4:3 - before, both were w times wider on screen (the glow
  beside a pole at 16:9 is narrower by about that). The buffers are written back, squeezed, into the effect buffers
  in VRAM for later readers (the game textures vehicles with the 128x128 corner of FBP 420). REPEAT wrapping is a
  modulo in the shader (the widened buffers are not powers of two; same result for GS sizes).
- **Frame copy and motion blur** (`nativeFrameCopy`, letter `c` of `PS2X_HW_POSTFX`): the full-screen 1:1 reads of
  another 640-wide target (u = x + 0.5) that the game draws as 40 strips: a plain copy (the shown buffer) is a
  texture copy, a blended one (motion blur, colour 127/128 at FIX 16) one full-screen quad with the draw's own state.
  Same picture as the strips (static regions within run-to-run noise). The self-copy over the sky stays a GS draw
  (depth-tested; 1:1, so read at full resolution).

### Native frame (milestone 3 step 4, started 2026-10-23)

"Internal resolution: Match window" (`display.scale` 0, `PS2X_HW_SCALE=0`): the picture's own targets - the
640-wide ones: the drawn frame FBP 140, the shown copy FBP 0, the Z buffer FBP 280 - are drawn at the window's size
(the letterboxed area at the 3D frames' aspect, `gsHwSetFrameSize`; it stays when a menu shows at 4:3), every other
target (effect buffers) at 1x - except the reflection map, which the game names a window-scaled target
(`gsHwSetTargetRole`, docs/FULLY_NATIVE.md step 3d; `PS2X_HW_REFLECTION_SCALE=0` = 1x). The radar has no target: it is
drawn in the HUD layer. So the frame is not a multiple of 640x448: at 16:9 1080p
it is 1920x1080, 3 x 2.41 pixels per PS2 pixel.
- Each target and depth buffer has its own scale (`Rt::scale`, `scaleFor`): creation, growth (rescaled when the
  window changes), uploads, clears, depth fills, scissor, the display, write-backs; the shader reads a render target
  with separate x / y scales (`rtScale2`, `fetchRtFull` for fractional scales). A Z buffer is kept per scale (the
  effect buffers carry the frame's ZBP), and ZTST ALWAYS without Z writes binds none.
- With the copies native and 1:1 reads at full resolution, nothing brings the picture back to 640x448; the bloom /
  trails stay at their 1x buffers (their look as on the PS2). The HUD layer is drawn at the same size.
- `PS2X_HW_FRAME=WxH` forces the frame size (tests in a small window, with `PS2X_HW_OUTPUT` / `PS2X_CAPTURE_SIZE`).
- Checked: scenes at 16:9 1920x1080 against 1x (static scenes the same: loading 1.24, pause map 0.05 mean difference;
  3D sharper, effects as before), 1x unchanged by the per-target scales. Game fps, two runs each (GTX 1080 Ti):
  flyover 25.5 / 27.5 native frame vs 28.2 / 28.2 at 1x, cutscene 28.0 / 30.0 vs 28.4 / 30.0, gameplay at the 30 fps
  cap (one 1x run dipped to 22.5 - the runs vary more than the difference). Not checked: a window resized while
  running (the frame targets are then rescaled, `growRt`).
- **MSAA** (Display > Anti-aliasing, `display.msaa` / `PS2X_HW_MSAA` = 2 / 4 / 8, restart; off by default): the
  picture's targets (640 wide) and their depth buffers are multisampled; every read of such a target - render-target
  textures (effects, copies, the self-copy), the display, growth, dumps - sees a single-sampled copy resolved when the
  target was drawn to since the last read (`readable`, `Rt::dirty`); a multisampled frame copy is drawn instead of
  copied. Effect buffers stay single-sampled. Depth buffers are kept per scale and sample count.
- **Anisotropic filtering** (Display > Texture filtering, `display.aniso` / `PS2X_HW_ANISO` = 2-16, restart; off =
  the original bilinear): the game's textures get a mip chain, and bilinear-filtered draws with plain repeat / clamp
  wrapping (about 90% of textured draws outside the barracks) are sampled by the hardware - trilinear, anisotropic -
  instead of the GS's own bilinear (`linearIipFgeAte.x == 2`, `hwSampler`); region wraps, render targets and nearest
  filtering keep the exact GS path. `PS2X_HW_ANISO_LOG=1` counts which path textured draws take. The chain is built on
  the CPU (2x2 box, rounded: `buildMipChain`) and the texture made in one plain creation with every level
  (`rhi::Device::createTextureLevels`) - before 2026-11-02 each new texture was a render target with a `GenerateMips`
  pass on the main thread, which made texture bursts hitch (PERFORMANCE.md 21; `PS2X_HW_CPU_MIPS=0` = before).
- **New textures are made off the main thread** (D3D11; Vulkan when asked, 2026-11-02, PERFORMANCE.md 21, 25): the GS stage decodes a texture
  (a paletted one through a 256-entry table: `GSCpuBackend::paletteTable`) and hands the pixels to two helper threads
  that make it on the free-threaded device (`rhi::Device::freeThreadedCreation`, `HwBackend::makeTextureEarly`); the
  executor takes the finished texture at its `TexCreate` (`EarlyTexture::done`). Vulkan makes the image there and stages
  its first contents; the recording thread records the copies into the set-up commands at its next submit
  (`stage` / `drainStaged`). Off by default with Vulkan (no gain measured on a 2-core laptop, PERFORMANCE.md 25):
  `PS2X_HW_EARLY_TEXTURES=1` turns it on, `=0` off everywhere.
- **No shader depth on Vulkan** (2026-11-02, PERFORMANCE.md 26): the GS pixel shader is built with and without its
  depth output (SHADER_DEPTH); Vulkan uses the rasterised depth - the same value with a 0..1 depth range - so early depth
  rejection works; D3D11 too since 2026-11-03 - with the shader depth it lost the game's fire sprites
  (game-internals.md 6m; `PS2X_HW_SHADER_DEPTH=1` brings it back). The draw constants are
  written only when they change (`updateConstants`), and the Vulkan backend records viewport, scissor, blend constants
  and buffers only when they differ from what the command buffer has. Depth test / write / compare are dynamic state:
  pipelines that differ only there share one VkPipeline (`PS2X_RHI_STATIC_DEPTH=1` = baked in), and a dynamic vertex
  buffer's ring slice holds only the bytes last written (`PS2X_RHI_FULL_COPY=1`; PERFORMANCE.md 27).
- **The GS bilinear by Gather** (2026-10-30, PERFORMANCE.md section 15): with filtering off, the same draws (power-of-two
  textures without a mip chain) keep the GS's bilinear but fetch its 2x2 texels with `Gather` through a nearest sampler
  of their wrapping (`linearIipFgeAte.x == 3`, `m_gatherSampler`), drawn with a pixel shader built for that path alone
  (then `GATHER_ONLY`; since 2026-10-30 `SAMPLE_MODE` 5 of the per-read variants, `drawPS[5][dual]`, pipeline key bits
  24-26 - PERFORMANCE.md 18). The same texels and weights as the four loads (`PS2X_HW_GATHER=0`;
  `=verify` draws both and marks a differing pixel magenta). It made the helicopter's dust - 60 screens of textured
  overdraw - hold 60 fps at 3x.
- RHI: texture sample counts and mip levels, `resolveTexture`, `generateMips`, sampler anisotropy (D3D11).
- Checked at 16:9 1080p native frame: MSAA 4x smooths the polygon edges (poles, roofs), AF 16x keeps the paving and
  ground decals sharp into the distance; reference scenes all run, static scenes unchanged (loading 0.55, pause map
  0.13); game fps unchanged (flyover 27.2, cutscene 30, gameplay 30 with both on).
- Not done yet: higher-precision depth (D32F already holds the PS2's 24-bit Z exactly); the radar / reflection map at
  higher resolution.

### Render submission worker (milestone 4 performance, 2026-10-23)

Render submission - the VIF1 / GIF chains, VU1 microprograms (and the native draws they reach), the GS front end and
the HW backend's frame building - runs on its own thread, `RenderWorker` (`runtime/ps2_render_worker.h`,
`PS2X_RENDER_THREAD=0` for the old synchronous path; on by default with `PS2X_GS=hw`). The game thread copies each
chain when kicked (as before), marks the DMA complete at once (as before), and posts the work; the worker runs it in
kick order. The frame's end (`endFrame` at VBlank, with the display registers read then) is posted too, and the game
thread waits rather than get more than one frame ahead (`PS2X_RENDER_THREAD_AHEAD`).
- **Fences**: whatever the game can observe of that work waits until the worker has run everything posted before:
  GS CSR / SIGLBLID reads and CSR writes, GIF / VIF1 registers, VIF1 / GIF FIFOs, VU1 memory, the HLE GS library
  stubs, and any other direct call into the GIF / VIF1 from the game thread (`FenceReason::Other`, a safety net).
  In gameplay only the CSR is read, ~5 times a frame (game-internals.md 4); waiting costs 2-8 ms per second.
- **In order with the chains**: VIF1 FIFO writes; the HUD anchor map (built at the frame kick, handed to the
  classifier through the queue - `vcs_hud.cpp`). The native GIF fast paths (`tryProcessNativeGif*`) step aside: their
  chains take the copied path to the worker.
- **Thread safety**: the VU1 start callbacks don't touch the game thread's context on the worker (no VU1 D / T stops
  there - VCS never enables them); the VU decode's MAC-read tables are locked (VU0 decodes on the game thread at the
  same time); chain buffers come from a pool shared by both threads; the worker runs with the game thread's MXCSR.
- `PS2X_RENDER_THREAD_LOG=1`: items / frames per second, game-thread waits, fences by reason. The perf line gains
  `worker_ms` (the worker's busy time per frame); `render_ms` is now only the game thread's part (copying chains,
  waiting). `VCS_PROFILE_THREAD=RenderWorker` samples the worker.
- Checked: reference scenes, worker off vs on: static scenes the same (loading 0.00, pause map 0.40 mean difference),
  gameplay / cutscene pictures alike apart from timing; HUD at the screen edges (16:9) and the pause menu pages
  right; world tour subset (18 points) without crash; soak below.
- Measured (60 fps mode, i7-7700K): outside the barracks at the 60 cap with 4 ms to spare (game thread 11.5 ms, worker
  ~5 ms); driving (AI car from Fort Baxter) 26-29 -> 44-54 fps - game code ~17-20 ms is now the limit, worker ~17 ms
  in parallel; reference scenes' flyover 42.5 -> 59.9 fps.

### The GS stage (2026-10-25, docs/PERFORMANCE.md 7)

The render worker split in two (`PS2X_GS_THREAD=0` = one thread as before; on by default with the worker and the HW
renderer). The worker keeps the VIF1 chains, RSL replay, VU1 programs and their native reproductions (skinning,
clipping, lighting); the GS front end and the HW backend's frame building (draw state, texture binding and decoding,
uploads, CLUT loads, packed / native draw recording, the frame end) run on a second thread, `GsStage`.
- **Commands**: the GS entry points the worker calls (`processGIFPacket`, `submitPackedKick`, `submitNative`,
  `writeRegister`) go into a single-producer ring (64 MB, `ps2_render_worker::gsReserve / gsSend`) with their data
  copied - GIF packets, the native draw and its vertices - and the worker's thread-locals the GS reads (GIF path, PATH1
  producer = draw source, the upload's raster) captured; the stage runs them in order.
- **Answers the worker needs**: `submitPackedKick`'s "not that shape" fallback (`processGIFPacket`) runs on the stage;
  `submitNative`'s refusal is decided by the draw alone - the backend's `AcceptsNative`, which SubmitNative uses too -
  so the worker knows at once whether to run the VU program instead (PRIM is written either way, as before).
- **In order with the commands**: the frame end (`endFrame`, run through `runOnGsStage`; the worker is never more
  than one frame end ahead of the stage) and the HUD anchor map (`vcs_hud.cpp`). The rest of the GS that the worker
  could reach (native packed / image upload paths, framebuffer clears, local->host transfers) drains the stage first
  and runs on the worker.
- **Fences** wait for the worker, then for the stage (`gsDrain`). Off while `PS2X_HLE_VERIFY` / `PS2X_HLE_SURVEY` read
  what the GS recorded from the worker, and with the GPU renderer.
- The perf line gains `gs_ms` (the stage's busy time per frame; `worker_ms` is the worker's own now).
- Measured (uncapped, `--opt none`): city 1325,-325 (~1,150 draws) 142 -> 215 fps (worker 6.7 ms -> 3.5 + stage 4.5
  in parallel); the world tour's slowest points were all worker-bound. Reference scenes: static ones unchanged
  (loading 1.12, pause map 1.81); tour subset unchanged; save / load / delete through the menus pass.

### Draw sources - what is still drawn through the GS path (milestone 3, 2026-10-24)

The roadmap's "unconverted draws" counter. Every recorded GS-path draw carries where its GIF packet came from
(`runtime/gs/gs_draw_source.h`: PATH2, PATH3, or PATH1 from a VU1 program / from a C++ reproduction of one - the
`hle` packets: the clipper, the env pass, 0x02c0, variant C, which compute natively but still hand GIF packets to the
GS front end). The executor counts per frame:
- perf line / CSV / overlay: `gs_draws` `gs_verts` (draws still made from GS primitives), `gs_vu1` `gs_hle` `gs_path2`
  `gs_path3` (the same by source), `native_draws`, `hud_draws` (native HUD layer), `post_draws` (GS draws replaced by
  the native post passes);
- by kind (source + VU1 program, target, texture / render target, blend, depth, "full" = covers its scissor, "far" =
  at the far plane): the rig's `gsdraws [reset]`, `PS2X_HW_DRAW_SOURCES=1` (logged every 5 s). `tools/tour.py` and
  `tools/missions.py` record it per point / mission and the tour summary lists the kinds.

First survey (2026-10-24: reference scenes, the tour subset, 12 missions 6-94), per game frame:

| | reference gameplay | city (tour subset) | missions |
|---|---|---|---|
| GS-path draws | 59 (9.9k verts) | 106 mean, 231 max | 62 mean, 150 max |
| of them `hle` (C++ reproductions) | 45 | ~88 | ~47 |
| VU1 | 0 | 9 (clipped 0x02c0 shine) | 4.4 in PHI_A1 |
| PATH2 | 14 | ~15 | ~15 |
| PATH3 | 0 | 0 | 0 |
| native / HUD layer / post | 237 / 18 / 16 | | |

- The VU1 draws were 0x02c0 entry 0x0d (shine) packets that cross the guard band or are culled, or that use one of
  two more code images: reproduced / accepted since (game-internals.md 6d), so **no VU1 draws** in the tour subset or
  the missions. The **setup programs** (matrices, per-batch tables, bone copies; ~330 runs a frame in the cutscene)
  are native too: VU1 programs a frame 195 -> 4 (cutscene), 86 -> 4 (gameplay), tour subset mean 104.6 -> 3.4,
  worker 8.76 -> 8.05 ms. The last ones - the per-frame Z clear (a second low code image) and table-0x13 packets
  through the clipper - followed: **no VU1 program runs in a normal frame** (`vu1_programs` 0 in the reference scenes
  and the tour subset).
- PATH2 (VIF DIRECT, the game's 2D / effect packets): the reflection map built in FBP 420 (a 128x128 sample of FBP
  140 plus a sun sprite), coronas / light sprites, and the full-screen effect draws not taken by the native post
  passes (colour-filter quads, the depth-tested self-copy over the sky, fades). Flat full-screen draws are resolution
  independent; the reflection map stays 1x.
- Found with it: every VU1 XGKICK had gone to the GS **twice** since the paraLLEl-GS integration (a brace-less
  `else` in the VU1 core; PS2RECOMP_PATCHES.md). Only the VU1 path was hit: with `PS2X_HLE=0` the cutscene drew
  220,848 vertices a frame instead of 122,823, gameplay 98,124 instead of 50,768 (worker 23.1 -> 15.2 ms); blended
  VU1 draws were doubled in the reference pictures. The native path (default) was unaffected (scenes identical).

**Packed draws (2026-10-24).** The C++ reproductions' strips and fans (clipper polygons, env pass, 0x02c0 re-draws
and shines) no longer go through the GS front end: `GS::submitPackedKick` takes a single-tag PACKED ST / RGBAQ /
XYZF2 packet, decodes its vertices as `writeRegisterPacked` does and hands them to the HW backend at once
(`SubmitPacked` -> `recordPacked`), which records them with the same per-primitive code as `Submit`
(`recordPrimitive`: flat colour, render-target sampling, widescreen squeeze, merging) - not asked: the HUD
classifier (3D packets are never HUD); depth-buffer / target bookkeeping once per packet. Register packets (TEX0,
TEXFLUSH) still take the front end; the GS registers and vertex queue are left as the per-vertex path leaves them.
`PS2X_HLE_PACKED=0` = all through the front end.
- Checked with `PS2X_HW_PACKED_VERIFY=1` (each packet also recorded one `Submit` per triangle into a scratch frame,
  every vertex and its draw state compared): a city spot 4,180,000 packets, the reference scenes 5,300,000, PHI_A1
  4,580,000 - all exact (2 split into a different number of draw calls with the same vertices).
- The counter shows them as `packed_draws` / `packed_verts`, not GS-path draws: at (1325, -325) GS-path draws went
  from ~224 to ~15 a frame (what is left: PATH2 and register packets).
- `tools/bench.py`, one run each (same build, `PS2X_HLE_PACKED=0` vs default), render worker ms: city (1325, -325)
  14.5 -> 11.7, AI drive 21.2 -> 18.2 (41.3 -> 44.9 fps); the light spots (~2-3 ms) unchanged within noise.

**The last three (2026-10-26, FULLY_NATIVE.md 5e).** A+D runs that continue the current PRIM (queue vertices first,
without a kick), A+D packets with pure state writes before PRIM (written through the register path after the whole
packet is checked), and Im2D line strips / lines (`recordPacked` mode 1, as `Submit`) - checked with
`PS2X_HW_PACKED_VERIFY=1`, 7.5M packets exact. `gs_draws` 0 in the reference scenes and the tour subset.
`PS2X_HW_PATH2_CONTINUED=0` / `PS2X_HW_PATH2_LEAD=0` / `PS2X_HW_PATH2_LINES=0` = the front end;
`PS2X_HW_FRONT_DRAW_LOG=<n>` logs the draws the front end still makes, with their packets.

**PATH2 packets recorded whole (2026-10-24, docs/FULLY_NATIVE.md step 1).** The game's own 2D / effect packets take
the same route: `GS::processGIFPacket` hands a PATH2 packet to `submitPackedKick` first, which now also takes
- Im2D's packet - one PACKED tag, ST / RGBAQ / **XYZ2** (`0x512`: a 32-bit Z, fog from the FOG register), the shape of
  every PATH2 packet that draws (game-internals.md 6f) - and
- A+D vertex runs: `PRIM`, then only RGBAQ / ST / UV / XYZ2 / XYZ3 writes (the effect strips); a packet with any other
  register goes to the front end untouched (nothing written before the check).
Q starts at 1.0 at the tag, as for every GIFtag; the registers and vertex queue are left as the per-vertex path
leaves them. PATH2 primitives are asked about by the **HUD classifier** with their GS vertices, as `Submit` does; and
the classifier is now asked about PATH2 draws only, in both paths: a 3D triangle (PATH1) whose corners happened to land
on HUD vertex positions had been taken into the HUD layer and squeezed (found at 16:9 by the check below).
`PS2X_HW_PATH2_PACKED=0` = the front end.
- Checked with `PS2X_HW_PACKED_VERIFY=1`, which now also compares whether each vertex went to the HUD layer: a city spot
  at noon and night, 4:3: 3.9M packets exact; 16:9 with "Screen edges" HUD: 5.14M exact.
- GS-path draws a frame: city spot 15.8 (noon) / 18.5 (night) -> 3.0; tour subset mean 3.0, max 3.0. Left: the three
  draws that read render target 140 - the reflection map's two copies into FBP 420 and the depth-tested self-copy over
  the sky (their packets set state before `PRIM`) - which wait for render targets outside GS memory (FULLY_NATIVE.md
  step 3). Reference scenes ok, tour classes unchanged.

### Stage 8 - Enhancements (M each, optional order) - internal resolution done

- Internal resolution: native draws render into the same scaled render targets as the GS path, so
  `PS2X_HW_SCALE` (or `scale=<1..8>` in vcs.ini next to vcs.exe) raises the whole picture's
  resolution; checked at 3x (gameplay still at the 30 fps cap). Native draws keep the PS2's
  1/16-pixel vertex snapping (~0.2 px at 3x).
- Widescreen is not done yet; planned in docs/SETTINGS_MENU_WIDESCREEN.md. The game has its own
  widescreen mode (aspect global 0x487488, preference at *(0x48F3D0)+0x30, a HUD that follows it),
  which drives the camera, culling and both renderers - so no projection patching is needed.
- MSAA / anisotropic filtering are not done: the GS emulation samples textures with explicit
  texel loads (to reproduce the GS wrap / clamp modes and render-target textures) and reads render
  targets back as textures, which an MSAA target would need resolving for at every such use.
  Higher internal resolution is the supersampling route that works today.


Only possible cleanly once the 3D scene is native:
- Native-resolution 3D independent of the PS2 framebuffer size (GS path keeps
  `PS2X_HW_SCALE`), MSAA, anisotropic filtering, higher-precision depth.
- Widescreen: projection change in the camera code + HUD aspect handling
  (game-internals.md "Camera and projection" / "HUD" sections still to fill).
- Each as an option that defaults to authentic output.

### Vulkan backend (Linux, 2026-10-30)

`src/rhi/vulkan/rhi_vulkan.cpp` implements rhi.h on Vulkan 1.3 (dynamic rendering, synchronization2, push descriptors;
volk + VMA). It keeps the D3D11 backend's behaviour, which the renderer was written against:
- D3D's Y-up NDC through a negative viewport height (the shaders are unchanged); depth 0..1 and pixel centres at +0.5
  are the same in both, so `gsPixelCentreOffset` holds.
- One push-descriptor set with the D3D registers as bindings (tools/hlsl_spirv.py: bN -> N, tN -> 16 + N,
  sN -> 32 + N). Unbound slots and a texture that is also the pass's target read zeros, as D3D's null views do.
- State stays bound across draws and passes; beginPass unbinds textures and resets viewport / scissor (as D3D11's).
- Dynamic buffers: a new slice of the frame's upload ring per update (WRITE_DISCARD), and their CPU copy goes into the
  ring again when a later frame draws with them unchanged. Static buffers / textures are updated by copies.
- Copies, uploads and readbacks inside a pass suspend its rendering and resume it with LOAD (`PS2X_RHI_STATS=1` counts
  them). Textures rest in SHADER_READ_ONLY (or their attachment layout) and return there after each operation.
- `depthClip=false` is depth clamp (D3D11's DepthClipEnable=FALSE clamps to the viewport, too).
- New targets start as zeros; pipelines are made per (colour format, depth format, samples) at the first draw.
- Dear ImGui is drawn through the RHI (render/ui_renderer.cpp, shaders/ui.hlsl).
- Hidden windows get no swapchain (an off-screen back buffer).
Checks still to do: picture against D3D11 on the same PC (scenes), validation clean, the in-pass suspension count.

### D3D12 backend (Windows, 2026-11-04)

`src/rhi/d3d12/rhi_d3d12.cpp` implements rhi.h on Direct3D 12 (feature level 11_0), for the main thread's per-draw
cost on AMD's D3D11 driver (DECISIONS.md 2026-11-04; the default on Windows since 2026-11-05, D3D11 when no D3D12
device can be made; Graphics > Graphics API, `VCS_RHI=d3d12`). Built as the Vulkan
backend, with D3D11's DXBC shaders unchanged:
- One root signature: b0..b3 root constant buffer views (vertex + pixel), t0..t7 and s0..s7 descriptor tables (pixel).
  The texture tables are copied into a shader-visible heap per frame (cached by contents, skipped when the draw's
  textures are the last draw's); sampler tables live while the sampler heap lasts. Unbound slots read a null view.
- Dynamic buffers as Vulkan's: the CPU copy, a slice of the frame's upload ring at the first use after an update.
  Static buffers (the native meshes) are ranges of 32 MB GPU pages, filled by the set-up commands (submitted before
  the frame's own): a committed resource per mesh was a driver allocation each - up to 125 a frame while driving, the
  main thread at 9-160 ms a frame; with the pages 1-2 ms (hidden rig, Starfish Island / Viceport).
- Textures carry their resource state; a pass's targets go to RENDER_TARGET / DEPTH_WRITE for it and back to their
  resting state after (PIXEL_SHADER_RESOURCE when sampled). No render-pass API: copies inside a pass are plain.
- Pipelines per (colour format, depth format, samples) at the first draw; generateMips draws each level from the one
  above (D3D12 has no GenerateMips); new committed resources start as zeros.
- Textures made on helper threads (freeThreadedCreation; the GS textures' helpers are on with D3D12, as with D3D11) stage
  their data in ranges of 64 MB upload chunks, copied by the owner's set-up commands at its next submit. Sampled textures
  with data are placed in 128 MB heaps; targets stay committed. Dear ImGui through the RHI (render/ui_renderer.cpp).
- Frees (after the frame's fence) run on a thread of their own (`RhiReaper`): releasing a resource costs ~0.25 ms in
  the driver and the texture cache lets 750-3800 go at once - 160-200 ms of the main thread in one frame before.
- Stutter rig (`cruise.py`, 144 km/h through the city, 2026-11-04): D3D11 p95 20.6 / p99 33.1 ms, 297 frames over
  25 ms, 45 hitches; D3D12 p95 16.7 / p99 16.9 ms, 37 frames over 25 ms, 16 hitches (texture bursts of 200-350 new
  textures left, in both). `PS2X_HOST_SPIKES=<ms>` prints a long frame's D3D12 time: pipelines, textures, buffers,
  memory, GPU waits, present, frees.
Checked against Microsoft's "Porting from Direct3D 11 to Direct3D 12" (the player's reference): device / swapchain made
apart through DXGI, committed resources, uploads through mappable memory with GetCopyableFootprints, one PSO for the
state, the DIRECT list as the immediate context, fences and a ring for WRITE_DISCARD, descriptor tables renamed by the
app, explicit ResourceBarrier transitions, FLIP_DISCARD with one RTV per back buffer, GenerateMips by shaders. Not yet:
shaders built as SM 5.1 / 6 (the guide's advice; D3D11's SM 5.0 DXBC runs), GPU timing through a query heap
(`PS2X_GPU_TIMING` is D3D11 only).
Checked (2026-11-04, hidden rig): pictures as D3D11's at the same spots; the debug layer clean (`PS2X_RHI_DEBUG=1`
prints its messages into the log); `PS2X_RHI_STATS=1` per-frame counts (draws, passes, barriers, textures / buffers /
pipelines made, tables, fence waits). To do: main-thread ms on the player's PC in the dense streets (a hidden window
does not show AMD's D3D11 cost), scenes compare, the readback of the rig's brightness probe (a wait every frame, as
on D3D11).

## Verification

- Every stage: the stage 0 capture diff over the reference scenes; perf CSV
  before/after recorded in PROGRESS.md.
- HLE handlers: per-program A/B (`PS2X_HLE`), and a debug mode that draws the native
  and emulated versions of the same packet into two targets and reports the diff.
- D3D11 debug layer clean (no warnings) in debug runs.

## Risks and open questions

- GS tricks the native path must respect: Z cleared by drawing CT32 strips into the
  depth buffer's memory (handled today as `DepthFill`); framebuffers reused as
  textures for post effects; the PMODE two-circuit anti-flicker blend; 16-bit
  targets for effects. Keeping native draws inside the GS backend's frame list is
  what makes these keep working.
- Mesh cache invalidation under streaming (stage 4, item 4).
- Programs that read back results or depend on VU1 side effects (none known for
  rendering, but the survey must check XGKICK-free paths and MAC-flag use).
- Open bugs that may live in the GS backend (screen black for a few frames,
  frontend menu not appearing): don't let the port hide them - reproduce them on
  the GL executor first if they change behaviour.
- Upstream divergence grows: raylib removal touches several runtime files.

## Decisions to confirm in review

1. SDL3 for the platform layer.
2. Own thin RHI (D3D11 now) instead of an existing library.
3. Stage 1 keeps OpenGL briefly so the platform swap and the API swap are separate,
   bisectable steps (costs a small throwaway GL blit).
4. paraLLEl-GS kept as readback-only (its Vulkan window present is dropped).
5. HLE hooks at the VIF1 packet level with a mesh cache, not in recompiled game code.
6. The emulated GS path stays as fallback and reference indefinitely.

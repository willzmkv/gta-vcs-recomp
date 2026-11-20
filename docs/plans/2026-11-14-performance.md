# Performance: GPU Skinning and the Rest of RSL Submission Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lower frame time in dense streets with the raised population: skin the peds on the GPU instead of the render
worker's CPU, and finish the RSL-level submission (docs/RSL_SUBMISSION.md phases 3-4) so no 3D draw is built from DMA
packets.

**Architecture:** Skinned peds go through the native lit/skinned pass in `src/game/vcs_hle_world.cpp`
(`skinScalar` / `skin`, the 4-bone VU1 skinning of stage 0x5a5 reproduced bit-exact on the CPU). GPU skinning moves the
bone palette into a constant buffer and the weighted transform into the vertex shader (`src/render/shaders/gs_draw.hlsl`,
SPIR-V through `tools/hlsl_spirv.py`), keeping the CPU path as the reference (`PS2X_HLE_VERIFY`-style compare and a
switch). RSL phases 3-4 follow docs/RSL_SUBMISSION.md as written.

**Tech Stack:** C++ (src/game, src/render, src/rhi D3D12 / D3D11 / Vulkan), HLSL.

---

## Task 0: Measure first (CROWD_BANDS_PLAN.md step 6 says so)

- [ ] `VCS_PROFILE=1` sampling profile on the avenue benchmark with the bands on and off
  (`python tools/bench_avenue.py build/bench/skin --rounds 2 VCS_CROWD_BANDS=0,1`): the render worker's share in
  `skin` / `skinScalar`, per frame in ms. PERFORMANCE.md 43 had ~10 % of the worker with the full crowd. Stop here if
  it is under 0.5 ms a frame with the bands on (record it), and go to Task 3.

## Task 1: GPU skinning

**Files:**
- Modify: `src/game/vcs_hle_world.cpp` (the skinned branch of the lit pass: emit unskinned positions / normals + 4
  weights / indices per vertex and the packet's bone matrices instead of skinned positions)
- Modify: `src/render/shaders/gs_draw.hlsl` (a skinned vertex path: `float4x4 bones[64]` palette in a constant buffer,
  weighted sum in the VU's operation order)
- Modify: the RHI draw item (`src/rhi/`: a palette upload per skinned draw), the presenter's input layout
- Modify: `docs/NATIVE_RENDERER.md`, `docs/PERFORMANCE.md`, `docs/PS2RECOMP_PATCHES.md` if the runtime changes

- [ ] **Step 1: Reference capture**: with `PS2X_HLE_VERIFY=1` record N skinned packets' CPU-skinned positions.
- [ ] **Step 2: Shader path** behind `VCS_GPU_SKIN=1` (off): palette upload, shader skinning; compare the GPU result by
  reading back the transformed vertices of the captured packets (stream-out on D3D11 or a compute copy) - within 1 ulp
  of the CPU (`FE_TOWARDZERO` not reproducible on GPU: record the max difference; pictures must match the reference
  scenes).
- [ ] **Step 3: Measure** Task 0 again with it on; pictures (`tools/scenes.py compare`); D3D12, D3D11 and Vulkan.
- [ ] **Step 4: Default on** if the worker gains and pictures match; reviewer; commit.

## Task 2: RSL submission phases 3-4 (docs/RSL_SUBMISSION.md)

- [ ] Phase 3 - textures without GS memory for 3D draws (the plan's bullets), behind a switch, verified with scenes
  compare and the tour subset.
- [ ] Phase 4 - an empty chain: 2D, effects and the Z clear moved to their RSL / Im2D call sites, one at a time.
- [ ] "Done when" of RSL_SUBMISSION.md; each phase reviewed and committed separately.

## Task 3: Profile the dense street again and pick the next

- [ ] After Tasks 1-2: the avenue benchmark and a sampling profile; the top three costs written into PERFORMANCE.md
  with a proposal each.

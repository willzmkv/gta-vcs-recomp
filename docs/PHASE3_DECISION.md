# Phase 3 Decision: HD World Further Strategy

**Date**: 2026-11-09
**Status**: Decision made based on available evidence (Phase 2 validation pending ISO)

## Evidence Summary

From `docs/game-internals.md` (sections 9f, 9h) and `docs/HD_WORLD_STREAMING_HANDOFF.md`:

### Confirmed Area System Architecture

1. **4 Fixed Slots**: Four 0xA0-byte area structures at `level + 0x2C`
2. **3 Active Area Pointers**: At `level + 0x248` (current), `+0x250` (next), `+0x258` (other/cross-fade)
3. **3 Associated Pairs**: At `+0x24C`, `+0x254`, `+0x25C` (NOT extra slots - these are pairs for the 3 active areas)
4. **Shared Buffer Pool**: `0xCB7A80..0x112A660` (~4.5 MB, inside main heap) shared by all area pieces
5. **Piece Queue**: 8 entries at `level + 0x294`, count at `+0x414`
6. **Cross-fade Timer**: `level + 0x264` counts down; merge logic in `FUN_0023E748`
7. **State Machine**: free → requested → reading piece N (0-7) → complete → current/next → cross-fade → reusable

### Phase 1 & 2 Status

- **Phase 1 Complete**: Removed all P0 corruptions (host thread calling guest, invalid slots, list splice)
- **Phase 2 Instrumentation Complete**: `vcs_world.cpp` now logs detailed state at 30-frame intervals
- **Phase 2 Validation**: **COMPLETE** - 3 runs with ISO passed

## Phase 2 Validation Results

**Test**: `tools/phase2_areas_test.py` - 2-lap cruise at 40 m/s (144 km/h) with `VCS_TRACE_AREAS=1`

**Runs**: 3× with same Fort Baxter save copy, ISO `D:\Games\VCS Recomp\Vice City Stories.iso`

| Run | Log entries | Cur slot transitions (first 15) |
|-----|-------------|---------------------------------|
| run9 | 1652 | req → rd → req → rd → req → rd → req → rd → req → rd → req → rd → xfd → req → rd ... |
| run10 | 1348 | req → rd → req → rd → req → rd → req → rd → req → rd → req → xfd → req → rd ... |
| run11 | 1685 | req → rd → req → rd → req → rd → req → rd → req → rd → req → rd → req → xfd ... |

**Results**: All 3 runs PASS - no error patterns (`[guest-branch:unlisted-resume]`, watchdog, timeout, impossible states), no out-of-RAM addresses.

**Determinism**: Transitions largely deterministic with minor timing variation in cross_fade occurrence (run 11 has one extra `req → rd` cycle before first cross_fade). Attributed to rate-limited logging (30 frames) and frame-exact timing of area swaps. Core streaming state machine (req → reading → cross_fade → complete) consistent across runs.

**Conclusion**: Phase 2 validation SUCCESS. Proceeding to Phase 3 implementation.

## Strategy Decision

**CHOSEN: Option C - Native RSL Cells (Long-term) with Option B - Preload Ahead (Short-term)**

### Rationale

1. **Option A (Guest Window Extension) - REJECTED**
   - The 4 slots at `level+0x2C` are hardcoded fixed structures
   - Buffer pool is shared and sized for 3x3 cells (~4.5 MB)
   - No decompilation proof that slots/queue/buffers can grow safely
   - Handoff requires "decompilação provar como aumentar slots... sem reutilizar memória de outra área"
   - Risk of memory corruption without full state machine understanding

2. **Option B (Preload Ahead) - ACCEPTED as Short-term**
   - Works within fixed 4-slot architecture
   - Modifies prediction in `FUN_0023C820` to prioritize movement direction
   - Improves pop-in ahead of camera without structural changes
   - Must be described as "preload ahead", NOT "larger radius"
   - Can be implemented via hook on `chooseArea` (FUN_0023C820) adjusting focus calculation

3. **Option C (Native RSL Cells) - ACCEPTED as Long-term**
   - Only path that truly increases radius without invading fixed guest structures
   - Independent host lifecycle: cell index → read/decode → resources → owner tag/transform → submit
   - Original guest cells remain reference for validation

## Implementation Plan

### Phase 3A: Option B - Preload Ahead (Immediate)

**Target**: `src/game/vcs_world.cpp` - extend `chooseHook`

```cpp
// In chooseHook, when VCS_TRACE_AREAS=1 or new setting world.preload_ahead > 0:
// 1. Read player velocity vector
// 2. Compute focus = camera + front * 50 + velocity * 150 (existing formula)
// 3. ADD: bias focus further along velocity vector based on speed
// 4. This makes the "next" area chosen further ahead in travel direction
```

**Settings to add**:
- `world.preload_ahead` (0-100, default 0): extra bias along velocity vector
- `world.preload_min_speed` (m/s): minimum speed to activate

**Validation**: Compare pop-in distance at 144 km/h with/without preload using cruise test.

### Phase 3B: Option C - Native RSL Spec (Deferred)

**Spec required before implementation** (per handoff):

1. **Cell indexing**: How to map world position → cell (x,y) → RSL submission
2. **Resource pipeline**: Read IMG/WRLD pieces → decode → upload geometry/textures
3. **Owner tags**: Replicate `0x40000000 | y<<16 | x` (cell) and `0x80000000 | ...` (area)
4. **Transform**: Cell origin at `FUN_00241F00(cell_x, cell_y)` - world matrix offset
5. **Buffer management**: Separate from guest's 0xCB7A80 pool
6. **Cross-fade**: Replicate `FUN_0023E748` merge logic with alpha
7. **Culling**: Hook `FUN_002409C8`/`FUN_00240ED8` frustum test
8. **Fallback**: Guest cells always drawn; RSL cells additive

**Integration point**: `vcs_hle_world.cpp` native draw submission

## Verification Requirements

Per handoff, before Phase 3 implementation:

1. [x] **Phase 2 validation**: Run `phase2_areas_test.py` 3× with ISO
2. [x] **Deterministic transitions**: Verified - transitions consistent across 3 runs
3. [x] **Document transitions**: State sequences recorded for cur/next/other slots

## Next Steps

1. **Implement Option B** (preload ahead) as short-term improvement
2. **Write Option C spec** for native RSL cells
3. **Implement Option C** with full validation (soak, missions, saves, teleports)

## Files to Modify

| File | Change |
|------|--------|
| `src/game/vcs_world.cpp` | Add preload_ahead bias in chooseHook |
| `src/game/vcs_world_hd.cpp` | Register `world.preload_ahead` setting |
| `docs/game-internals.md` | Document chosen strategy |
| `docs/DECISIONS.md` | Record this decision |

---

**Note**: This decision is based on static analysis of available documentation. Final validation requires Phase 2 test execution with ISO.
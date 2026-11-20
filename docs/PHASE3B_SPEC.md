# Phase 3B: Option C - Native RSL Cells Specification

**Date**: 2026-11-09
**Status**: LDM decoder implemented and compiling; integration pending
**Depends on**: Phase 3A complete (preload_ahead validated)

## Goal

Implement native RSL (Render Submission Layer) cells to extend the detailed city radius beyond the fixed 4-slot guest architecture. This is the only path that truly increases the radius without invading fixed guest structures.

## Current Status (2026-11-09)

### ✅ Completed
- **LDM decoder** (`src/game/vcs_ldm.cpp/.h`): Native C++ decoder ported from `tools/mdlinfo.py`
  - Parses `ldm` resource format (header, relocations, nodes, elements, geometry)
  - Decodes PS2 DMA data (UNPACK V3-16 positions, V2-8 UVs, V4-5 colors, V3-8 normals, V4-32 skin weights)
  - Triangle strip → triangle list conversion with shared vertex handling
  - Material/texture association
  - Creates GPU-ready buffers (vertices, indices, per-material batches)
- **Build integration**: Added to CMake glob, compiles successfully
- **Game runs**: Decoder linked into `build\vcs.exe`, game runs at 60fps

### 🔄 In Progress
- **Host cell manager** (`vcs_hle_world.cpp`): Not yet implemented
  - Cell indexing (world pos → cell grid)
  - Async piece streaming from ISO
  - Resource upload to GPU (vertex/index buffers, textures)
  - Owner tag/transform replication
  - RSL submission integration

### 📋 Pending
- Texture upload pipeline (CLUT + mipmaps)
- Cross-fade replication
- Fallback handling
- Settings: `world.rsl_cells_radius`, `world.rsl_stream_budget_mb`

## Architecture Overview

### Current Limitation
- Guest has exactly **4 fixed area slots** at `level+0x2C` (0xA0 bytes each)
- 3 active pointers: `+0x248` (current), `+0x250` (next), `+0x258` (other)
- Shared buffer pool: `0xCB7A80..0x112A660` (~4.5 MB)
- Native renderer already submits draws via RSL (`vcs_hle_world.cpp`)

### Solution: Host-Managed Extra Cells

Host maintains independent cell lifecycle:
```
Cell Index → Read IMG/WRLD → Decode Geometry → Upload Resources → Owner Tag/Transform → Submit via RSL
```

Guest cells remain the reference (drawn normally). RSL cells are **additive** - drawn in addition to guest cells.

## Detailed Specification

### 1. Cell Indexing

**Map world position → cell (x,y)**:
```cpp
// From FUN_00241F00 / game-internals.md 9h
void gridToWorld(int gx, int gy, float &ox, float &oy) {
    ox = (gx + 0.5f - (gy & 1) * 0.5f) * 125.0f - 2400.0f;
    oy = (gy + 0.5f) * 108.25f - 2000.0f;
}

void worldToGrid(float x, float y, int &gx, int &gy) {
    gy = static_cast<int>((y + 2000.0f) * 0.009237875f); // 1/108.25
    gx = static_cast<int>((x + 2400.0f) * 0.008f + (gy & 1) * 0.5f); // 1/125, hex offset
}
```

**Cell coverage**: ~125 x 108.25 units. 3x3 = ~260m radius. Target: 5x5 or 7x7 for 400-550m.

### 2. Resource Pipeline

**Step 1: Locate piece in WRLD image**
- Level piece index: `*(level + 0x1C) + 4 + gy * 8` (base + row stride)
- Each piece: 0x20 bytes, `+0` 'DLRW', `+8` bytes read, `+0xC` bytes kept, `+0x18` offset in image
- Piece size: 110-220 KB apart in image

**Step 2: Read piece data**
- Use existing ISO mapping (`src/game/vcs_memory.cpp` streams from ISO)
- Read piece at offset → decode geometry

**Step 3: Decode geometry** ✅ **IMPLEMENTED**
- VCS model format (LDM): nodes, elements, PS2 meshes
- Use `vcs::ldm::Decoder` to parse ldm bytes
- Extract: vertex buffers, index buffers, materials, textures

**Step 4: Upload to GPU**
- Create vertex/index buffers in RHI (D3D11/Vulkan)
- Upload textures (CLUT + mipmaps)
- Create draw descriptors for RSL submission

### 3. Owner Tags

**Must replicate guest tagging exactly** (from game-internals.md 9h):

| Tag | Meaning |
|-----|---------|
| `0x40000000 | (gy << 16) | gx` | Cell piece (3x3 around camera) |
| `0x80000000 | area_id` | Area piece (larger chunks) |
| `0x8000` in sector record | Hidden (low-detail replaced by detailed) |

**Host cells use same scheme**: tag = `0x40000000 | (gy << 16) | gx` for proper low-detail hiding.

### 4. Transform

**Cell origin** (from FUN_00241F00):
```
world_origin = cell_origin - camera_origin
```
Guest renderer moves world origin per cell (`0x6FE980` matrix). Host must apply same transform when submitting RSL draws.

### 5. Buffer Management

**Separate from guest's 0xCB7A80 pool**:
- Host allocates own vertex/index buffer pool
- Texture pool separate from guest streaming heap
- Budget: ~50MB for extra cells (configurable)

### 6. Cross-Fade

**Replicate FUN_0023E748 merge logic**:
- Guest cross-fade timer: `level + 0x264` counts down
- During cross-fade: merge current + previous cell lists by sector ID
- Alpha fade for sectors in only one list
- Host cells: same logic - when guest swaps, fade in host cells over ~1.1s (p50)

### 7. Culling

**Hook frustum test** (`FUN_002409C8`, `FUN_00240ED8`):
- Sector record: center (half-float), radius (half-float)
- Test against camera frustum planes
- Host cells: same culling using sector bounds

### 8. Fallback

**Guest cells always drawn**. RSL cells are **additive**:
- If host cell fails to load → guest low-detail remains visible
- If host resources exhausted → stop adding cells, guest continues
- Never break guest rendering

## Integration Points

### File: `src/render/vcs_hle_world.cpp`
- Extend `HleWorld::submitFrame()` to include host cells
- Add `HostCellManager` class with:
  - `update(camera_pos, velocity)` - determine visible cells
  - `streamPieces()` - async piece reading/decoding
  - `submit(rsl)` - submit draws to RSL

### File: `src/game/vcs_memory.cpp`
- Extend ISO streaming to support host piece reads
- Add `readPieceAtOffset(offset, size)` for host use

### File: `src/game/vcs_world.cpp`
- Add `world.rsl_cells_radius` setting (0-3 rings)
- Coordinate with `world_hd.cpp` settings

## Settings

| Setting | Range | Default | Description |
|---------|-------|---------|-------------|
| `world.rsl_cells_radius` | 0-3 | 0 | Extra rings of RSL cells (0=guest only) |
| `world.rsl_stream_budget_mb` | 10-200 | 50 | GPU memory budget for RSL cells |
| `world.rsl_crossfade` | 0/1 | 1 | Enable cross-fade for RSL cells |

## Validation Plan

1. **Unit test**: Piece read → decode → upload → draw for single cell
2. **Visual test**: Cruise at 144 km/h, compare pop-in distance (screenshots + distance_check)
3. **Soak**: 3 hours autodrive, verify no leaks/crashes
4. **Fallback**: Kill host streaming mid-run, guest continues correctly
5. **Cross-fade**: Verify smooth transition at area boundaries

## Dependencies

- [x] Phase 3A complete (preload_ahead working)
- [x] Native LDM decoder (`src/game/vcs_ldm.cpp/.h`)
- [ ] Texture upload pipeline (CLUT + mipmaps)
- [ ] RSL integration in `vcs_hle_world.cpp`
- [ ] Host cell manager with async streaming

## Timeline Estimate

| Task | Effort |
|------|--------|
| Native LDM decoder | ✅ Done |
| Texture upload pipeline | 2 days |
| Host cell manager | 3 days |
| RSL integration | 2 days |
| Cross-fade/fallback | 1 day |
| Testing/soak | 2 days |
| **Total** | **~10 days** |

---

**Next Step**: Implement texture upload pipeline and host cell manager in `vcs_hle_world.cpp`.
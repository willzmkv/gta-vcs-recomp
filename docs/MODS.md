# PC mods in `Mods/` and their native equivalents

`Mods/` (kept locally, ignored by git) holds **GTA Vice City (PC)** mods - not San Andreas, not PS2: `.asi` /
`d3d8.dll` / `rwd3d9.dll` plugins that hook `gta-vc.exe` and its Direct3D 8/9 RenderWare. None of them can load here:
vcs.exe is VCS (PS2) recompiled, drawing through its own D3D11 / Vulkan renderer, with RSL (Leeds' RenderWare Stories),
not PC RenderWare, and VCS's Vice City (1984) is a different map from VC's (1986). What can be ported is each mod's
**effect**, rebuilt natively as a setting (authentic by default, AGENTS.md). Never commit anything from `Mods/` (their
binaries, `.cso` shaders, textures, `.dat` files). The work list is [FORK.md](FORK.md).

| Mod | What it is |
|---|---|
| `Proper Shaders/` (Junior_Djjr, VC port by Jessica Natalia; version 07-30) | D3D9 proxy + ModLoader folder `Proper Shaders VC/`: per-pixel lit pipelines, shadows, SMAA, simulated HDR / tonemap / bloom, sun rays, PBR / simple sky, volumetric clouds, **volumetric height fog**, deferred lights, car reflections, rain on cars; `ProperShaders.ini` documents every knob |
| `SkyGFX/` (aap, 3.0b) | Makes PC III / VC look like the PS2 / Xbox: PS2 lighting, PS2 water, PS2 env-map blending, dual-pass alpha, Xbox (neo) rim / gloss / water-drop pipelines, YCbCr colour correction, Leeds radiosity |
| `Project2DFX/` (ThirteenAG; `VCLodLights.asi` + `.dat`, Open Limit Adjuster) | Draw distance for VC: `ViewDistance` (metres added to the far plane), `DrawDistance` (x LOD / object distance, 1-5), `DetailDistance` (x small objects and trees), `ShadowDistance`; distant lights (`DistantLights`, `DistantLightSize`, `DistantTraffic`, `Searchlights`) from a table of VC's lamp positions (`VCLodLights.dat`: model, colour, offset, size, distance, blink...). The limit adjuster lifts VC's pool limits (peds, vehicles, buildings, 2D effects, coronas...) |
| `Proper Ragdoll/` | Physics ragdolls for peds (gravity, joints measured on the game's animations, world collision, water), an in-game editor, impact sounds |

Texture replacement is not one of these mods: it comes from upstream PR #8 (PCSX2 PNG packs), extended here with DDS
(FORK.md T1).

## Map to vcs.exe

| Effect (mod) | In VCS Recomp | Status |
|---|---|---|
| Distance fog density / start (Proper Shaders' fog, foggy weathers) | The game's own GS fog: its range scaled at the setter (game-internals.md 6l, `src/game/vcs_fog.cpp`) | **Done** 2026-11-02 - Graphics > Fog; `tools/fog_check.py` |
| View distance, LOD / object draw distance, detail distance (Project2DFX) | VCS draws a streamed hex grid of world cells (game-internals.md 6b) under a far plane of 2000, entities against near / far at `0x4CD130` / `0x4CD12C`; what decides HD cell vs low detail and the entity distances is still to be read | T2 (research first) |
| Population and car spawn / removal distance (follows the draw distance) | `CPopulation::Update` `FUN_00430148`, `CCarCtrl::RemoveDistantCars` `FUN_00433008` (game-internals.md 12) | T2 |
| Distant lights (Project2DFX `VCLodLights`) | Coronas from VCS's own 2D effects (VC's `.dat` is the wrong map) | T2, last |
| Pool limits (Open Limit Adjuster) | Only if a larger draw distance hits VCS's pools; the guest has 32 MB | T2, when needed |
| Height / volumetric fog bank (Proper Shaders `[VolumetricFog]`) | Post pass before the HUD reading the Z buffer (target 280) + the camera (`0x520A80`) to rebuild world height | T4, on the pre-HUD pass (T3) |
| SMAA, simulated HDR / tonemap / bloom, YCbCr correction | Post pass on the 3D picture before the HUD | T5 / T6, on T3 |
| Sun rays | Post pass: sun position from the time cycle + depth occlusion | T7, on T3 |
| PS2 lighting, PS2 water, PS2 env map, dual-pass alpha, no backface culling, PS2 load screens (SkyGFX) | Already the case: the PS2's own renderer, reproduced | Nothing to do |
| Leeds radiosity / trails (SkyGFX `radiosity`, `trailsSwitch`) | VCS's own (the trails / bloom targets, game-internals.md 6g) | Already there |
| Larger reflection map (SkyGFX `envMapSize`) | The 128x128 reflection target (420, role set in `vcs_display.cpp`) | Backlog |
| Xbox rim light / gloss / screen water drops (SkyGFX neo) | Native shader variants in `vcs_hle_world.cpp` / `gs_draw.hlsl` | Backlog |
| Per-pixel lit pipelines, shadow maps, deferred lights, car paint reflections, rain on cars, PBR sky, volumetric clouds | The native renderer drawing the scene a second time (light view) and new shaders | Backlog; milestone-sized each |
| Ragdolls (Proper Ragdoll) | A physics engine driving the ped skeleton on death / falls, against VCS's collision | Backlog; gameplay, not render |
| ModLoader (`ModLoader/`: folders as mods, `modloader.ini`, an in-game menu, hot swapping) | The mod loader below: `modloader/`, the same ini, F2 > Mods, live | **Core done** 2026-11-02 (textures); game files: T1c |
| ModLoader's `.dff` / `.txd` replacement | VCS has neither format (below): its own files (`.mdl`, `.xtx` in the `.IMG` archives) through the mod loader; PC `.dff` / `.txd` only by conversion | T1c (VCS formats), T9 (PC formats, research) |
| ASI loader, CLEO | Trainer (F5) and the rig's `scm` for scripts | Not ported (no PC executable) |

## The mod loader

PC GTA's Mod Loader (LINK/2012; the copy in `Mods/ModLoader/`) made mods folders: nothing of the game's is replaced
on disc, everything is injected at run time, each mod can be switched off in a menu, and mods can be swapped while
the game runs. `src/mods/modloader.cpp` does the same for this port:

- **Where**: `modloader/` beside vcs.exe, or in the checkout's root when vcs.exe is in `build/` (`VCS_MODLOADER_DIR`
  elsewhere). Not `mods/`: on Windows that is the `Mods/` folder of PC mods kept for reference. `modloader/README.md`
  is the only file of it in git.
- **A mod** is any folder in it (a leading `.` hides one). `modloader/modloader.ini` is read in Mod Loader's own
  format, so a player's knowledge carries over: `[Folder.Config] Profile`, and the profile's `[...Priority]
  Mod=1..100` (50 by default, 0 ignores the mod; a higher one wins a file two mods have), `[...IgnoreMods]`
  (wildcards; `_ignore` when there is no file yet, as Mod Loader ships it) and `[...Config] IgnoreAllMods`. Profiles
  beyond that (parents, `UseIfModule`) are not read. The game **makes the file** in `modloader/` when it is missing
  (Mod Loader's template, every installed mod listed at 50), so it is there to see and edit.
- **Priorities in F2**: a slider (1-100) beside each mod's switch. It writes `modloader.ini` (its line replaced, or
  added to the profile's Priority section; the rest of the file kept), so the file stays the one source: the
  priorities are not in vcs.ini (`settings::Spec::saved = false`, read back with `read`), and Rescan reads an edited
  file again. Reset page to defaults puts every mod back on, at 50.
- **F2 > Mods**: Load mods (`mods.enabled`), Replace textures (`graphics.texture_replacements`, which also governs the
  packs outside `modloader/`: `VCS_TEXTURE_PACK`, `graphics.texture_pack` in vcs.ini, `textures/SLUS-21590`), one
  switch per mod (`mods.<name>`, with its file count and priority as help), Rescan (files read again) and Open folder.
  A switch is applied at the next host frame: the sources are indexed again on the main thread (~20 ms for 6279 files
  here) and `gsHwSetTextureReplacements` hands the pack to the GS stage, which swaps it at its next frame (the old
  pack's GPU textures dropped; the game's own or the new pack's come back as they are drawn). A new mod folder shows
  after a restart (the settings registry is built at start-up).
- **What a mod can hold**: texture packs so far (PCSX2-named `.dds` / `.png` anywhere in it, `dumps/` aside;
  docs/TEXTURE_PACKS.md). Game files are next (T1c).
- **Test runs stay authentic**: a hidden run loads mods only with `VCS_MODS=1`, the other packs only with
  `VCS_TEXTURE_REPLACEMENTS=1`, or both with `VCS_TEST_MODS=1` (switches left free for the `setting` command), and
  writes nothing into the player's `modloader/` - only into a folder of its own given by `VCS_MODLOADER_DIR`.
- **Checked** (`tools/mods_check.py`, 2026-11-02): a `modloader/` with no ini of "HD" (the HD pack, linked), "Tinted"
  (half of a city spot's dumped textures painted magenta), `_ignore` and `.hidden`: the game makes `modloader.ini` (HD
  and Tinted at 50, `_ignore` ignored); at 50 / 50 HD wins the shared textures (Tinted supplies its 7 own, no magenta);
  Tinted's slider to 60 (`setting mods.tinted.priority 60`) - Tinted supplies 377, magenta, `Tinted=60` in the ini;
  then Tinted off (no magenta, HD stays), Replace textures off (the game's own), on again (HD back), Load mods off -
  6 swaps, pass. With the player's own two mods linked in: "HUD Remastered" (160 PNG, 152 of them in the HD pack too)
  supplies 8 textures at 50 / 50 (the DDS wins a tie) and all 160 at 60, the HD pack ceding its 152.

### Game files: GTA3PS2.IMG entry by entry (T1c, 2026-11-03)

As the player suggested: no archive is rebuilt or replaced - a mod holds only the entries it changes, and the game reads
those from the mod. VCS's model archive has no directory on the disc (game-internals.md 9d): the game's own table gives
each entry its sectors, and its model infos keep each name's key, so a file named after a model finds its entry.

- **In a mod**: `<model name>.mdl` (`admiral.mdl`), `<texture dictionary>.xtx` (`admiral.xtx`: the TXD store's
  names) or `#<streaming index>.<anything>` (`#1234.bin`) for an archive entry, and a loose disc file by its own name (`AMERICAN.GXT`, `ENGLISH.GXT`, `MAIN.SCM`), anywhere in the mod's folder. The priority slider decides between two mods with the same entry. **Any size**: a smaller file is
  padded with zeros; a larger one has its record moved past the end of the disc (the streaming info's offset and
  size, LSN 0x180000 on, and the archive's size raised to reach it), where only the mod's bytes are served - the
  game allocates the mod's size and the entry touches no neighbour; the game's values come back when the mod goes.
- **How**: the mod loader hands the list to the stream layer (`vcs::setStreamFiles`); at the next read the stream
  thread resolves each name against the game's tables (the disc's file table for GTA3PS2.IMG's LSN, the model infos'
  keys, the streaming info's offset / size) and lays the mod's bytes over every disc read that covers the entry. An
  entry already in memory changes when the game streams it again. `VCS_MOD_FILES=0`: none.
- **`tools/img.py`**: `table` (the directory from a hidden run into `build/img/streaming.json`), `list`, `extract`
  (from the player's disc image into `build/img/GTA3PS2`, named `admiral.mdl` / `#1234.bin`): an extracted folder is a
  mod as it is. The entries are the game's data - never in git.
- **Checked** (`tools/gamefiles_check.py`): an ADMIRAL spawned in the street with no mod, with its own entry from a mod
  (the same picture: 0.32) and with the CHEETAH's entry as `admiral.mdl` (a Cheetah drawn as "Admiral": 1.82); and the
  text files from a mod with "Admiral" written "ADMIRAL" - the game's text for that key reads "ADMIRAL" - pass.
  And the ADMIRAL's entry (37 sectors) as `cheetah.mdl` (30): a CHEETAH spawned draws the Admiral's body, read from
  past the disc. A loose file larger than the disc's moves the same way (its file-table record).

### `.dff` / `.txd`: what PC mods replace, and what VCS has

PC GTA III / VC / SA mods are RenderWare 3 files: `.dff` (a clump: frames, geometry, materials, skin) and `.txd` (a
texture dictionary), loaded by the PC executable's RenderWare and streamed from `gta3.img`. **VCS on the PS2 has
neither**: its models are Leeds' RenderWare Stories (RSL) structures with PS2-native geometry (VU1 packets, game-
internals.md 6d), in `GTA3PS2.IMG` and the world's `BEACH / MAINLA / MALL.IMG` + `.LVZ`; its textures are `.XTX`
(GS formats, section 7). A PC `.dff` / `.txd` dropped into the game would not be read. Two different things can be
built:

1. **VCS's own files from a mod** (T1c) - what Mod Loader's `std.stream` does on PC, for VCS's formats: a file in a
   mod named as an archive entry (or a loose file: `MAIN.SCM`, the `.GXT` texts) is read in place of the disc's. The
   disc reads are native already (`src/game/vcs_stream.cpp`), so the bytes can come from the mod's file; what has to
   be found first is each archive's directory (entry names, offsets, sizes - where VCS keeps them: `GAME.DTZ`?) and
   what the game assumes about an entry's size (buffers sized from the directory). Mods made with VCS / LCS
   modding tools for the PS2 / PSP versions would then work.
2. **PC `.dff` / `.txd` themselves** (T9, research): either converted to VCS's formats ahead of time (RW 3 clump ->
   RSL structures with PS2 strips and VU1 data, TXD -> XTX), or loaded on the host and drawn by the native renderer in
   place of a model the game submits (needs draws at the RSL level: docs/RSL_SUBMISSION.md, milestone 7). Either is a
   project of its own; the texture half is already possible today as a texture pack (textures are matched by content,
   not by file).

### T9 findings: PC VC car mods in VCS (2026-11-03)

- **The tutorial** the player pointed to (MixMods, vehicles in GTA SA PS2:
  https://forum.mixmods.com.br/f250-tutoriais/t1411-gta-ps2-veiculos) sits behind a Cloudflare JavaScript check; the
  player pasted the pages into a text file (kept locally in `Mods/`, not in git). It holds aap's "About SkyGFX" and the
  forum's follow-up thread of tips for converting PC cars to SA PS2 (the t1411 steps themselves are not in it). What
  carries over to VCS:
  - **Keep the original car's collision**: converted cars with their own collision float or sink; the fix is the
    replaced car's collision put back (RW Analyze). In VCS the mod loader replaces only the `.mdl`: the collision,
    handling and model info stay the VCS car's - the same choice, by construction.
  - **Textures as PS2 paletted rasters (PAL4 / PAL8, Magic.TXD)**: unpaletted ones weigh far more in the game and
    flicker. VCS's own are mostly PSMT4 + CLUT (game-internals.md 6e). For the converter: either quantise a `.txd`'s
    DXT textures to PAL4 / PAL8 rasters in the vehicle's TXD (authentic, lossy), or hand them to the renderer by name
    through the texture packs (no quantisation, no PS2 memory limits) - the second first.
  - The tutorial itself (DanielSantos, pasted later; a local copy as `Mods/Tutorial_GTA-PS2_Veiculos.md`, not in git):
    PC car -> SA PS2 is the `.txd` to PS2 format (Magic.TXD), the `.dff` through aap's RenderWare File Converter
    (`convdff`, the original mesh or - better - tristrips), the collision rebuilt as COL1 and put back into the `.dff`
    (COL Editor, HxD, RWAnalyze), and the files put into `gta3.img` / `gta3_1.img`; models over ~600 KB slow the game.
    `convdff` writes RenderWare's PS2 geometry - SA's format, not VCS's RSL `ldm` - so its output does not load here;
    the port's converter writes `ldm` (tristrips too), and the mod loader spares the collision and IMG steps.
  - aap: RSL is "at the high level a simplified copy & paste of RW", "at the lower levels completely different" - the
    node / element / geometry structure maps from a `.dff`, the PS2 mesh data has to be built anew. VCS's vehicle
    reflections: an environment map rendered from the camera with coronas, alpha blended (LCS adds it).
- **`tools/rwinfo.py`** reads PC RenderWare files on the host (stdlib only; `--self-test` writes and reads a small
  clump and texture dictionary): a `.dff`'s frames with their names and parents, geometries (vertices, triangles,
  materials and texture names; PS2-native flagged), atomics; a `.txd`'s textures (name, size, D3D8 DXT / palette /
  D3D9 FourCC, levels). Checked on the player's VC mods (`Mods/Fixed_Xbox_Vehs_1.3`: `comet.dff`, 33 frames and 6
  geometries; `comet.txd`, 256x256 DXT1 with 9 levels and 8-bit palettes) - read only.
- **The match**: a VCS vehicle's `.mdl` (game-internals.md 9d) carries the very frame names of a VC car's `.dff`
  (`chassis_dummy`, `chassis_hi` / `_vlo`, `wheel_*_dummy`, `door_*_dummy`, `bonnet` / `boot` / `bump_*` /
  `windscreen` / `wing_*`), so a VC car's parts line up with what VCS's vehicle code looks for. Its textures have names
  too, in the same style (`admiral868bit_sl` vs VC's `comet868bit128`). VC car mods also come in Mod Loader's folder
  layout (`gta3.img/comet.dff`).
- **The routes**:
  - (c) a `.txd`'s textures by name over a VCS vehicle's: only for skins made for VCS's own models - VCS remodelled
    its cars (other UV layouts), so a VC car's textures on a VCS model would map wrong. Not the way to reuse VC cars.
  - (a) **convert a `.dff` (+ its `.txd`) into a VCS `ldm` vehicle resource** offline (a `tools/dff2mdl.py`): the
    result goes through T1c's game files as `<car>.mdl`, and the game's own paths draw it (the CHEETAH-as-ADMIRAL swap
    shows the game takes another `ldm`). **Chosen**: self-contained, the game's own paths, and T1c already carries it.
  - (b) load the `.dff` on the host and draw it in place of the model's atomics: no PS2 limits, but it needs draws at
    the RSL level (docs/RSL_SUBMISSION.md) and the vehicle code's frame logic mirrored. Kept as the alternative.
- **Route (a) done** for cars: below.

### PC VC cars in VCS: `tools/dff2mdl.py` and `tools/txd2xtx.py` (T9, 2026-11-03)

A VC car's `.dff` and `.txd` become a VCS vehicle's two archive entries, `<vcs car>.mdl` and `<vcs car>.xtx`. **The
mod loader does it itself**: a VC car mod put into `modloader/` as it comes (`comet.dff` + `comet.txd`, in any
subfolder - Mod Loader's `gta3.img/` layout too) replaces VCS's car of that name, converted while the game loads
(`src/mods/rwconv.cpp`: the Comet in 21 + 55 ms; the 80 cars of `Fixed_Xbox_Vehs_1.3` in 4.1 s at boot). A `.txd` is
taken only beside a `.dff` of its name. The same conversion as tools, to look at a result or make VCS files by hand:

```
python tools/img.py extract                  # once: the disc's vehicles into build/img/GTA3PS2 (the templates)
python tools/dff2mdl.py convert comet.dff build/img/GTA3PS2/admiral.mdl "modloader/VC Comet/admiral.mdl" --txd comet.txd
python tools/txd2xtx.py convert comet.txd "modloader/VC Comet/admiral.xtx"
python tools/carmod_check.py                 # the Comet from Mods/Fixed_Xbox_Vehs_1.3 as the ADMIRAL, in the game
```

- **The model** (`dff2mdl convert DFF TEMPLATE OUT [--txd TXD]`): the VCS vehicle replaced is the template - its
  handling, collision, wheels, sounds and model info stay (the tutorial's advice, by construction).
  - **Nodes**: each VCS node with a VC frame of its name is put where that frame is relative to the root, whatever the
    trees in between (`wheel_*_dummy`, `door_*_dummy`, `brakelights` ...); the root itself is the vehicle's place.
  - **Parts** (each element gets VC parts): the part of its node's name (`chassis_hi`, a bike's `forks_front` /
    `wheel_front` / `handlebars`, a boat's `boat_hi` and flaps, `extra1`); an `X_dummy`'s intact element (object flag
    4) `X_hi_ok`, its damaged one `X_hi_dam`; a helicopter's `static_rotor(2)` VC's `toprotor` / `rearrotor` (VCS's
    `moving_rotor` blur discs stay theirs: the game fades them in with the speed). VC parts left over: damaged ones,
    extras and LODs go; the rest - the wings VCS builds into its chassis, small parts - join `chassis_hi` (or a boat's
    hull) where they are. Elements no part reaches are emptied (one degenerate triangle): VCS-only pieces (`car_parts`,
    `part1`, `glass_hi`, a van door's second piece) would show VCS geometry with textures the new dictionary lacks.
  - **Meshes**: the triangles from the bin mesh (exporters leave the triangles' own material numbers at 0), strips
    started at the triangle with the fewest free neighbours and grown both ways, one PS2 mesh per material (and per
    UV repeat when a material spans more than two: the 8-bit UVs keep the game's precision), encoded as the game's own
    (`--self-test`: 3466 / 3466 byte for byte), with each mesh's and the geometry's int16 bounding box (game-internals
    9d). NaN in a `.dff` reads as 0.
- **Materials**: one per VC material, with VC's texture name (found in the `.xtx` below) and colour; VC's paint colours
  (`3CFF00` first, `FF00AF` second) put the material into the vehicle's paint lists - the game colours those with the
  car's colours - and lend it the template's paint effects (the reflection). A texture the `.txd` lacks maps to VCS's
  shared one where there is one (`vehiclegeneric256` -> `xv_vehiclegeneric_sl`, ...) or goes (the material's colour
  alone).
- **The textures** (`txd2xtx convert TXD OUT [--max 128] [--keep VCS.xtx]`): D3D8 textures (DXT1 / 3 / 5, 8-bit
  palettes, 8888 / 888 / 1555 / 565 / 4444 / LUM8) decoded, halved down to 128 pixels (VCS's own vehicle textures are
  128 at most) and quantised (median cut) to 4 bit when nothing is lost, else 8 bit - the format of every texture on
  the disc. The VCS dictionary's own textures the `.txd` has no name for follow, as they are (a helicopter's `rotor`
  blur). `info FILE.xtx --png DIR` shows a dictionary's textures (the disc's too); `tools/img.py extract
  build/img/GTA3PS2 "*"` writes the vehicles' models and dictionaries (`admiral.mdl`, `admiral.xtx`).
- **The mod loader** takes `<texture dictionary>.xtx` by the TXD's name (the game's TXD store keeps the names; a
  vehicle's is the vehicle's name), as `<model>.mdl` by the model's. VC names VCS shortened map to VCS's: `boxville`,
  `enforcer`, `fbiranch`, `police`, `sabretur`, `washing` -> `boxvil`, `enforce`, `fbiran`, `policem`, `sabretb`,
  `washin`. A VC vehicle VCS does not have (Infernus, Landstalker ...) is named in the log: rename its files as the
  vehicle it should replace. VC's `wheels.dff` / `.txd` are not used yet: VCS's wheel models are resident (T10).
- **Checked** (`tools/carmod_check.py`): the ADMIRAL with no mod, then the Comet in its place (both entries larger than
  the disc's: 292 KB model, 165 KB textures) - spawned in the street, the player in it, from behind: the VC Comet with
  its own textures, the game's paint, no crash.
- **In the game, as they come** (`carmod_check.py --direct`, `--folder`): the Comet's `.dff` / `.txd` named
  `admiral.*` - converted, resolved, the same picture; the whole `Fixed Xbox Vehicles/gta3.img` folder as one mod
  (160 files converted, none failed): the VC Xbox Comet, Cheetah, Taxi, Voodoo, Stinger, Mesa and PCJ-600 spawned,
  each VC's model with its textures. The C++ and Python converters agree byte for byte on all 236 `.dff` / `.txd` of
  the player's mods (`tools/rwconv_check.py`).
- **Checked triangle by triangle** (`tools/carmod_verify.py FOLDER [--native]`): both cars put together as drawn -
  the VC one from its frames, the converted one from its node tree and decoded PS2 meshes - intact and damaged; every
  VC triangle must be at its place (5 mm) with its texture and UVs, and nothing else may be there. 67 / 67 vehicles of
  `Fixed_Xbox_Vehs_1.3` (both converters), and the in-game pictures from four sides (`carmod_check.py --views`, the
  rig's `campose` free-camera pose) for cars, a bike, boats and helicopters.
- **Not yet**: VC's wheels (`wheels.dff`, resident in VCS); VC extras where VCS has no extra element of that name;
  memory - a VC car with its textures is 4-5x a VCS car, and VCS streams into a fixed 5.4 MB heap (game-internals.md
  9e): loading many different VC cars at once can exhaust it. T10 (issue #15) gives the port more memory.

## The pre-HUD pass (the infrastructure T4-T7 need)

Most of Proper Shaders' post effects need the finished 3D picture **before the HUD and menus** are drawn over it, plus
its depth. `present.hlsl` runs on the final picture (HUD included), so it can only do whole-picture colour (the
existing Exposure). The frame's structure (game-internals.md 6a / 6g) has a point where the world is done and 2D begins
(`vcs_hud.cpp` sees the 2D at the RenderWare level); a pass inserted there with the frame (target 140) and Z buffer
(target 280) bound is what volumetric fog, SMAA, tone mapping and sun rays build on.

## History

A first attempt took the mods for San Andreas ones, catalogued a pack that is not in `Mods/`, and drew fog as a
screen-height gradient over the finished picture; its four `MODS_*.md` files were never committed (DECISIONS.md
2026-11-02).

# AGENTS.md - working on the GTA: Vice City Stories native port

Read this first. It is the contract for anyone (human or AI agent) changing this repository. The
detailed knowledge lives in `docs/`:

| Doc | What's in it |
|---|---|
| [docs/game-internals.md](docs/game-internals.md) | Everything known about how the game works inside: code map, boot, camera, renderer, VU1 pipeline, 2D, streaming, sound, memory card - with addresses and how each fact was found |
| [docs/NATIVE_PORT_ROADMAP.md](docs/NATIVE_PORT_ROADMAP.md) | The plan to completion (milestones 1-8) and what "done" means |
| [docs/PROGRESS.md](docs/PROGRESS.md) | Status per area, open bugs, and the table of debug switches (`PS2X_*`, `VCS_*`) |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Why things are the way they are; the user's decisions |
| [docs/PS2RECOMP_PATCHES.md](docs/PS2RECOMP_PATCHES.md) | Every change to the vendored PS2Recomp runtime, with the reason |
| [docs/NATIVE_RENDERER.md](docs/NATIVE_RENDERER.md) | The D3D11 renderer: layers, stages, HLE method |
| [docs/SETTINGS_MENU_WIDESCREEN.md](docs/SETTINGS_MENU_WIDESCREEN.md) | Settings registry / menu, Hor+ widescreen |
| [docs/FULLY_NATIVE.md](docs/FULLY_NATIVE.md) | The steps left until a normal frame is fully native, with their status - the working list |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | Frame rate well past 100 fps everywhere: how it is measured, what was done (numbers), what is next |
| [docs/RSL_SUBMISSION.md](docs/RSL_SUBMISSION.md) | Plan: render submission at the RSL level (milestone 7) - draws from the game's render calls instead of DMA packets |
| [docs/DISC.md](docs/DISC.md), [docs/CONTROLS.md](docs/CONTROLS.md) | Disc layout; keyboard / gamepad mapping |
| [docs/TRAINER.md](docs/TRAINER.md) | The trainer (F5): what it does, how (the game's script commands), its check |
| [docs/LINUX.md](docs/LINUX.md) | The Linux build: packages, flags, the Vulkan backend, what differs from Windows, the work list |
| [docs/LAUNCHER.md](docs/LAUNCHER.md) | The launcher and releases: what a release holds, the player's flow, the tools behind each button, the `@vcs` events, what was checked |
| [docs/FORK.md](docs/FORK.md) | This fork: what it changes from upstream, the work list (tasks T1-T8 with their steps and "done when"), how the work is done |
| [docs/MODS.md](docs/MODS.md) | The PC mods in `Mods/` (Proper Shaders VC, SkyGFX, Project2DFX, Proper Ragdoll): each effect mapped to the game, done or which task |

**When you learn something about the game, write it into docs/game-internals.md in the same change.**
Record addresses and how you found them. When something is weird, use judgment, fix or work around it,
and write it down (game-internals for the game, PS2RECOMP_PATCHES for the runtime, DECISIONS for
choices).

## What the project is

- GTA: Vice City Stories (PS2, SLUS-21590 USA) statically recompiled to C++ (PS2Recomp, vendored and
  heavily modified in `third_party/PS2Recomp`), turned step by step into a native port: native D3D11
  rendering of the game's models (`src/game/vcs_hle_world.cpp`), SDL3 platform layer, settings menu.
- Windows and Linux (docs/LINUX.md: clang, Vulkan; a plain host build). macOS later is possible (SDL3 + the RHI); the
  Steam Deck is postponed.
- Distribution is source-only, bring your own disc. **Never commit game code or data**: no ISO, no
  ELF, no generated C++ (`generated/`), no disassembly listings, no extracted assets. Analysis dumps go
  to `build/analysis/` (ignored). Function maps (names + addresses) and docs describing behaviour are
  fine.
- Authentic by default; every enhancement is an option (settings registry, `src/settings`).

## Layout

| Path | What |
|---|---|
| `src/main.cpp` | Launcher: ISO, settings registration, runtime start |
| `src/game/` | Game-specific code: overrides / hooks (`vcs_overrides.cpp`), native draw HLE (`vcs_hle_world.cpp`), display (`vcs_display.cpp`), the trainer (`vcs_trainer.cpp`), frame timing (`vcs_framerate.cpp`, per-frame mission meters `vcs_script_timing.cpp`), the player's controls (`vcs_input.cpp`; free aim `vcs_aim.cpp`), the game's music / effects levels as settings (`vcs_volume.cpp`) |
| `src/render/`, `src/rhi/`, `src/platform/` | Presenter + shaders (HLSL; DXBC by fxc, SPIR-V by `tools/hlsl_spirv.py`), the RHI (backends `d3d11/`, `vulkan/`, `null/`; only they see a graphics API), SDL3 platform layer |
| `src/sound/` | Sony's libsd on the host (the native sound driver in `src/game/vcs_sound.cpp` drives the SPU2 through it) |
| `src/settings/`, `src/ui/` | Settings registry + vcs.ini, the in-game settings menu (F2), the trainer menu (F5) |
| `src/mods/` | The mod loader: `modloader/` folders as mods (Mod Loader's `modloader.ini`), F2 > Mods, live switches (docs/MODS.md) |
| `src/debug/` | Crash handler, watchdog, sampling profiler, self-test, memory watch, the F3 game-state panel (`game_info.cpp`, also `gamectl.py gameinfo`) |
| `third_party/PS2Recomp/ps2xRuntime` | The runtime (EE, VIF, VU1, GS HW backend, IOP host, stubs); our changes marked `[gta-recomp]` |
| `game/vcs/` | Recompiler config, function map, VU1 upload list |
| `tools/` | Python 3 tools (standard library only) - see below |
| `launcher/` | `VCSLauncher.exe` / Linux `VCSLauncher` (a CMake project of its own, no third-party code: its own window - Win32, or X11 / Wayland -, renderer - D3D11 or Vulkan, one `ui.hlsl` -, DirectWrite / FreeType text, its own icons, animated UI, after uppies): disc, build tools, build, play - a front end over `tools/` (docs/LAUNCHER.md) |

## Build and run

```
python tools/build.py                       # rebuild after code changes (--reconfigure after adding files)
python tools/build.py --recompile [ISO]     # disc -> generated C++ -> build (slow)
python tools/build.py --iop-tests           # SPU2 unit tests (reverb, interpolation, noise, PMON)
python tools/build.py --quick               # no LTO (= --opt none): fast relinks; kept until --opt lto
python tools/build.py --check-deps          # Linux: missing packages and the command that installs them
python tools/build.py --opt none|lto|pgo    # game optimisation, kept until changed (lto: the default; pgo: ThinLTO + profile, trains if needed)
python tools/bench.py [OUT]                  # game-thread benchmark: fixed city spots + an AI drive (game / worker ms)
python tools/cruise.py run [OUT] [ENV=VAL]   # stutter rig: the car along a fixed route at 144 km/h, every frame timed;
                                             # hitches with their texture / main-thread / game-thread cause (PERFORMANCE.md 21)
python tools/clean.py [--apply] [--keep-days 7]   # delete test output in build/ (lists first; builds, saves, baselines kept)
python tools/build.py --launcher             # the launcher (VCSLauncher(.exe) in the root; no disc, ~20 s the first time)
python3 tools/build.py --install-deps        # Linux: the missing packages through pkexec (what the launcher's button runs)
python tools/build.py --recompile-if-needed  # recompile only when generated/ is stale (generated/recompile_stamp.json)
python tools/doctor.py [--json]              # folder, compiler, disc, build state, the next step (the launcher reads --json)
python tools/build_dir.py [check|set P|reset] # the build folder elsewhere (.vcs-build-dir, VCS_BUILD_DIR): tc.BUILD follows;
                                             # the test tools keep writing to build\ in the source folder
python tools/release.py [--ref vX | --worktree]   # the Windows release zip (build/dist); refuses game files. The number is
                                             # VERSION (root; 0.2.0): bump it before tagging - the tag must match;
                                             # the notes: docs/releases/<tag>.md (else GitHub generates them)
python3 tools/release.py --platform linux [--worktree --allow-new-glibc]   # the Linux AppImage (CI builds it on
                                             # Ubuntu 22.04 for glibc 2.35; a v* tag publishes zip + AppImage together)
python tools/deps.py list|fetch|check        # the libraries CMake downloads; deps/ makes the build offline
python tools/backup.py create|list|restore   # memory card backups (VCS_DATA_DIR honoured: test on copies)
python tools/setup_vs.py status|install [--dry-run]   # Visual Studio Build Tools (+ clang) from Microsoft, elevated
python tools/update.py check|apply [--zip F]  # a release folder to the newest GitHub release (not a clone)
python tools/rip_disc.py list|rip E: OUT.iso  # a PS2 disc in a DVD drive to an .iso; report.py zips logs for bug reports
python tools/framerate_audit.py [--steps 15,30,60,100]   # behaviour per simulated second at forced time steps
python tools/run.py 60 name                 # run 60 s with the window, log to build/run/name.log (0 s: until it exits; --hidden for tests)
python tools/run.py 60 name --control       # + control channel for tools/gamectl.py
python tools/scenes.py capture OUT          # reference scenes from an empty card (screenshots + perf)
python tools/scenes.py compare REF NEW      # picture / perf diff of two captures
python tools/gamectl.py press F2|screenshot f.png|perf|quit
python tools/gamectl.py gameinfo           # player position / heading / state, vehicle, camera, clock, pauses, scripts (F3's panel)
python tools/rig.py start --save build/rig/saves/fortbaxter   # straight into gameplay from a card copy
python tools/rig.py player|scripts|peekf 0x4e4910 1|scm <hex>  # inspect / drive the running game
                                                              # (control channel: `campose x y z yaw pitch|off` = free camera there)
python tools/tour.py OUT [--every 9 --limit 18]               # world tour (quick subset with --every/--limit)
import spots; spots.put(g, "police"|"havana")                  # (tools/spots.py) the reference spots for pictures and measurements
                                                              # of the city: pavement facing the street, camera behind the player
python tools/saves.py [OUT]                                   # save / Load Game / Delete Game through the menus
python tools/soak.py [OUT] --hours 3                          # hours of autodrive: crash / hang / leak / falling
python tools/shakedown.py [OUT]                               # mission 50: cutscene + shop entry (the "Loading..." hang)
python tools/soundcheck.py all OUT [--mission N]             # native sound vs the emulated IOP: record, replay, compare
python tools/trainer_check.py [OUT] [--vehicles all]          # every trainer feature, checked in the game's memory
python tools/finale_check.py [OUT] [--story] [--full]         # Last Stand's end from the trainer: final cutscene, credits
python tools/controls_check.py [OUT]                          # bindings, presets, reset, vibration, volume levels; menu screenshots
python tools/hud_check.py [OUT]                               # HUD at the screen edges: radar left, centred text in the middle
python tools/fog_check.py [OUT] [--save DIR]                  # Graphics > Fog: the game's fog range scaled, untouched at 100 %
python tools/distance_check.py [OUT] [--save DIR]             # Graphics > Draw distance: the entities' LOD scale, more drawn at 300 %
python tools/texture_check.py [OUT] [--pack DIR] [--loader-only] [--validate-pack] [--dump-check]   # texture packs (DDS / PNG)
python tools/mods_check.py [OUT] --pack DIR                    # the mod loader: modloader.ini, priorities, F2 > Mods switched live
python tools/img.py table|list|extract [OUT] [names]          # GTA3PS2.IMG entry by entry (the game's directory; extracts stay in build/)
python tools/gamefiles_check.py [OUT]                         # a GTA3PS2.IMG entry replaced from a mod (admiral.mdl), in the game
python tools/rwinfo.py FILE.dff|FILE.txd | --self-test         # what a PC RenderWare .dff / .txd holds (frames, geometry, textures)
python tools/mdlinfo.py FILE.mdl [--obj F] [--png F] | --vehicles   # a VCS model (ldm): nodes, elements, PS2 meshes decoded
python tools/dff2mdl.py convert CAR.dff TEMPLATE.mdl OUT.mdl [--txd CAR.txd] | --self-test   # a VC car onto a VCS vehicle
python tools/txd2xtx.py convert CAR.txd OUT.xtx | info F.xtx [--png DIR]   # a VC .txd -> VCS texture dictionary (4/8 bit)
python tools/carmod_check.py [OUT] [--dff F --txd F --vehicle admiral --model 171] [--direct] [--folder DIR --models 266,172]
                                                              # VC cars in the game (converted ahead, or by the mod loader)
python tools/rwconv_check.py [FOLDER]                        # the mod loader's .dff/.txd converters = the Python tools, byte for byte
python tools/carmod_verify.py FOLDER [--native]               # converted VC vehicles vs their .dff, triangle by triangle (intact + damaged)
python tools/weaponmod_check.py [OUT] [--folder DIR]          # a PC weapon mod in the game: each weapon in Vic's hand (front, side), the HUD icon
python tools/weaponfit.py --vc VC/models/gta3.img [--check]   # where VCS holds each weapon against VC: the converters' WEAPON_PLACES
python tools/fire_check.py [OUT] [--variant NAME=ENV=VAL]      # fire on screen: a burning car, the flame-thrower, a molotov (fire pixels counted)
python tools/irxdis.py --decompile X.IRX OUT.c                # an IOP module as C (headless Ghidra; analysis only)
python tools/eedecomp.py 0x2f7d58 [...] [--out FILE]          # game functions as C (the analysed EE project; analysis only)
```

The rig (`src/debug/rig.cpp`, `tools/rig.py`) drives the game through its own script VM: `Game.teleport`,
`set_time`, `weather`, `invincible`, `mission(n)` and raw `scm(bytes)` (assembler: `rig.Scm`, VCS opcode numbers
from the Sanny Builder Library, checked against the handler table - game-internals.md 11). `build/rig/saves/` holds
card copies to start from (never the user's live card).

- Test runs are **hidden** (the user's choice, 2026-10-24): no window, physical input ignored, sound on (the user
  mutes the game in Windows); all launches go through `toolchain.test_window`; the rig passes `run.py --hidden`.
  `VCS_TEST_VISIBLE=1` shows the window when asked. `run.py` on its own shows the window: the user starts the game
  with it too.
- `VCS_DATA_DIR=<dir>` gives a run its own memory card / cache. **Never run tests against the user's
  card** (`%LOCALAPPDATA%/VCSRecomp/mc0`) when they could write to it; copy it with
  `robocopy /E /COPY:DAT /DCOPY:T` (keeps creation times, which the card order depends on).
- `vcs.ini` next to `vcs.exe` holds the user's settings; environment overrides (`VCS_ASPECT=169`)
  apply to one run without touching it.
- Rebuild costs: a change to `ps2_memory.h`, `gs_backend.h`, `ps2_hle_survey.h` or anything the
  generated code includes rebuilds everything (~6-10 min); runtime-only changes ~1 min; `src/` only
  seconds. Prefer `src/game/vcs_overrides.cpp` for experiments.
- Test output (screenshots, logs, card copies) goes under `build/<tool>/<name>`; it adds up (23 GB by 2026-10-23,
  mostly screenshots). Run `python tools/clean.py --apply --keep-days 7` after a session of test runs. A capture
  worth keeping as a reference goes to `build/scenes/reference` (scenes compare) or `build/regress/baseline`.
- Optimisation (`--opt`): a fresh build directory is `lto` (ThinLTO: what players get from the README). `--quick`
  (= `none`) for day-to-day work (fast relinks); the choice stays in the CMake cache, and the last line of a build says
  which one it is. `pgo` / `lto` relink slower and are for measuring and releases. A PGO profile from older code still
  builds; retrain (`--opt pgo --train`, ~30 min) after large changes. Never edit sources while a build or the PGO
  pipeline is compiling them.
- A successful build writes `iso=<disc>` into the `vcs.ini` next to `vcs.exe` when that file has none, so the exe
  started on its own finds the disc.
- The launcher (`launcher/`, docs/LAUNCHER.md) only runs `tools/`: build logic stays in Python, so a change to the
  pipeline is a change to the scripts. The tools print `@vcs {json}` event lines (`toolchain.event` / `stage` / `fail`
  with a code) the launcher reads; keep them when changing a tool's output. `VCSLauncher.exe --screenshot F.png
  [--screen home|task|sheet:saves|gallery... --demo error --frames N --action build]` renders its window offscreen
  (tests, docs; `--screen gallery` shows every widget); `--hidden --quit-after S` runs the real loop unshown and prints
  its cost; `--self-test` is CI's check. The launcher's UI is immediate mode: never open a file dialog inside a frame
  (`screens::runPending`), and keep it idle - no animation that runs forever while nothing happens, except Home's
  backdrop (the user's wish, 2026-11-01: 30 fps, off while the game runs and with reduce motion; DECISIONS.md).

## Rules that keep things working

1. **Reference first.** A native replacement of an emulated path is checked against it (bit-exact
   where possible: `PS2X_HLE_VERIFY=1`; picture diff via scenes otherwise) and keeps a switch back
   (`PS2X_HLE=0`-style). The emulated path is never deleted.
2. **Hooks on recompiled functions** (DECISIONS.md): a generated function may return to the EE
   scheduler mid-body and be resumed through the function table, bypassing a wrapper. A hook may work
   *before* calling the original or replace it entirely; never rely on code after the original
   returns. Never call a guest function directly from host code expecting it to run to the end -
   chain it as a guest tail call (see `VCS_TEST_SAVE`).
3. **Floating point**: no FP contraction in generated / VU code; VU maths rounds toward zero
   (`FE_TOWARDZERO` where reproducing it). Bit-exactness depends on both.
4. **Never run `PS2X_GS=gpu`** (paraLLEl-GS) on the dev machine: it hangs the GTX 1080 Ti into a TDR
   reset and blanks the user's screen. `hw` (D3D11) is vcs.exe's own default.
5. **Patch scripts**: shell heredocs on this machine turn `\n` inside Python strings into real
   newlines and corrupt C++ string literals. Use the Edit tool, or write the script to a file first.
6. **Scripts are Python 3, standard library only**, runnable from any shell; extend `tools/*.py`
   (shared helpers in `tools/toolchain.py`) instead of adding shell scripts.
7. **Runtime changes** in `third_party/PS2Recomp` get a `[gta-recomp]` marker and a row in
   PS2RECOMP_PATCHES.md.
8. **Commit** at every milestone / major change, with a message that says why. End commit messages
   with the attribution line the environment asks for.

## Recognising old problems

- Wrong colours on every texture: data shifted by VIFcodes (VIF1 DIRECT continuation, fixed).
- Stuck on a loading screen, no crash: an IOP-side wait (`VCS_TRACE_DELAY`, `PS2X_TRACE_SPU`).
- Thread "returns" to a float bit pattern (`0x3F333333`...): a saved `$ra` overwritten - the
  checkpoint-unwind class of bug (`VCS_WATCH=lo,hi,...`).
- Saves written but never found: card directory order (game-internals.md 10c).
- A mission stuck after a ped should talk, or peds vanishing: check the mission audio slots and who deletes the ped
  (`VCS_TRACE_PED_REMOVE`, game-internals.md 12); Shakedown's was the recompiler's 32-bit compare-with-zero branches.
- Something wrong only above 100 fps: a `ms_fTimeStep < 0.5` branch the PS2 never takes (game-internals.md 4).
- A mission bar / meter / timer faster at high frame rates: the mission script adds a fixed amount every frame; add
  the instruction to `src/game/vcs_script_timing.cpp` (game-internals.md 11f has the survey and how to read a mission).
- Load Game stuck on "Loading..." then a return to text (`0x4548435F`) from the script VM: host code took a game
  script for one of its own (a remembered script slot the load had refilled; game-internals.md 11d, `[rig] script
  slot` in the log).
- "Loading..." over black after a mission's last cutscene, the game running behind it (clock and scripts move): the
  screen left faded out for a script that never comes - Last Stand out of story order waits for the credits
  (game-internals.md 11g). F12's draw log shows the black quad and the text; the active scripts show who is missing.
- `ninja: error: manifest 'build.ninja' still dirty after 100 tries`: files dated after the clock (a zip from another
  time zone); `build.py` sets them to now (`toolchain.fix_future_files`, docs/LAUNCHER.md).
- A tool fails within a second, its log empty (a player's report, 2026-10-31): a crash before the first line - the log
  now ends with the exit code (`0xC000001D` illegal instruction: a build for a newer CPU, `PS2X_CPU`; DECISIONS.md).
- `[main] fatal exception: No mapping for the Unicode character exists in the target multi-byte code page`, or a path
  full of `?`s: a non-ASCII folder seen through the ANSI code page - vcs.exe's UTF-8 manifest missing or ignored
  (`src/vcs.manifest`, DECISIONS.md 2026-11-01).
- A save gone after saving (an empty slot file, or the whole save folder deleted): a card name the host could not
  store, or a failed card write - the game deletes its save folder after one (game-internals.md 10f). The card is
  zipped into `<data>/backups/auto-*.zip` before every change (`[MC] card backup` in the log).
- No fire (flame-thrower, molotovs, explosions, burning cars) while their smoke shows: GS sprites lost in the D3D11
  renderer - the pixel shader's depth output was the cause once (game-internals.md 6m); `tools/fire_check.py`.
- Decorative crowds or traffic appearing close to the player or replacing real peds/cars, or distant lights missing:
  read `docs/T2_POPULATION_AND_DISTANT_LIFE_PLAN.md` before touching `vcs_distance.*`, `vcs_distant.*`, or proxy draws
  in `vcs_hle_world.cpp`. Never clamp `pedDistance()` or `carDistance()` when `Distant crowds` / `Distant traffic` are
  enabled, never read `0x006F4CA0` directly inside `CWorld::Process`, and keep all decorative proxies strictly beyond
  the real population band (`>= 320-350 m`).
- First things to grep in a log: `[ee] thread N -> dormant`, `[guest-branch:unlisted-resume]`.

## Working style for long autonomous runs

- Follow docs/NATIVE_PORT_ROADMAP.md milestone by milestone without pausing for routine decisions;
  record decisions in DECISIONS.md and progress / numbers in PROGRESS.md.
- Measure before and after anything on the hot path (perf lines from `gamectl.py perf`, scenes'
  `perf_*.txt`); keep the numbers in the docs.
- Keep the regression rig (milestone 1) green; add a check when you fix a bug so it stays fixed.

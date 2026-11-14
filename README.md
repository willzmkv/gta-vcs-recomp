# GTA: Vice City Stories — Native PC port (PS2 static recompilation)

> [!WARNING]
> **Extremely experimental — do not use this fork yet.**
> This fork is **extremely experimental** and it is **not recommended for anyone to use it at the moment**. If you want to build or play the game, please use the original repository instead: **[gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp)**.

> **This is a fork** of [gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp) that brings the effects of
> PC mods (Proper Shaders, SkyGFX, Project2DFX, texture packs) to the native port as settings, authentic by default,
> runs PC Vice City (and San Andreas weapon) mods through a mod loader, and draws the city and its population much
> further. What it adds is listed in [What this fork adds](#what-this-fork-adds); the work list is in
> [docs/FORK.md](docs/FORK.md) and the [issues](../../issues).
>
> **There is no download.** This fork publishes no release, launcher or installer: you build the game yourself from
> the source and your own disc, as described in [Build it yourself](#build-it-yourself-windows-and-linux). The newest
> work is on the **`hd-world-merge`** branch until it is merged into `main`.

## What this fork adds

Everything is off or at the game's own values by default; the PS2 picture stays the reference.

**Mods**
- **Texture packs**: PCSX2-named DDS (BC1-BC5 / BC7, with their mips, uploaded as stored) or PNG files, and a texture
  dump (`VCS_TEXTURE_DUMP=1`) to make them. F2 > Mods > Replace textures ([docs/TEXTURE_PACKS.md](docs/TEXTURE_PACKS.md)).
- **Mod loader**, the way PC GTA's Mod Loader works: one folder per mod in `modloader/`, its `modloader.ini`
  (priorities, ignore lists), and F2 > Mods with a switch and a priority slider per mod, Rescan and Open folder, all
  applied while the game runs ([docs/MODS.md](docs/MODS.md)).
- **Game files from mods**: a mod's `GTA3PS2.IMG` entry (`admiral.mdl`, `#<index>.bin`) or loose file
  (`AMERICAN.GXT`, `ENGLISH.GXT`, `MAIN.SCM`) is read in place of the disc's, without rebuilding any archive;
  `tools/img.py` lists and extracts entries. A mod switched on or off that changes Vic's model or textures takes effect
  at once (he is dressed again in his outfit); other models change when the game loads them again.
- **PC Vice City vehicle mods as they come**: drop a car's `.dff` + `.txd` into `modloader/` and the game's vehicle of
  that name is replaced, converted to VCS's formats while the game loads. Cars, bikes, boats and helicopters, every
  part checked triangle by triangle against the `.dff`. VC's `wheels.dff` is not supported yet.
- **PC weapon mods** (Vice City and San Andreas): converted the same way, placed where VCS holds each weapon in Vic's
  hand, with VCS's HUD icons kept.

**The city and its people, further** (F2 > Graphics > Distance and population, experimental)
- **Detailed city distance**: the detailed buildings of the cells around you, not only the one you stand in - the city
  in full detail several hundred metres away instead of its low-detail version.
- **View / detail / population distance, vehicle and pedestrian density**: sliders from 0 (the game's own, the
  default) up to ten times as far / four times as many, with a population frame-rate target that thins the extra
  people and cars when the frame rate drops.
- **Distant traffic and distant crowds**: decorative cars and walkers on the roads and pavements beyond the simulated
  population, drawn with the real models, as Project2DFX does on PC.
- **Crowd bands**: about 25 real people within 100 m and 25 more out to 200 m (with Pedestrian density 50), the
  distant crowd past that; a walker coming in becomes a real person in its place, and back. Fewer people to simulate
  for the same busy streets ([docs/CROWD_BANDS_PLAN.md](docs/CROWD_BANDS_PLAN.md)).

**Graphics**
- **Fog**: the game's own distance fog range as settings, F2 > Graphics > Fog (100 % = the game's values).
- **Fire on screen again**: flames from the flame-thrower, molotovs, explosions and burning cars were missing on
  Direct3D 11 while their smoke showed; fixed (`tools/fire_check.py`).

**Memory and performance**
- **Direct3D 12** (the default on Windows; F2 > Graphics > Graphics API keeps Direct3D 11 and Vulkan): far less CPU
  per draw call than AMD's D3D11 driver, and fewer stutters while driving. Falls back to D3D11 when the system has no
  D3D12 device.
- **Streaming memory past the PS2's 32 MB**: F2 > Game > Memory > Streaming memory (PS2 5.4 MB, 12, 20 or 30 MB;
  restart) for large mod models and more different cars in traffic.
- **Native draws joined and instanced**: consecutive draws of the same mesh are merged; the distant cars and walkers are
  drawn many at once.

**Controls**
- **Modern layout** (F2 > Gamepad > Presets): R2 / L2 accelerate and brake, fire and aim, free aim on the right stick,
  and optionally GTA V's buttons. The forklift's forks stay on the D-pad's up / down in every layout.

---

A native **Windows and Linux** port of **Grand Theft Auto: Vice City Stories** (PS2, USA, SLUS-21590), made by
recompiling the game's PS2 executable into C++. Both are built from the same source and run natively: Direct3D 12 /
11 on Windows, Vulkan on Linux.

**This repository contains no game code or game data.** You need your own copy of the game as an `.iso`.
The build reads the executable from your disc, and the finished game reads everything else from it while it runs.

Status: in development. See [docs/PROGRESS.md](docs/PROGRESS.md) for what works, and [docs/LINUX.md](docs/LINUX.md)
for the Linux build in detail.

## Build it yourself (Windows and Linux)

You need about **6 GB** of free space (plus your disc image), an internet connection for the first build (it downloads the compiler and the
libraries), and **30 to 60 minutes** the first time. Use a short, local folder: `C:\Games\vcs-recomp` on Windows,
`~/games/vcs-recomp` on Linux - not Program Files, not a OneDrive-synced folder such as Desktop or Documents (the build
writes thousands of files).

### 1. Install the tools (once)

**Windows**
1. **Python 3** from [python.org](https://www.python.org/downloads/). During setup, tick **Add python.exe to PATH**.
2. **Git** from [git-scm.com](https://git-scm.com/download/win) (the default options are fine).
3. Open a new terminal (Start > *Terminal*) and check both:
   ```
   python --version
   git --version
   ```

There is no compiler to install: the first build downloads [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
(clang, lld, libc++ and the Windows headers in one package, ~190 MB once, into `build\build-tools`). CMake and Ninja are
used wherever they are already installed; when none is new enough (CMake 3.21, Ninja 1.10) the build downloads them
too (~55 MB). With Visual Studio 2022 or newer (*Desktop development with C++* and *C++ Clang tools for Windows*),
`set VCS_TOOLCHAIN=msvc` before building uses its clang-cl instead; the game is the same.

**Linux**
1. Python 3 and Git (most distributions have both).
2. After step 2 below, let the build list the rest:
   ```bash
   python3 tools/build.py --check-deps
   ```
   It prints what is missing and the command that installs it (`apt`, `dnf` or `pacman`): clang 19 or newer with lld,
   glslang, and the FFmpeg and SDL3 development packages. On Ubuntu 24.04 / Linux Mint 22 that is
   ```bash
   sudo apt install clang-20 lld-20 llvm-20 cmake ninja-build pkg-config git glslang-tools \
       libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
       libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
       libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev \
       libpipewire-0.3-dev libpulse-dev libasound2-dev libudev-dev libdbus-1-dev
   ```
   (`python3 tools/build.py --install-deps` installs them for you, asking for your password.)
3. A graphics driver with Vulkan 1.3: Mesa (AMD, Intel) or NVIDIA's own. `vulkaninfo --summary` shows it.

### 2. Get the source and your disc

1. Get the source - the `hd-world-merge` branch holds the newest work:
   ```bash
   git clone --branch hd-world-merge https://github.com/willzmkv/gta-vcs-recomp.git vcs-recomp
   cd vcs-recomp
   ```
2. Copy your disc image into that folder. It must be the **USA** release (SLUS-21590), and its file name must contain
   `Vice City Stories`, like `Grand Theft Auto - Vice City Stories (USA).iso`. (A disc elsewhere works too: step 3.)
3. Optional: check that it is the right disc, and the whole setup:
   ```bash
   python tools/vcs_disc.py info "Grand Theft Auto - Vice City Stories (USA).iso"
   python tools/doctor.py
   ```
   `doctor.py` checks the folder, the compiler, the disc and the build state, and says what to do next.

### 3. Build

In a terminal in the `vcs-recomp` folder (Windows Explorer: right-click the folder > *Open in Terminal*):
```bash
python tools/build.py --recompile        # Windows
python3 tools/build.py --recompile       # Linux
```
This reads the executable from your disc, recompiles it into C++ (`generated/`, about 200 MB, never committed) and
builds the game, optimised. It ends with:
```
[build] ok: build\vcs.exe (optimised: ThinLTO)        (Linux: build/vcs)
```
If your `.iso` is somewhere else, give its path: `python tools/build.py --recompile "D:\path\to\your.iso"`.
The whole output is in `build/build.log`.

### 4. Play

Start `build\vcs.exe` on Windows, `build/vcs` on Linux (a desktop shortcut works well; `python tools/run.py 0` starts
it with its output logged to `build/run/run.log`). The build already told it where your disc is.

- **F2** opens the settings: presets, resolution, widescreen, frame rate, vsync, sound, every key, mouse and gamepad
  binding, mods, and the experimental distance and population settings. Settings are saved in `vcs.ini` next to the
  game.
- **Alt+Enter** switches between full screen and the window. **F3** shows the performance overlay, **F4** records it
  to a CSV.
- **F5** opens the trainer ([docs/TRAINER.md](docs/TRAINER.md)). **F8** saves a screenshot to `screenshots/`.
- Keyboard, mouse and gamepads (Xbox, PlayStation, Switch Pro, ...) all work: [docs/CONTROLS.md](docs/CONTROLS.md).
- Saves (the memory card) live in `%LOCALAPPDATA%\VCSRecomp` on Windows and `~/.local/share/VCSRecomp` on Linux.
  `python tools/backup.py create` backs them up.
- Mods go into `modloader/` in the source folder, one folder per mod ([docs/MODS.md](docs/MODS.md)).

### 5. Update

```bash
git pull
python tools/build.py --recompile-if-needed
```
It recompiles from the disc only when the recompiler or its config changed; otherwise only what changed is compiled
(a few minutes). Your settings and saves are kept.

## Features

The defaults are the PS2 game as it was, except the frame rate: 60 fps with frame-rate independent timing. Every
other improvement below is an option in the settings menu (F2), and *Authentic (PS2)* goes back to the PS2's 30 fps.
*Display > Presets* turns on the usual ones at once: **Enhanced** for a modern graphics card, **Widescreen, light**
for older PCs and integrated graphics, **Authentic (PS2)** to go back.

**Runs natively, on Windows and Linux.** The game's own code, recompiled to C++ and running on the PC, not in an
emulator. The 3D world, characters, vehicles, HUD and menus are drawn by a native renderer - Direct3D 12 / 11 on
Windows, Vulkan on Linux -, sound and streaming run on the PC, and the movies are decoded with FFmpeg.

**Frame rate**
- Frame-rate independent timing: 60, 100, 120, 144, 165, 240 fps, your display's refresh rate or uncapped (up to
  240), with game time following real time - 60 by default. Or the original timing at 30 fps, or 60.
- Things the PS2 game counted per frame (vehicle physics, mission meters and timers) put back on real time, so they
  keep their speed at high frame rates ([docs/PROGRESS.md](docs/PROGRESS.md)).
- Low input delay with frame-rate independent timing: each frame starts on fresh input just in time to be shown.
- Vertical sync on or off, and a tearing mode for G-Sync / FreeSync monitors.

**Widescreen and ultrawide**
- Hor+ widescreen: 16:10, 16:9, 21:9, 32:9, or *Match window* for any shape between 4:3 and 32:9. You see more to the
  sides; nothing is cropped or stretched.
- The HUD kept at 4:3 in the middle, moved to the screen edges (keeping its shape), or stretched.

**Image quality**
- Internal resolution from 1x (640x448) up to 8x, or the window's own resolution.
- Anti-aliasing (MSAA 2x / 4x / 8x) and anisotropic texture filtering (up to 16x).
- Windowed or borderless full screen (Alt+Enter).

**Controls**
- Keyboard, mouse and gamepads (Xbox, DualShock / DualSense, Switch Pro and anything else SDL3 knows), mixed freely.
- Mouse look on foot, as in a PC game, with sensitivity and inversion; free aim with the mouse or the right stick.
- Every key, mouse button and gamepad button rebindable, two keys per control, deadzones, camera speed, trigger press
  point, swapped sticks, inverted axes; DualShock 2 rumble from 0 % to 200 %, DualSense adaptive triggers.

**Sound**
- Master, music, sound effects and movie volume; stereo, mono or swapped speakers.
- Keep running, mute or pause when the game is in the background.

**Built-in trainer (F5)** ([docs/TRAINER.md](docs/TRAINER.md)): god mode, wanted level, money, weapons and ammo,
spawn any of the 110 vehicles, teleport to every district, start any story mission or side job, time of day, weather
and game speed. It works through the game's own script commands, and nothing of it is ever saved.

## Problems

| What you see | What to do |
|---|---|
| `llvm-mingw (clang 19 or newer) not found, and downloading it failed` | Check the internet connection and build again; or unpack an llvm-mingw (`ucrt-x86_64`) yourself and set `VCS_LLVM_MINGW` to its folder. |
| `vswhere.exe not found` (with `VCS_TOOLCHAIN=msvc`) | Visual Studio isn't installed (`python tools/setup_vs.py install`), or build without it: unset `VCS_TOOLCHAIN`. |
| `no disc image` | Put the `.iso` in the source folder, or give its path after `--recompile`. |
| `unsupported executable build` or `SLUS_215.90 not found on disc` | The disc isn't the USA release (SLUS-21590). Other regions aren't supported. |
| CMake configure fails while downloading | Check the internet connection and that `git --version` works in a new terminal. |
| `CMake 3.21 or newer not found, and downloading it failed` | Check the internet connection, or install CMake yourself ([cmake.org](https://cmake.org/download)). `VCS_CMAKE` / `VCS_NINJA` set to a full path choose a particular one. |
| A step fails within seconds, its log ending in `exit code 0xC000001D (illegal instruction ...)` | The build was made for a newer processor than this PC's. Build again: the build follows the PC's processor (`python tools/doctor.py` shows which). |
| `vcs.exe` does nothing when started | It can't find the disc: put `iso=C:\path\to\your.iso` in `vcs.ini` next to `vcs.exe`. |
| Linux: `clang 19 or newer not found` / `missing build dependencies` | Install what `python3 tools/build.py --check-deps` lists. |
| Linux: `cannot find -lstdc++` | Install `libstdc++-<N>-dev` for your newest GCC. |
| Linux: no window / `no GPU with Vulkan 1.3` | Install your GPU's Vulkan driver (Mesa or NVIDIA); `vulkaninfo --summary` should list it. |

The build's full output is in `build/build.log` (`build/configure.log` for the configure step).
`python tools/doctor.py` checks everything at once and says what to do next; `python tools/report.py` zips the logs for
a bug report (no game files, no saves).

## For developers

| Command | What |
|---|---|
| `python tools/build.py` | rebuild after code changes |
| `python tools/build.py --quick` | rebuild without link-time optimisation: much faster relinks. It stays quick until `--opt lto` |
| `python tools/build.py --opt lto` | back to the optimised build (the default) |
| `python tools/build.py --opt pgo` | ThinLTO + profile-guided; trains a profile first (~30-50 min) |
| `python tools/recompile.py [ISO]` | disc → generated C++ only (`--check`: is generated/ current?) |
| `python tools/build.py --recompile-if-needed [ISO]` | recompile only when the recompiler, its config or the function map changed, then build |
| `python tools/doctor.py` | everything checked: folder, memory, compiler, disc, build state, the next step |
| `python tools/build_dir.py [set PATH \| reset]` | build in another folder (another drive...) instead of `build\`; every tool follows it |
| `python tools/backup.py create\|list\|restore` | the memory card backed up / restored (order and times kept) |
| `python tools/deps.py fetch` | the libraries the build downloads, into `deps/` (offline builds) |
| `python tools/run.py 0` | run the game with its output logged to `build/run/run.log` (`run.py 60 name`: stop after 60 s) |
| `python tools/gamectl.py` | control a running game: screenshots, key presses, perf numbers, quit |
| `python tools/bench_avenue.py` | the performance benchmark: a drive along Little Havana's avenue with the population full |
| `python tools/ghidra_analyze.py` | redo the Ghidra analysis (Ghidra 12 + JDK 21) |

All tools are Python 3 scripts using only the standard library. [AGENTS.md](AGENTS.md) is the guide to working
on the code; the documentation is in [docs/](docs).

| Path | What |
|---|---|
| `third_party/PS2Recomp` | Vendored PS2 recompiler + runtime (GPL-3.0) |
| `game/vcs` | Recompiler config and function map for SLUS_215.90 |
| `src` | Game-specific code (overrides, renderer, settings, mods, platform) |
| `tools` | Disc reader, build pipeline, test and analysis scripts |
| `docs` | How the game works, decisions, progress, plans |

## License

GPL-3.0 (inherited from PS2Recomp). Grand Theft Auto and all game content are property of Rockstar Games /
Take-Two Interactive.

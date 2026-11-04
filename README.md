# GTA: Vice City Stories — Native PC port (PS2 static recompilation)

> **This is a fork** of [gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp) that brings the effects of
> PC mods (Proper Shaders, SkyGFX, Project2DFX, texture packs) to the native port as settings, authentic by default,
> and runs PC Vice City (and San Andreas weapon) mods through a mod loader. What it adds is listed in
> [What this fork adds](#what-this-fork-adds); the work list and what comes next are in [docs/FORK.md](docs/FORK.md).
> Everything after that section is upstream's.

## What this fork adds

Everything is off or at the game's own values by default; the PS2 picture stays the reference.

**Mods**
- **Texture packs**: PCSX2-named DDS (BC1-BC5 / BC7, with their mips, uploaded as stored) or PNG files, and a texture
  dump (`VCS_TEXTURE_DUMP=1`) to make them. F2 > Mods > Replace textures ([docs/TEXTURE_PACKS.md](docs/TEXTURE_PACKS.md);
  [#9](https://github.com/willzmkv/gta-vcs-recomp/pull/9), PNG part from upstream PR #8).
- **Mod loader**, the way PC GTA's Mod Loader works: one folder per mod in `modloader/`, its `modloader.ini`
  (priorities, ignore lists), and F2 > Mods with a switch and a priority slider per mod, Rescan and Open folder, all
  applied while the game runs ([docs/MODS.md](docs/MODS.md); [#9](https://github.com/willzmkv/gta-vcs-recomp/pull/9)).
- **Game files from mods**: a mod's `GTA3PS2.IMG` entry (`admiral.mdl`, `#<index>.bin`) or loose file
  (`AMERICAN.GXT`, `ENGLISH.GXT`, `MAIN.SCM`) is read in place of the disc's, without rebuilding any archive; `tools/img.py`
  lists and extracts entries ([#13](https://github.com/willzmkv/gta-vcs-recomp/pull/13)).
- **PC Vice City vehicle mods as they come**: drop a car's `.dff` + `.txd` into `modloader/` and the game's vehicle of
  that name is replaced, converted to VCS's formats while the game loads. Cars, bikes, boats and helicopters, every
  part checked triangle by triangle against the `.dff`. VC's `wheels.dff` is not supported yet
  ([#14](https://github.com/willzmkv/gta-vcs-recomp/pull/14), [#19](https://github.com/willzmkv/gta-vcs-recomp/pull/19)).
- **PC weapon mods** (Vice City and San Andreas): converted the same way, placed where VCS holds each weapon in Vic's
  hand, with VCS's HUD icons kept. SA's D3D9 textures are read, SA's muzzle-flash mesh is left out, and the SA minigun
  is drawn once, its barrels turning while firing
  ([#18](https://github.com/willzmkv/gta-vcs-recomp/pull/18), [#19](https://github.com/willzmkv/gta-vcs-recomp/pull/19),
  [#21](https://github.com/willzmkv/gta-vcs-recomp/pull/21)).

**Graphics**
- **Fog**: the game's own distance fog range as settings, F2 > Graphics > Fog (fog distance, fog start; 100 % = the
  game's values).
- **Distance and population (beta, experimental)**: F2 > Graphics > Distance and population, sliders from 0 (the
  game's own, the default) to 100: View distance and Detail distance (objects and crowds drawn and detailed up to ten
  times as far, through the game's own LOD scale), Population distance (cars and people appear and stay up to three
  times as far), Vehicle density and Pedestrian density (up to four times as many; larger pools with Streaming memory
  above PS2), a population frame-rate target, and Distant traffic (decorative cars on the road network beyond the
  simulated traffic, drawn with the real cars' models, as Project2DFX's distant cars)
  ([#23](https://github.com/willzmkv/gta-vcs-recomp/pull/23), [#25](https://github.com/willzmkv/gta-vcs-recomp/pull/25)).
  Dense streets still cost frame rate (each car and person runs the PS2's code, and their draws weigh on Direct3D 11);
  the detailed world further and distant lights are still to come ([docs/FORK.md](docs/FORK.md), T2).
- **Fire on screen again**: flames from the flame-thrower, molotovs, explosions and burning cars were missing on
  Direct3D 11 while their smoke showed; fixed, and checked by `tools/fire_check.py`
  ([#20](https://github.com/willzmkv/gta-vcs-recomp/pull/20)).

**Memory and performance**
- **Streaming memory past the PS2's 32 MB**: F2 > Game > Memory > Streaming memory (PS2 5.4 MB, 12, 20 or 30 MB;
  restart) gives the game's streaming more room for large mod models, and more different cars in traffic. Texture
  pack images are now created off the main thread (far fewer hitches with an HD pack)
  ([#17](https://github.com/willzmkv/gta-vcs-recomp/pull/17), [#19](https://github.com/willzmkv/gta-vcs-recomp/pull/19)).
- **Native draws joined**: consecutive draws of the same mesh are merged, about half the draw calls; with VC car mods
  the hitches mostly disappear ([#22](https://github.com/willzmkv/gta-vcs-recomp/pull/22)).

---

A native **Windows and Linux** port of **Grand Theft Auto: Vice City Stories** (PS2, USA, SLUS-21590), made by
recompiling the game's PS2 executable into C++. Both are built from the same source and run natively: Direct3D 11 on
Windows, Vulkan on Linux.

**This repository contains no game code or game data.** You need your own copy of the game as an `.iso`.
The build reads the executable from your disc, and the finished game reads everything else from it while it runs.

Status: in development. See [docs/PROGRESS.md](docs/PROGRESS.md) for what works, and [docs/LINUX.md](docs/LINUX.md)
for the Linux build in detail.

## Install on Windows: the launcher (easiest)

1. Download the newest `VCS-Recomp-<version>-windows.zip` from the [Releases](../../releases) page and extract it
   somewhere short and local, like `C:\Games\VCS` (not Program Files, not a OneDrive-synced folder such as Desktop or
   Documents: the build writes thousands of files).
2. Start **`VCSLauncher.exe`** in that folder. It walks you through four steps, one at a time, then shows a big
   **Play** button:
   - **Folder**: the game is built in `build\` next to the launcher, or in a folder you choose (another drive with
     more room, say): *Choose another folder* on this step.
   - **Disc**: it looks for your `.iso` in Downloads, Desktop, Documents and its own folder; otherwise choose it (or
     drag it onto the window, or copy your disc from a DVD drive). It checks that it is the USA release.
   - **Build tools**: the game is compiled on your PC with Microsoft's free C++ tools. If they are missing,
     *Install build tools* runs Microsoft's installer with the right options (about 6 GB; Windows asks for permission).
   - **Build**: recompiles the game from your disc and compiles it, with a progress bar - 20 to 60 minutes the first
     time, a few minutes after an update. Nothing is downloaded during the build: the release brings Python and the
     libraries.
   - **Play**.

The launcher also backs up and restores your saves, makes a desktop shortcut, checks for a new release (only when you
ask, or at start if you turn that on in *Settings*), and zips the logs into a report for bug reports. Windows may warn
that the launcher is from an unknown publisher (it isn't signed): *More info > Run anyway*. How it works:
[docs/LAUNCHER.md](docs/LAUNCHER.md). Everything it does is a script in `tools/` you can also run yourself (below).
If the *Pricedown* font (GTA's lettering, free for personal desktop use from Typodermic) is installed on your PC, the
launcher uses it for its title and Play button; its licence doesn't allow including it, so it isn't.

## Install on Linux: the launcher (easiest)

1. Download the newest `VCS-Recomp-<version>-x86_64.AppImage` from the [Releases](../../releases) page. It runs on
   any 64-bit desktop with glibc 2.35 or newer: Ubuntu 22.04, Debian 12, Linux Mint 21, Fedora, Arch and later, on
   X11 or Wayland. It needs a Vulkan driver (Mesa or NVIDIA's) and `python3`, which every desktop has.
2. Make it executable and start it: in the file manager, *Properties > Permissions > Allow executing as a program*, or
   `chmod +x VCS-Recomp-*.AppImage && ./VCS-Recomp-*.AppImage`. The first start puts the game's files (the source,
   the scripts, the libraries) in `~/.local/share/VCSRecomp/install`, where the game is built, then walks you through
   the same steps as on Windows:
   - **Folder**: where the game is built (another drive with more room: *Choose another folder*).
   - **Disc**: your `.iso` from Downloads, Desktop or Documents, chosen, or dropped onto the window.
   - **Build tools**: the compiler (clang 19 or newer) and the libraries the build needs, from your distribution.
     *Install packages* asks for your password and installs them; *Install in a terminal* shows the command instead.
     On Ubuntu 22.04 and Debian 12, whose clang is older, it adds LLVM's own repository ([apt.llvm.org](https://apt.llvm.org)) first.
   - **Build**, then **Play**.

The AppImage is the whole launcher: *Add a shortcut to the desktop* in *Settings* puts it in your applications menu
too. Updates replace the AppImage itself (*Settings > Check now*); the next start puts the new files in place and only
what changed is rebuilt. Without FUSE (some containers and minimal systems), start it with
`--appimage-extract-and-run`.

## Install by hand (Windows and Linux)

The release zip contains the full source: the steps below work in it too, and in a git clone.

### 1. Install the tools (once)

**Windows**
- **Python 3**: [python.org](https://www.python.org/downloads/). During setup, tick *Add python.exe to PATH*.
- **Git**: [git-scm.com](https://git-scm.com/download/win) (the default options are fine). The first build uses it
  to download the libraries the game needs (SDL3, FFmpeg, ...), so it also needs an internet connection.
- **Visual Studio 2022 or newer** (the free Community edition, or just the
  [Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)). In the installer, pick the workload
  **Desktop development with C++**, and under *Installation details* on the right also tick
  **C++ Clang tools for Windows**.

That's all: you don't need to open Visual Studio or a special command prompt. CMake and Ninja are found wherever
they are (Visual Studio's own, the CMake installer, winget, scoop, Chocolatey, pip, CLion...); when none is new enough
(CMake 3.21, Ninja 1.10), the build downloads them once (~55 MB, into `build\build-tools`).

**Linux**
- Python 3 and Git (most distributions have both), then let the build list the rest: in the source folder (step 2),
  ```bash
  python3 tools/build.py --check-deps
  ```
  prints what is missing and the command that installs it (`apt`, `dnf` or `pacman`): clang 19 or newer with lld,
  glslang, and the FFmpeg and SDL3 development packages (CMake and Ninja too if you like; without them, or with ones
  that are too old, the build downloads its own). On Ubuntu 24.04 / Linux Mint 22 that is
  ```bash
  sudo apt install clang-20 lld-20 llvm-20 cmake ninja-build pkg-config git glslang-tools \
      libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
      libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev \
      libpipewire-0.3-dev libpulse-dev libasound2-dev libudev-dev libdbus-1-dev
  ```
- A graphics driver with Vulkan 1.3: Mesa (AMD, Intel) or NVIDIA's own. `vulkaninfo --summary` shows it.

### 2. Get the source and your disc

1. Get the source, e.g. into `C:\Games\vcs-recomp` (Windows) or `~/games/vcs-recomp` (Linux):
   ```bash
   git clone <this repository's URL> vcs-recomp
   ```
   (*Code > Download ZIP* on GitHub works too.)
2. Copy your disc image into that folder. It must be the USA release, and its file name must contain
   `Vice City Stories`, like the usual `Grand Theft Auto - Vice City Stories (USA).iso`.

Optional check that the disc is the right one:
```bash
python tools/vcs_disc.py info "Grand Theft Auto - Vice City Stories (USA).iso"
```

### 3. Build

Open a terminal in the folder (Windows Explorer: right-click the folder > *Open in Terminal*) and run:
```bash
python tools/build.py --recompile        # Windows
python3 tools/build.py --recompile       # Linux
```
This recompiles the game from your disc and builds it, optimised. The first time takes a while: the game becomes
about 200 MB of C++ to compile (about 15 minutes on a 4-core laptop). It ends with:
```
[build] ok: build\vcs.exe (optimised: ThinLTO)        (Linux: build/vcs)
```
If your `.iso` is somewhere else, give its path: `python tools/build.py --recompile "D:\path\to\your.iso"`.

### 4. Play

Start `build\vcs.exe` on Windows, `build/vcs` on Linux (a desktop shortcut works well). The build already told it
where your disc is.

- **F2** opens the settings: presets, resolution, widescreen, frame rate, vsync, sound, and every key, mouse and
  gamepad binding. The defaults match the PS2 except the frame rate (60 fps, frame-rate independent); every other
  improvement is an option you turn on (Display > Presets > Enhanced turns on the usual ones at once). Settings are
  saved in `vcs.ini` next to the game.
- **Alt+Enter** switches between full screen and the window.
- **F5** opens the trainer ([docs/TRAINER.md](docs/TRAINER.md)).
- **F8** saves a screenshot to the `screenshots` folder next to the game.
- Keyboard, mouse and gamepads (Xbox, PlayStation, Switch Pro, ...) all work: [docs/CONTROLS.md](docs/CONTROLS.md).
- Saves (the memory card) live in `%LOCALAPPDATA%\VCSRecomp` on Windows and `~/.local/share/VCSRecomp` on Linux.

## Features

The defaults are the PS2 game as it was, except the frame rate: 60 fps with frame-rate independent timing. Every
other improvement below is an option in the settings menu (F2), and *Authentic (PS2)* goes back to the PS2's 30 fps.
*Display > Presets* turns on the usual ones at once: **Enhanced** for a modern graphics card, **Widescreen, light**
for older PCs and integrated graphics, **Authentic (PS2)** to go back.

**Runs natively, on Windows and Linux.** The game's own code, recompiled to C++ and running on the PC, not in an
emulator. The 3D world, characters, vehicles, HUD and menus are drawn by a native renderer - Direct3D 11 on Windows,
Vulkan on Linux -, sound and streaming run on the PC, and the movies are decoded with FFmpeg.

**Frame rate**
- Frame-rate independent timing: 60, 100, 120, 144, 165, 240 fps, your display's refresh rate or uncapped (up to
  240), with game time following real time - 60 by default. Or the original timing at 30 fps, or 60.
- Things the PS2 game counted per frame (vehicle physics, mission meters and timers) put back on real time, so they
  keep their speed at high frame rates. Some behaviour at high frame rates is still being fixed
  ([docs/PROGRESS.md](docs/PROGRESS.md)).
- Fast: well past 100 fps nearly everywhere at the original resolution (over 200 in most of the city on an
  i7-7700K + GTX 1080 Ti), and a locked 60 on a 2015 laptop with integrated graphics (Linux, Intel HD 520;
  [docs/PERFORMANCE.md](docs/PERFORMANCE.md)).
- Low input delay with frame-rate independent timing: each frame starts on fresh input just in time to be shown.
- Vertical sync on or off, and a tearing mode for G-Sync / FreeSync monitors.
- Frame rate counter, and a performance overlay (F3) with CSV recording (F4).

**Widescreen and ultrawide**
- Hor+ widescreen: 16:10, 16:9, 21:9, 32:9, or *Match window* for any shape between 4:3 and 32:9 (3440x1440 and
  other monitors that are not exactly 21:9). You see more to the sides; nothing is cropped or stretched.
- The HUD kept at 4:3 in the middle, moved to the screen edges (keeping its shape), or stretched.

**Image quality**
- Internal resolution from 1x (640x448) up to 8x, or the window's own resolution, with the effects keeping their
  original look.
- Anti-aliasing (MSAA 2x / 4x / 8x) and anisotropic texture filtering (up to 16x).
- Optional [PCSX2 texture packs](docs/TEXTURE_PACKS.md) (DDS or PNG), enabled in F2 > Mods > Replace textures.
- Windowed or borderless full screen (Alt+Enter).

**Controls**
- Keyboard, mouse and gamepads (Xbox, DualShock / DualSense, Switch Pro and anything else SDL3 knows), mixed freely.
- Mouse look on foot, as in a PC game (the mouse turns the camera itself), with sensitivity and inversion; right click
  locks on.
- Driving on W / S / Space (accelerate, brake / reverse, handbrake), keys that do something else on foot.
- Every key, mouse button and gamepad button rebindable, with two keys per control and a *Walk* key for walking speed
  on the keyboard.
- Stick deadzones, camera speed, trigger press point, swapped sticks, inverted camera axes.
- DualShock 2 rumble on your gamepad, from 0% to 200%.

**Sound**
- Master, music, sound effects and movie volume; stereo, mono or swapped speakers.
- Keep running, mute or pause when the game is in the background.

**Built-in trainer (F5)**, a list menu in the style of RDR2's Native Trainer ([docs/TRAINER.md](docs/TRAINER.md)):
- Player: god mode, heal, never wanted, set the wanted level, money, infinite sprint, police / everyone ignore you.
- Weapons: any weapon, loadouts, unlimited ammo, no reload, fast reload.
- Vehicles: spawn any of the 110 vehicles (cars, bikes, boats, helicopters, planes), invincible, repair, flip,
  boost, random paint.
- Teleport to every district, save and return to a position.
- Start any of the 59 story missions or a side job (Paramedic, Vigilante, Fire Truck, Air Rescue, Taxi).
- World: time of day, freeze time, weather, game speed 25% to 200%.
- Works through the game's own script commands, and nothing of it is ever saved: every start is the original game.

**Also**
- Skip the intro movies (off by default, as on the PS2).
- Saves on a memory card (`%LOCALAPPDATA%\VCSRecomp` on Windows, `~/.local/share/VCSRecomp` on Linux), written and
  read by the game's own save code.
- Every setting kept in `vcs.ini` next to the game.

## Updating

- **A release** (the launcher): *Check for updates*, then *Update now*: it puts the new release in place (keeping
  your built game, settings and saves) and the next *Update the build* compiles only what changed. By hand:
  `python tools/update.py check` / `python tools/update.py apply`. On Linux the AppImage file is replaced; start it
  again and it puts the new files in place.
- **A git clone**: `git pull`, then build again: `python tools/build.py --recompile-if-needed` recompiles only when
  the recompiler or its config changed.

## Problems

| What you see | What to do |
|---|---|
| `vswhere.exe not found` | Visual Studio isn't installed: see step 1 (or `python tools/setup_vs.py install`). |
| CMake configure fails, mentioning `clang-cl` | Open the Visual Studio Installer, *Modify*, and tick **C++ Clang tools for Windows**. |
| `no disc image` | Put the `.iso` in the repository folder, or give its path after `--recompile`. |
| `unsupported executable build` or `SLUS_215.90 not found on disc` | The disc isn't the USA release (SLUS-21590). Other regions aren't supported. |
| CMake configure fails while downloading | Check the internet connection and that `git --version` works in a new terminal. |
| `CMake 3.21 or newer not found, and downloading it failed` | Check the internet connection, or install CMake yourself ([cmake.org](https://cmake.org/download), or Visual Studio Installer, *Modify*, **C++ CMake tools for Windows**). `VCS_CMAKE` / `VCS_NINJA` set to a full path choose a particular one. |
| A step fails within seconds, its log empty or ending in `exit code 0xC000001D (illegal instruction ...)` | The build was made for a newer processor than this PC's. Build again: the build follows the PC's processor (AVX2, AVX or SSE4; `python tools/doctor.py` shows which). Processors without SSE4.2 (before 2008) can't run the game. |
| `vcs.exe` does nothing when started | It can't find the disc: put `iso=C:\path\to\your.iso` in `vcs.ini` next to `vcs.exe`. |
| Linux: `clang 19 or newer not found` / `missing build dependencies` | Install what `python3 tools/build.py --check-deps` lists. |
| Linux: `cannot find -lstdc++` | Install `libstdc++-<N>-dev` for your newest GCC (`build.py` works around it when an older one has it). |
| Linux: the game starts but no window / `no GPU with Vulkan 1.3` | Install your GPU's Vulkan driver (Mesa or NVIDIA); `vulkaninfo --summary` should list it. |

The build's full output is in `build/build.log` (`build/configure.log` for the configure step).
`python tools/doctor.py` checks everything at once (folder, compiler, disc, whether the game is current) and says what
to do next; `python tools/report.py` zips the logs for a bug report (no game files, no saves).

## For developers

| Command | What |
|---|---|
| `python tools/build.py` | rebuild after code changes |
| `python tools/build.py --quick` | rebuild without link-time optimisation: much faster relinks, a somewhat slower game. It stays quick until `--opt lto` (switching recompiles everything, ~6-10 min) |
| `python tools/build.py --opt lto` | back to the optimised build (the default) |
| `python tools/build.py --opt pgo` | ThinLTO + profile-guided; trains a profile first (~30-50 min, from the rig's card copies in `build/rig/saves` or a new game) |
| `python tools/recompile.py [ISO]` | disc → generated C++ only (`--check`: is generated/ current?) |
| `python tools/build.py --recompile-if-needed [ISO]` | recompile only when the recompiler, its config or the function map changed, then build |
| `python tools/build.py --launcher` | build the launcher (`VCSLauncher.exe` in the repository root; needs no disc) |
| `python tools/doctor.py` | everything checked: folder, memory, compiler, disc, build state, the next step (`--json` for the launcher) |
| `python tools/build_dir.py [set PATH \| reset]` | build in another folder (another drive...) instead of `build\`; every tool follows it |
| `python tools/backup.py create\|list\|restore` | the memory card backed up / restored (order and times kept) |
| `python tools/release.py [--worktree]` | the Windows release zip (source, launcher, embeddable Python, `deps/`), self-tested; refuses game files, and uncommitted changes without `--worktree`. On Linux, `--platform linux` makes the AppImage |
| `python tools/deps.py fetch` | the libraries the build downloads, into `deps/` (offline builds) |
| `python tools/run.py 0` | run the game with its output logged to `build/run/run.log` (`run.py 60 name`: stop after 60 s, log to `name.log`) |
| `python tools/gamectl.py` | control a running game: screenshots, key presses, perf numbers, quit |
| `python tools/ghidra_analyze.py` | redo the Ghidra analysis (Ghidra 12 + JDK 21) |

All tools are Python 3 scripts using only the standard library. [AGENTS.md](AGENTS.md) is the guide to working
on the code; the documentation is in [docs/](docs).

| Path | What |
|---|---|
| `third_party/PS2Recomp` | Vendored PS2 recompiler + runtime (GPL-3.0) |
| `game/vcs` | Recompiler config and function map for SLUS_215.90 |
| `src` | Game-specific code (overrides, renderer, settings, platform) |
| `tools` | Disc reader, build pipeline, launcher back end, test and analysis scripts |
| `launcher` | The launcher (Windows and Linux, its own renderer and UI; runs the tools) |
| `docs` | How the game works, decisions, progress |

## License

GPL-3.0 (inherited from PS2Recomp). Grand Theft Auto and all game content are property of Rockstar Games /
Take-Two Interactive.

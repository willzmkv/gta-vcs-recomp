# GTA: Vice City Stories — PC (VCS.rec)

A native **Windows** port of **Grand Theft Auto: Vice City Stories** (PS2, USA, SLUS-21590), made by recompiling the
game's PS2 executable into C++ and drawing it with a native renderer. This is a fork of
[gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp): it keeps that port's game fixes and adds its own
launcher, a city drawn much further with a fuller population, PC mod support and more (see
[What this fork adds](#what-this-fork-adds)).

**No game code or game data is included.** You need your own copy of the game as an `.iso`. The launcher builds the
game on your PC from this source and your disc, and the game reads everything else from the disc while it runs.

> Linux comes after 1.0.0 (the base port runs there; this fork's build hasn't been checked on it yet).

## Download and play (Windows)

**You need:** Windows 10 or 11 (64-bit), a processor with SSE4.2 (any from the last ~12 years; AVX2 builds a faster
game), 8 GB of memory, a graphics card with Direct3D 12 (Direct3D 11 works too), about **6 GB** of free space, an
internet connection for the first build, and the game's **USA** disc as an `.iso`.

1. Download **`VCS.rec-Launcher-<version>.exe`** from [Releases](../../releases): one file with everything inside (or
   the same as `VCS-Recomp-<version>-windows.zip`, to extract yourself). Keep the game in a short local folder, such
   as `C:\Games\VCS`: not Program Files, and not a OneDrive-synced folder such as Desktop or Documents.
2. Start it. The launcher walks you through setup:
   - **Folder** — checks the place and the free space.
   - **Disc** — drop your `.iso` on the window or pick it. The launcher checks that it is the USA release; it can also
     copy a PS2 disc from a DVD drive.
   - **Build tools** — downloaded on the first build (the compiler, about 190 MB, once). Nothing to install.
   - **Game** — builds the game for your PC: **30 to 60 minutes** the first time.
3. Press **Play**.

Your saves live in `%LOCALAPPDATA%\VCSRecomp`. The launcher's **Saves** page backs them up and restores them, and the
game itself keeps a backup before it changes a save.

### Updating

In the launcher: **Check for updates and update the build** (the launcher is in English or Portuguese). It downloads
the new version from this fork's releases and rebuilds the game. Most updates only recompile what changed (a few
minutes); the notes of each release say when one needs a full rebuild. Saves and settings are kept.

Version numbers: **1.0.x** are small fixes, **1.x.0** bring in a new build of the
[upstream port](https://github.com/gothamor/gta-vcs-recomp), and **x.0.0** are big improvements of this fork's own.

## What this fork adds

The PS2 game is the reference: the improvements are settings, and **Original (PS2)** in the graphics presets puts
everything back to the game's own look.

**The launcher**
- Setup, build, play, updates, saves and reports in one window, with a Vice City look.
- **Graphics presets** from *Original (PS2)* through *Low*, *Medium* and *High* to *Vice Ultra*, with the GPU and CPU
  cost of each shown before you choose; any single setting can still be changed. On a first install the launcher
  reads your graphics card, processor and memory and picks the preset your PC runs with room to spare.
- **Controls** shown on a picture of your gamepad (DualShock 2, Xbox, DualShock 4, DualSense, Switch Pro) or the
  keyboard and mouse, for the classic PS2 layout or a modern one (triggers to drive and shoot).
- **Shader warm-up** before the game starts, against stutter the first time an effect is drawn.

**The city and its people, further** (game settings, F2 → Graphics → Distance and population)
- **Detailed city distance**: the detailed buildings several hundred metres away instead of the low-detail city.
- **View, detail and population distance, vehicle and pedestrian density**: from the game's own up to ten times as
  far / four times as many, with a frame-rate target that thins the extra people and cars when the frame rate drops.
- **Distant traffic and distant crowds**: cars and walkers on the roads and pavements beyond the simulated population,
  drawn with the real models, as Project2DFX does on PC.
- **Crowd bands** (experimental): a few real people near you and the crowd beyond, a walker turning into a real
  person as you come closer.

**Mods** ([docs/MODS.md](docs/MODS.md)) — one folder per mod in `modloader/`, switched and ordered live in the game
- **Included:** HD Hands (Vic's hands in higher detail) and HD Hud (a sharper HUD, radar and weapon icons), made for
  this fork.
- **Texture packs**: PCSX2-style packs, DDS or PNG ([docs/TEXTURE_PACKS.md](docs/TEXTURE_PACKS.md)).
- **PC Vice City vehicle mods** as they come: a car's `.dff` + `.txd` replaces the game's vehicle of that name (cars,
  bikes, boats, helicopters).
- **PC weapon mods** (Vice City and San Andreas), held where VCS holds each weapon, with the game's HUD icons.
- **Game files from mods**: models, textures, `MAIN.SCM` and text files read in place of the disc's.

**Graphics and performance**
- **Direct3D 12** by default (Direct3D 11 and Vulkan in F2 → Graphics): far less CPU per draw, fewer stutters.
- **Streaming memory** past the PS2's (F2 → Game → Memory) for big mod models and more different cars.
- **Fog** distance as a setting; fire, flames and explosions drawn again on Direct3D 11.

**Fixes**
- Fists, bats and knives hit every pedestrian again (with the larger crowds many could not be hit).
- From the upstream port's 0.3.0: Hose the Hoes, Snitch Hitch and Kill Phil above 30 fps; smoke, sprays, fire,
  explosions, lightning, clouds, camera shake, rain and the night-time light trails as on the PS2 at any frame rate;
  the radio no longer skips when you get back in a car; fixes to the recompiler.

## Features of the port

**Frame rate.** 60 fps by default with frame-rate independent timing; 100, 120, 144, 165, 240, your display's rate or
uncapped, or the PS2's 30 fps. What the game counted per frame (vehicle physics, mission meters and timers, effects)
keeps its speed at any frame rate. Vertical sync on or off, and a tearing mode for G-Sync / FreeSync screens.

**Widescreen and ultrawide.** Hor+ 16:10, 16:9, 21:9, 32:9 or any window shape: more to the sides, nothing cropped or
stretched. The HUD in the middle at 4:3, at the screen edges, or stretched.

**Image quality.** Internal resolution from 1x (640x448) to 8x or the window's, MSAA up to 8x, anisotropic filtering
up to 16x, windowed or borderless full screen (Alt+Enter).

**Controls** ([docs/CONTROLS.md](docs/CONTROLS.md)). Keyboard, mouse and gamepads, mixed freely; mouse look and free
aim; every control rebindable; rumble, and DualSense adaptive triggers and light bar.

**Sound.** Master, music, effects and movie volume; stereo, mono or swapped; keep playing, mute or pause in the
background.

**In the game**
- **F2** the settings: graphics, distance and population, keys, mouse, gamepad, sound, mods. Saved in `vcs.ini` next
  to the game.
- **F3** the performance overlay (**F4** records it to a CSV). **F8** a screenshot into `screenshots/`.
- **F5** the trainer ([docs/TRAINER.md](docs/TRAINER.md)): god mode, wanted level, money, weapons, any vehicle,
  teleports, any mission, time and weather. It works through the game's own script commands and is never saved.

## Problems

| What you see | What to do |
|---|---|
| The launcher says the disc is another release | Only the USA release (SLUS-21590) is supported. |
| The first build fails while downloading | Check the internet connection and press *Try again*; it continues where it stopped. |
| A step fails within seconds with `0xC000001D (illegal instruction ...)` | The build was made for a newer processor: build again from the launcher (it follows this PC's processor). |
| Files "dated in the future" / `build.ninja still dirty` | The PC's clock or time zone is off: correct it and build again. |
| `vcs.exe` does nothing when started by itself | Start it from the launcher, or put `iso=C:\path\to\your.iso` in `vcs.ini` next to `vcs.exe`. |

The launcher's **Help → Make a report** zips the logs for a bug report (no game files, no saves). Report problems in
this fork's [issues](../../issues).

## Building from source

The launcher runs the same Python scripts you can run yourself (Python 3 and Git needed):

```
git clone https://github.com/willzmkv/gta-vcs-recomp.git vcs-recomp
cd vcs-recomp
python tools/doctor.py                    # checks the folder, compiler, disc and build; says what to do next
python tools/build.py --recompile [ISO]   # disc -> C++ -> the game, optimised (ends with "[build] ok: build\vcs.exe")
python tools/run.py 0                     # play, with the output logged to build/run/run.log
```

After `git pull`, `python tools/build.py --recompile-if-needed` rebuilds only what changed. Other useful commands:

| Command | What |
|---|---|
| `python tools/build.py --quick` | a fast unoptimised build for development (it stays so until `--opt lto`) |
| `python tools/build.py --opt lto` | back to the optimised build (the default) |
| `python tools/build.py --launcher` | the launcher (`VCSLauncher.exe` in the source folder) |
| `python tools/build_dir.py set PATH` | build in another folder; every tool follows it |
| `python tools/backup.py create\|list\|restore` | memory card backups |
| `python tools/release.py` | the Windows release zip (refuses any game file) |

[AGENTS.md](AGENTS.md) is the guide to working on the code and [docs/](docs) holds the documentation: how the game
works inside ([docs/game-internals.md](docs/game-internals.md)), decisions, progress, and this fork's work list
([docs/FORK.md](docs/FORK.md)).

## Credits and license

HD Hands and HD Hud are by willzmkv, this fork's author. The port and its recompiler are the work of [gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp),
built on [PS2Recomp](https://github.com/ran-j/PS2Recomp). GPL-3.0, inherited from PS2Recomp. Grand Theft Auto and all
game content are property of Rockstar Games / Take-Two Interactive.

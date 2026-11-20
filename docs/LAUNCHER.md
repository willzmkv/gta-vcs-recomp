# Launcher and releases

`VCSLauncher.exe` (Linux: `VCSLauncher`), shipped in every Windows release, takes a player from "extracted the zip" to "playing" without a
terminal: it finds and checks the disc, installs what the build needs, recompiles and builds the game with a progress
bar, and starts it. It is a front end over the Python tools in `tools/` - every button runs a script people can also
run by hand - so the launcher and the README's manual install are one pipeline.

Status (2026-11-01): rebuilt as a native app of its own - its own window, Direct3D 11 renderer, DirectWrite text and
animated UI, after uppies - with a rethought flow (Setup, Home, Task); before, SDL3 + Dear ImGui (2026-10-30, checked
end to end from a release zip in a fresh folder, below). Windows and Linux (2026-11-01: X11 and Wayland, Vulkan,
FreeType - "Linux" below; released as an AppImage with each Windows release since 2026-11-03, "The Linux release").
Decisions:
DECISIONS.md 2026-10-30 "A launcher for releases", 2026-11-01 "The launcher draws itself", "The launcher on Linux".

## Why

The manual install (README) is: Python with *Add to PATH*, Git (Visual Studio with two specific options until 2026-11-03), clone, copy
the `.iso` in under the right name, a terminal, `python tools/build.py --recompile`, then find `build\vcs.exe`. Each
step loses players, and the README's *Problems* table is mostly those steps going wrong. Distribution is source-only
(DECISIONS.md 2026-10-21): the player's PC compiles the game, so the launcher is also a build front end.

## A release

`python tools/release.py` (CI on a `v*` tag: `.github/workflows/launcher.yml`) writes
`build/dist/VCS-Recomp-<version>-windows.zip` (82 MB) and its `.sha256`. One folder inside:

| What | |
|---|---|
| `VCSLauncher.exe` | the launcher, one static exe (4 MB) |
| `VERSION`, `RELEASE.json` | the release's number (the repository's `VERSION`, also built into VCSLauncher.exe's version information; a `v*` tag must match it); its repository, commit and file list (tools/update.py) |
| `python/` | python.org's embeddable CPython (the version that made the release). Not on PATH: the build hands it to CMake (`-DPython3_EXECUTABLE`, `toolchain.python_defines`), which runs `tools/hlsl_dxbc.py` for the shaders - without it a PC with no Python of its own stopped at "Could NOT find Python3" (0.2.1, issues 67-73) |
| `deps/` | the libraries CMake would download (tools/deps.py): SDL3, Dear ImGui, zlib, ELFIO, toml11, fmt, libdwarf, rabbitizer, nlohmann/json, and the FFmpeg LGPL build - 136 MB unpacked |
| `THIRD_PARTY_LICENSES.txt` | their licences, collected from the libraries themselves |
| the source | the repository at the tag (`git archive`; `--worktree` for tests) |

With `deps/` there, `toolchain.configure` points CMake at it (`FETCHCONTENT_SOURCE_DIR_<NAME>`,
`-DFFMPEG_PREBUILT_URL=<zip>`, `FETCHCONTENT_FULLY_DISCONNECTED=ON`): the build needs neither git nor internet - except for
the compiler: the first build downloads llvm-mingw (~190 MB, once; DECISIONS.md 2026-11-03). Nothing is left to
install (with `VCS_TOOLCHAIN=msvc` Visual Studio's compiler, which the launcher installs).

**No game code, ever**: `release.py` checks every file of the release before zipping - names (`*.iso`, `SLUS_*`...),
folders (`generated/`, `build/`, the Ghidra project, memory cards), content (a MIPS ELF header, an ISO 9660 volume
at sector 16, anything over 200 MB) - and stops without writing the zip if one matches. Checked with planted files of
each kind.

**The launcher and the scripts must match.** The launcher is built from the working tree; `--ref` (default HEAD)
packages a commit. With uncommitted changes the two differ - a release made that way (2026-10-30) had the new launcher
next to the last commit's `tools/`, without `doctor.py`, and the launcher showed "build tools missing" because its check
never answered. So `release.py --ref` refuses a working tree with uncommitted changes (commit, `--worktree`, or
`--allow-dirty`), and every release runs its own launcher's `--self-test` (with its bundled Python and its scripts)
before zipping. The launcher itself checks at start that the scripts it runs are in `tools\`, and a check that gives no
answer shows as "Couldn't check" with the reason and *Check again* - never as something missing.

**Every file in the zip has one date, a day before the commit** (`release.zip_time`; `SOURCE_DATE_EPOCH` overrides
it). Zip dates are wall-clock times with no time zone, read in the extractor's zone: 0.0.1, made in Adelaide
(UTC+10:30), extracted in Brazil (UTC-3) with every file up to 13.5 h in the future, and Ninja re-ran CMake until it
gave up (`manifest 'build.ninja' still dirty after 100 tries`, a GitHub issue, 2026-10-31). A day back, written as UTC,
is in the past in every zone. The build repairs a folder that has such files anyway (an old zip, a clock set back):
`build.py` / `recompile.py` set files dated in the future to now before CMake runs (`toolchain.fix_future_files`,
0.3 s; build folder, `generated/`, `.git`, `python/` not looked at), doctor lists them as a folder warning, and a
Ninja loop that still happens fails as `file_dates` ("Files are dated in the future": check the clock). The old dates
are harmless in a fresh folder; `update.py` writes files with today's date, so an update still rebuilds what changed.
Extracting a newer zip by hand over a built folder would not (the sources older than the build): use the launcher's
update or a new folder.

## The Linux release (2026-11-03)

`python3 tools/release.py --platform linux` (CI's `linux` job on a `v*` tag, on Ubuntu 22.04) writes
`build/dist/VCS-Recomp-<version>-x86_64.AppImage` (36 MB) and its `.sha256`; CI's `publish` job puts it on the same
GitHub release as the Windows zip, and only when both were made (DECISIONS.md 2026-11-03).

| In the AppImage | |
|---|---|
| `AppRun` -> `usr/bin/VCSLauncher` | the launcher (2.4 MB: the C++ runtime linked in; needs glibc 2.35+) |
| `gta-vcs-launcher.desktop`, `.png`, `.DirIcon` | the desktop entry and icon (the name is the Wayland app id) |
| `usr/share/vcs-recomp/release/` | the release's files as in the zip, without Python (the system's `python3`) and FFmpeg (the system's): the source, `VERSION`, `RELEASE.json`, `deps/` (Linux's list: with Vulkan-Headers, volk and VMA; `deps.py --platform linux`), `THIRD_PARTY_LICENSES.txt` (with the AppImage runtime's) - 174 MB, 5,316 files unpacked |

- **The install folder.** The image is read-only and the game is built inside its release folder, so at start the
  launcher compares the image's `RELEASE.json` with the one in `$XDG_DATA_HOME/VCSRecomp/install` (`VCS_INSTALL_DIR`)
  and, when they differ, runs the image's `tools/update.py install --from <image> --to <folder>` as a task ("Putting
  the game's files in place") before anything else; the command-line forms wait for it. Files whose bytes are the same
  are left alone (their dates too), so an update rebuilds only what changed. From then on everything runs in that
  folder, as in a release folder on Windows: doctor, build, `build/`, `generated/`.
- **Updates** (`update.py check` finds the `-x86_64.AppImage` asset; `apply` downloads it, checks the SHA-256 and the
  AppImage magic, and renames it over `$APPIMAGE`): the Restart button starts the new image, as on Windows ("Restart
  after an update" below), then "Update the build".
- **The shortcut** points at the AppImage file, not where it happens to be mounted.
- **Made with** appimagetool 1.9.1 and the static type 2 runtime 20251108 (no libfuse2 needed), downloaded once into
  `build/dist/cache` and checked against pinned SHA-256s. Without FUSE, `--appimage-extract-and-run` (CI does that).
- **Checks before it is written**: the same game-file guard as the zip (on the release files), the launcher's glibc
  need (the floor 2.35: `--allow-new-glibc` for a local test on a newer system), and the AppImage's own
  `--self-test` started as a player would, which puts its files into a fresh folder first.

## The player's flow

One window with a title bar the launcher draws itself, and always one of three modes; moving between them is where the
motion is (2026-11-01, the user's "rethink everything"; before, a sidebar of pages - DECISIONS.md 2026-11-01).

- **Setup** (`launcher/screens/setup.cpp`), until the game can be played: a rail of the four steps (Folder, Disc,
  Build tools, Game: their titles in Pricedown when installed, each with a line under it) whose line fills in teal as
  they are done, and one step at a time below it in a large card - a pink label, a title, a line of explanation, the
  step, its state as a badge in the card's top right corner, and a footer whose primary (hot pink) button is the
  step's next thing. A step that turns good plays its check (a stroke that draws itself), holds 0.9 s, and the next
  slides in from the side it comes from. Behind the step shown sits a solid pink box, as in Vice City's menus: each
  step's is a quadrilateral of its own (its corners set off the step's text, each differently), and going to another
  step carries the box there while its corners blend from the old shape into the new one (`render::quad`, a filled
  convex quadrilateral). Any step can be looked at from the
  rail. The Folder step only stops Setup when doctor has a warning for it and nothing is built yet.
- **Home** (`home.cpp`): the name over Vice City at dusk (`backdrop.cpp`, drawn by the renderer: the icon's striped sun
  behind the beach's art deco towers, a hazier row behind them with smaller, dimmer windows, the sea with their
  reflection; stars at night). It moves, gently: the sun's stripes slide into the horizon, cloud streaks come and go
  across the sun (one every 10 s or so fades in somewhere around it, drifts and fades out: three to five at a time), far-off jets cross high up, at irregular times, each on a line of its own (through a random point of the upper
  sky at up to 24 degrees, in from the left, the right or the top; dimmed behind the words and the title bar) and with its
  own pace and distance drawing trails that spread, drift and fade over half a
  minute, windows go on and off and the tallest tower's light blinks, lines roll in on the water and the sun's reflection
  wavers - drawn at 30 frames a second (15 unfocused), only while Home shows, never while the game runs, and still with
  reduce motion on. In front of it,
  one big button for what's next (Play, or Check again when the check failed; with an update ready Play stays and a
  smaller *Update the build* sits under it with a pink NEW badge, why in its tooltip - offered, never forced), what was built under it ("Built 2 days ago ·
  Standard · AVX2"), and three cards along the bottom: Setup (opens it at any step), Saves, Updates. While the game runs
  the button is a "Playing · 12:34" panel (redrawn once a second, never continuously); a non-zero exit becomes a red
  card with `gameExitText()` and the log.
- **Task** (`task.cpp`): a build, the installer, a disc copy, an update, a report or a restore takes the window: a
  large percentage whose digits roll like an odometer (the one leaving blurs and fades as it rises, the one arriving
  comes up out of a blur into focus), a bar with a sheen, the time so far and left, the stages as a
  timeline (spinners that turn into checks or a cross), Cancel (asks first), the log in a drawer that rises from the
  bottom (Ctrl+L, or *Show details*; it opens by itself on a failure). A failure slides in a card with a red edge that
  shakes once: the code's title and fix (`errorHelp` in `launcher/model/tasks.cpp`), *Try again*, *Open the log*,
  *Make a report*. Success: the check, what happened, *Play* after a build.
- **Sheets** (`sheets.cpp`): Saves, Settings and Help from the title bar slide in from the right on a spring over a
  dimmed window; Escape, the dim or the button close them. **Dialogs** (`overlays.cpp`: restore a backup, cancel a
  task, quit during a task, the chosen build folder's check) scale up and fade in; **toasts** rise at the bottom and
  leave after 6 s.
- Keyboard: Enter is the highlighted action, Escape closes the top sheet or dialog, F1 Help, Ctrl+L the log, Tab /
  Shift+Tab walk the controls with a focus ring. *Reduce motion* in Settings: everything snaps and Home's scene
  stands still. Off until the player turns it on (`reduce_motion` in launcher.ini). Windows' "Animate controls and
  elements inside windows" has no say: it is off on many PCs tuned for speed - the dev machine's too - and first made
  the launcher snap, then (as the starting value) made new installs start with reduce motion on (2026-11-01).
- Light and dark (`ui/theme.h`): hot pink is the primary colour - the thing to do, the selection - on a deep black
  (dark) or on white and blush pink (light); teal only for done, amber and red only for attention, no purple. *Appearance*
  in Settings: System (the default: Windows' app mode, `AppsUseLightTheme`, followed live on `WM_SETTINGCHANGE`), Light
  or Dark (`theme=` in launcher.ini). Every colour is a field of a palette; the two palettes crossfade over a quarter
  of a second when the choice or Windows' mode changes, and the window's own frame (DWM dark mode, Windows 11's border)
  switches with them. The icon (`launcher/make_icon.py`, which writes `launcher.ico`) is Home's backdrop in a rounded square: the
  striped sun behind the beach's towers, the horizon's line and the sun on the sea, in the dark palette. Each size is
  drawn for itself: 16 and 20 px only the sun, its stripes and the sea; towers from 24 px; up to 64 px every edge and
  stripe on whole pixels; lit windows and stars only at 256. The same script draws vcs.exe's icon (`src/vcs.ico`): the
  scene by day - a white sky, an orange-to-pink sun, rose towers, no stars - so the game and the launcher don't look
  alike in Explorer or on the taskbar.
- The first frame: the last doctor answer (`build/launcher_state.json`) gives the right mode at once; while this run's
  check is out, only "fine" is believed (anything else shows as checking) and the big button waits.

What each step and sheet holds:

- **Folder** (tools/doctor.py `check_folder`): the **build folder** - `build\` next to the source, or one the
  player chooses (*Choose another folder*: a folder dialog, then tools/build_dir.py `check` says what it would mean -
  the folder used, free space, warnings, or why not - and *Build here* runs `set`; *Use the default* goes back) - and
  the source folder. Warns for either under Program Files, in a OneDrive / Dropbox folder, on a network drive, in a
  path over 60 characters or with non-ASCII letters; less than 3 GB free on the build folder's drive; memory under
  8 GB.
- **Disc**: the launcher's own check (`launcher/model/disc_check.cpp`, the same as vcs.exe's through
  `src/disc/vcs_disc_id.h`): SYSTEM.CNF's boot file, then the SHA-1 of `SLUS_215.90` - about a second. What a player can
  act on: another release ("the European release of Vice City Stories (SLES_546.22)", "Liberty City Stories (USA)"),
  not a PS2 disc, a `.bin` / `.chd` / `.cso` / archive with how to convert it. A drop zone that answers while a file is
  dragged over the window (OLE drag and drop: it glows and breathes), the file dialog, a search of Downloads / Desktop /
  Documents / its folder (one level deep, DVD-sized `.iso` files, Vice City Stories names first), or **Copy from a DVD
  drive** (tools/rip_disc.py). The choice goes into `build\vcs.ini` as `iso=` (vcs.exe's settings keep lines they don't
  own); a relative `iso=` is read against the build folder, then the source folder.
- **Build tools**: the processor (doctor's `check_cpu`: which build this PC gets - AVX2, AVX or SSE4,
  `toolchain.cpu_level`; a CPU without SSE4.2 can't build: `cpu_unsupported`, a red card), and tools/doctor.py
  `check_compiler`: llvm-mingw there (`build/build-tools`, `VCS_LLVM_MINGW`) or downloaded by the first build - always
  ok, `toolchain: "mingw"`, and the step's text says so. With `VCS_TOOLCHAIN=msvc`: vswhere, one `-requires` query per
  component - `-path` ignores `-requires`; the clang-cl the build would take: scoop's LLVM, then Visual Studio's, then
  PATH; `toolchain: "msvc"`; missing parts: *Install build tools* runs tools/setup_vs.py.
- **Game**: built and current, or the reasons not (doctor's `check_build`: no exe, generated/ stale, no / other
  `build_info.json`, source files newer than vcs.exe). Build type: Standard (ThinLTO) / Quick / Fastest (PGO), a
  segmented control; unset, the current build's type is kept.
- **Play**: `build\vcs.exe <iso>`, working folder `build\`, no console, output to `build\run\launcher_game.log`, in a
  job that outlives the launcher. The launcher minimises (or closes, or stays: *Settings*) and comes back when the game
  ends; a non-zero exit is explained from vcs.exe's exit codes (`vcs_disc_id.h`: 2 no disc, 3 unreadable, 4 wrong disc,
  5 no window / graphics / sound; `0xC...` a crash, with the log).
- **Saves**: the card's folder, Back up now, the backups with Restore (asks first; tools/backup.py). The list includes
  the game's own `auto-*` backups, made before it deletes or overwrites a save (game-internals.md 10f).
- **Settings**: what the launcher does when the game starts, updates (check now, check at start, Update now when one
  is found - a dot on the title bar's Settings marks it), Appearance (System / Light / Dark), Reduce motion, a desktop
  shortcut.
- **Restart after an update** (2026-11-04): the update's done card offers **Restart** (Enter; the primary button) and
  *Later*. Restart starts the new launcher - `VCSLauncher.exe` in the release folder (this one now runs as
  `VCSLauncher.exe.old`), or `$APPIMAGE` on Linux (the new image renamed over the file) - detached, its output to
  `build/run/launcher_restart.log`, with `--after-update <old pid>`, and closes this one. The new launcher waits for that
  process to end (at most 15 s) before anything else, so its start removes `VCSLauncher.exe.old` and an AppImage's old
  mount is gone; then it is the new version's window (an AppImage puts the new files in place first). After *Later*,
  Home's Updates card reads "Restart to finish" and Settings shows the installed version with the same Restart button.
  `update.py apply` marks a real update with `restart: true` in its result; "already the newest" has none and gets OK.
  Checked on Linux (2026-11-04) end to end: a 0.2.0 AppImage updated to a local 0.2.1 one through Settings, Restart,
  the old process ended at once, the new one opened as 0.2.1 with its files installed. That test found the old mount
  *staying*: the AppImage runtime unmounts once every holder of its keep-alive pipe has closed it, and the new launcher
  had inherited that pipe (fd 3, not close-on-exec) through our fork. The game and anything `openUrl` starts inherited
  it too. Every child now marks the fds above stderr close-on-exec before exec (`closeInheritedFdsOnExec`, Linux's
  `close_range`); with it the old mount and its FUSE process go when the old launcher ends.
- **Help**: Make a report (tools/report.py), the game's last log, the build log, the README, the keyboard, version and
  folder.

Cancel stops the whole tree (the tool runs in a Job Object), and a new build continues where it stopped. The PC is kept
from sleeping while a build or a disc copy runs. Closing during a task asks first.

## The launcher's own code (uppies' design)

A native app of its own, after uppies (D:\aislop\uppies, docs/ARCHITECTURE.md there): no third-party code, one 1 MB
static exe, 17 s from scratch. Layers, each using only those below it:

| Path | What |
|---|---|
| `launcher/main.cpp` | entry, the command-line forms, the frame loop, `--screenshot` |
| `launcher/screens/` | the window: `screens.cpp` (which mode, transitions, the model's readings), `titlebar`, `backdrop`, `setup`, `home`, `task`, `sheets`, `overlays` (dialogs, toasts), `gallery` |
| `launcher/ui/` | immediate-mode widgets with retained animation state (`ui.cpp`: ids, hover / press / focus, input layers, `animate` / `spring`, `wakeAt`, tooltips; `widgets.cpp`: buttons, segmented control, toggle, choice rows, cards, pills, status badges that morph, progress bar, rolling numbers, scroll areas, the log view), `theme.h` (every colour and size) |
| `launcher/render/` | `icons.cpp`: the icons, drawn from distance shapes into the atlas (both OSes); `draw.cpp`: the draw list - one 96-byte instance per primitive (rounded rect with gradient and border, glyph - sharp or Gaussian-blurred, sampled only inside its own texels -, Gaussian shadow, line, arc), base and overlay layers, clip / opacity / offset stacks; `font.cpp`: the glyph atlas (R8, shelf-packed, grows 1024 -> 4096 without moving glyphs, quarter-pixel variants), layout, ellipsis, wrapping; `font_dwrite.cpp` / `font_freetype.cpp`: DirectWrite / FreeType rasterise; `renderer.h`, `d3d11/` (`ui.hlsl`, compiled by fxc at build time) and `vulkan/` (the same `ui.hlsl` through glslang) |
| `launcher/platform/` | `platform.h` (window, input, time, wake, dialogs); `platform_win32.cpp`; `platform_linux.cpp` + `linux/` (`x11.cpp`, `wayland.cpp`, `dbus.cpp`) |
| `launcher/core/` | `math.h` (rects and rect cutting, sRGB colours, `damp`, a sub-stepped `Spring`, easings), `png.cpp` |
| `launcher/model/` | the state and the tools it runs: `tasks.cpp` (App, Task, the `@vcs` events, `errorHelp`, settings), `process.cpp` (Job Objects; output and exit wake the UI), `disc_check.cpp`, `system.cpp`, `json.cpp` |

- **Frame loop**: sleep in `MsgWaitForMultipleObjectsEx` until input, a wake (a tool printed or ended: `Process`'s
  reader thread and a thread-pool wait post `WM_APP`), a time something asked for (`ui::wakeAt`: a tooltip, the
  clocks once a second while something runs) or an animation in flight; then wait on the swap chain's latency
  waitable, pump input again, build the UI, one upload, one draw, present. dt is measured, clamped to 100 ms, one
  refresh after a sleep; animations are `damp` / springs, so motion is the same at 60 and 240 Hz. Win32's modal loops
  (moving / resizing the window, a file dialog) draw through `platform::setModalFrame`; dialogs are only opened
  between frames (`screens::runPending`), never inside one.
- **Rendering**: one instanced draw, no vertex or index buffer (`SV_VertexID`), clipped on the GPU by shrinking the
  quad (out to whole pixels; the pixel shader covers the pixels a clip edge cuts by how much is inside, so a clip that
  moves by fractions of a pixel moves smoothly); the pixel shader evaluates distance functions (uppies' shading, with an arc added and horizontal gradients).
  Flip-discard swap chain, latency 1, tearing when vsync is off, `WS_EX_NOREDIRECTIONBITMAP`, the first frame drawn
  before the window is shown; a lost device is recreated from CPU state (Ctrl+Shift+F10 simulates it); WARP when
  there is no GPU.
- **Text**: DirectWrite (grayscale, natural symmetric, grid-fitted at text sizes), cached per font / pixel size /
  code point / quarter pixel; kerning from the font. Fonts from Windows: Segoe UI Variable Text (Windows 11) else
  Segoe UI, Bahnschrift for the name and the big numbers, Cascadia Mono else Consolas for the log. Nothing bundled.
  **Icons** are no font (since 2026-11-01, both OSes): `render/icons.cpp` draws each from a few distance shapes on a
  24-unit grid (strokes with round caps, arcs, discs, rounded rectangles, polygons) at the exact pixel size into the
  glyph atlas - one glyph instance to draw, overlapping strokes never darker where they meet, axis-aligned 1-pixel
  strokes snapped to pixel centres below 25 px. `theme::icon`'s Segoe code points are only their identifiers now. **Pricedown** (GTA's lettering, Typodermic) for the few prominent things - the
  name on Home, Home's big button, the build percentage - when the player has it installed (`render::kFontBrand`,
  `fontFound`), else the display font. Its desktop EULA lets each person install it, not ship it in software or in
  open-source projects (4.1, 4.3, 4.6), so it is looked up by name and never in the repository or the exe
  (DECISIONS.md 2026-11-01). `VCS_BRAND_FONT=<file>` uses a font file of the player's own instead (tests).
- **Chrome**: an ordinary overlapped window whose whole area is client (`WM_NCCALCSIZE`), `WM_NCHITTEST` for the
  resize edges, the caption (drag, double-click, system menu, snapping) and `HTMAXBUTTON` over the maximise button
  (Windows 11's snap layouts); dark mode, Windows 11 corners and border through DWM attributes; the DWM shadow from a
  one-pixel frame extension.

### Linux (2026-11-01)

The same launcher; what differs is in files of its own (DECISIONS.md 2026-11-01 "The launcher on Linux"). Built with
`python3 tools/build.py --launcher` (the packages: docs/LINUX.md; `--check-deps` names them), `VCSLauncher` in the
repository root, ~1 MB stripped, 27 s from scratch on the dev laptop.

- **Window**: X11 (Xlib) or Wayland (libwayland-client), picked at startup - Wayland when `WAYLAND_DISPLAY` is set,
  else X11; `VCS_LAUNCHER_VIDEO=x11|wayland` forces one. The title bar is ours on both: X11 gets an undecorated window
  (Motif hints), Wayland client-side decorations (xdg-decoration asked for them; GNOME has none anyway). A press on the
  caption or a resize edge (6 logical px) goes to the window manager / compositor - `_NET_WM_MOVERESIZE`,
  `xdg_toplevel.move` / `resize` - so it moves, snaps and tiles as every other window; double-click maximises,
  right-click on Wayland opens the compositor's window menu.
- **Waiting**: `poll()` on the display connection, an eventfd that `wake()` writes (tool output, a process ending) and
  Wayland's key-repeat timerfd - no polling, no CPU while idle.
- **Scale**: X11 from `Xft.dpi` (`VCS_LAUNCHER_SCALE` overrides); Wayland from fractional-scale-v1 with viewporter (the
  buffer has physical pixels), else the output's integer scale.
- **Input**: keys as Windows virtual keys (platform.h's contract) from the unshifted keysym; text through the X input
  method or xkbcommon with compose (dead keys); key repeat Wayland's own rate. Pointer shapes: Xcursor's theme on X11,
  cursor-shape-v1 on Wayland (else libwayland-cursor). Dropped files: XDND / `wl_data_device` (`text/uri-list`).
- **Desktop settings**: dark / light from the portal's `color-scheme`, else the GTK theme's name (portal, or XSETTINGS'
  `Net/ThemeName` on X11 - XFCE, MATE, Cinnamon); animations (`Gtk/EnableAnimations`, `enable-animations`) and the
  double-click time likewise. Read at start and when the window gets focus (XSETTINGS: when it changes).
- **Rendering**: Vulkan 1.1+, opened with `dlopen("libvulkan.so.1")` (no link to the loader; no driver: a message). One
  pipeline, the instance buffer as a per-instance vertex buffer, push constants, the atlas by its dirty rectangle
  through a staging buffer; two frames in flight with persistently mapped buffers; FIFO with vsync, else MAILBOX /
  IMMEDIATE; `VK_KHR_present_wait` for one frame of latency where the driver has it. Integrated GPU first, then
  discrete, virtual, CPU (lavapipe: CI) - `VCS_LAUNCHER_VK_DEVICE=<part of the name>` picks one. A lost device is
  rebuilt (Ctrl+Shift+F10 simulates it). `VCS_LAUNCHER_VK_DEBUG=1`: the validation layer, messages as `[vulkan]`.
  Offscreen (`--screenshot`, `--hidden`) needs no display at all.
- **Text**: FreeType (grayscale, light hinting - vertical only, so the quarter-pixel positions hold) from fonts
  fontconfig finds: Inter, Cantarell, Noto Sans, Ubuntu, DejaVu Sans, `sans-serif`; Bahnschrift / Barlow Semi Condensed
  / Noto Sans Display for the name; Cascadia / JetBrains / Noto Sans / DejaVu Sans Mono for the log; Pricedown when
  installed.
- **Processes** (`model/process_posix.cpp`): fork + execvpe; a tool leads a process group of its own, so Cancel and
  closing the launcher stop the tree (SIGTERM, SIGKILL after 2 s), and `PR_SET_PDEATHSIG` stops it if the launcher
  dies; the game gets a session of its own and plays on. A waiter thread in `waitpid` and a reader in `poll` wake the
  UI. A death by signal N is exit code 128 + N.
- **The rest** (`model/system_linux.cpp`, `platform/linux/dbus.cpp`): XDG folders (saves in `$XDG_DATA_HOME/VCSRecomp`,
  as the game), `xdg-open`, the file manager over D-Bus (`FileManager1.ShowItems`), a `.desktop` shortcut on the
  Desktop and in the applications menu (icon: `launcher/launcher.png`, from `make_icon.py`), logind's sleep inhibitor
  during a build, file dialogs from the portal (else zenity, else kdialog), errors before the window as a zenity box
  when no terminal shows them.
- **Setup's build tools step**: doctor.py lists the missing packages and the command; *Install packages* runs
  `build.py --install-deps` (pkexec: the desktop's password prompt); *Install in a terminal* opens one with the same
  command where there is no polkit agent; on Debian / Ubuntu releases without clang 19+ (22.04, Debian 12) the
  package step adds apt.llvm.org first (`tools/linux_packages.py`, the only thing that runs as root). Copying a DVD is
  hidden (`rip_disc.py` is Windows only); update checks show in an AppImage, not in a clone.

## How it fits together

```
VCSLauncher.exe (launcher/: main.cpp, screens/ ui/ render/ platform/ core/, model/ tasks process disc_check system json)
   ├─ itself: window, disc check, file / folder dialogs, vcs.ini's iso=, processes (Job Objects), shortcuts, the game
   └─ python\python.exe -I tools\<tool>.py ...  with VCS_PROGRESS=json
         doctor.py --json     the state: folder, memory, cpu, compiler, downloads, disc, build, "next"
         build.py --recompile-if-needed <iso> --progress json [--opt X]
         setup_vs.py install, backup.py create|list|restore, report.py, update.py check|apply, rip_disc.py list|rip
```

Python is the release's `python\`, else a `python.exe` on PATH (not the Microsoft Store stub), else `py -3` - each
tried first (a py.exe can exist with no Python behind it).

**Events** (tools/toolchain.py): with `--progress json` / `VCS_PROGRESS=json` the tools print, between their usual
lines, `@vcs {...}` lines: `{"stage": id, "title": text}`, `{"progress": n, "total": n, "eta": s, "note": text}` (at most
4 a second), `{"error": code, "message": text, "log": path}` (from `tc.fail(message, code, log)`;
`configure_failure` / `build_failure` read the code from the log; a tool that crashed: `exit_failure` -
`illegal_instruction`, `missing_dll` - and its log ends with how it ended, `describe_exit`), `{"result": "ok", ...}`.
Everything else is log.

**The game's link** (`toolchain._GameLink`, 2026-11-01): with ThinLTO the link of vcs.exe is the longest step of a
build - the whole game is optimised and compiled to machine code there (133 s of a 5-minute rebuild on the dev PC),
while Ninja's count stands at its last step ("423 of 425 steps, about 0s left"). Ninja prints a step when it *ends*
(when its output is a pipe), so the link's start comes from a file it touches first: a PRE_LINK command of
ps2EntryRunner writes `game_link.started` in the build folder, and build.py's progress timer looks for it every
second. Then it sends a `link` stage ("Linking and optimising the game (ThinLTO)") and the link's own progress: the
time so far against the last link of the same kind (`link_times.json` in the build folder, per `thin` / `full` /
`pgo`), held at 99% until it ends, `"note": "linking"`; with no earlier link, `total` 0 (an endless bar and "one long
step"). Past the last time's length the eta is null ("longer than the last time"). Quick builds (no LTO) link in
seconds and get no stage.

**State files**: `generated/recompile_stamp.json` (a hash of what the generated code depends on: game/vcs, the
recompiler, the runtime headers it compiles in, the translate / sync scripts; `recompile.py --check`),
`build/build_info.json` (source version, generated inputs, optimisation, disc; written by a successful game build),
`build/launcher.ini` (the launcher's options), `build/launcher_state.json` (doctor's last answer, for the first frame).

## The tools (all usable without the launcher)

| Command | What |
|---|---|
| `python tools/doctor.py [--json] [--iso X]` | every check and the next step |
| `python tools/build_dir.py [check PATH / set PATH / reset]` | where the game is built. PATH: a new or empty folder is used as it is, a build folder made before is used again, any other folder gets a `VCS-build` folder inside; refused inside the source folder (other than `build\`), on a file, at the top of a drive, where it can't be made or written (tried for real: the top of C: allows folders, not files). Kept in `.vcs-build-dir` (git-ignored; `VCS_BUILD_DIR` for one run); every tool follows it through `toolchain.BUILD`, CMake puts vcs.exe at its top (`VCS_GAME_DIR`), `vcs.ini` and `launcher.ini` come along on `set`. The old folder is left as it was. A tree set up from another source folder gets a fresh CMake cache (`toolchain.drop_foreign_cache`). generated/ stays in the source folder |
| `python tools/build.py --recompile-if-needed [ISO] [--progress json]` | recompile only when stale, then build |
| `python tools/build.py --launcher` | build the launcher (build/launcher, the exe in the root; no disc) |
| `python tools/setup_vs.py status / install [--dry-run]` | Visual Studio Build Tools 2022 + clang: nothing there - Microsoft's bootstrapper (aka.ms, Authenticode-checked: valid and Microsoft's, or it isn't run) with the C++ workload, recommended parts and the clang components; a Visual Studio without clang / CMake - its installer's `modify --add` (the components only: the workload's ID differs between products); clang too old - `update`. Elevated via ShellExecuteEx `runas`; 1223 = declined, 3010 = restart needed |
| `python tools/backup.py create / list / restore NAME [--force]` | the card zipped with a manifest of every entry's creation / modification time; restore backs up the current card first, recreates in creation order and puts the times back (SetFileTime); refused while vcs.exe runs |
| `python tools/report.py [OUT]` | build logs, the newest four game logs (crash reports included), state files, doctor's JSON, Windows / CPU / GPU names; never the disc, generated code or the card |
| `python tools/update.py check / apply [--zip F]` | GitHub's latest release vs RELEASE.json; apply replaces the release's files, removes the ones the new release dropped, keeps build/ and generated/ and anything added; files in use (the launcher, the running Python) are renamed `*.old` and removed next time, a file held open is tried again for a few seconds; a file it still can't write is skipped and named, `VERSION` / `RELEASE.json` stay the old ones and it fails with `update_incomplete` (the launcher offers the update again; issue 66); checks the `.sha256`; its result says `restart: true` when it put a new release in place (the launcher's Restart button) |
| `python tools/rip_disc.py list / rip E: OUT.iso` | a disc's sectors (the size its ISO 9660 volume gives) to an .iso, retries, then the check |
| `python tools/deps.py list / fetch [DIR] / check [DIR]` | the FetchContent pins read from the CMake files (one list); nlohmann/json from its release archive (0.2 MB instead of a 188 MB clone) |
| `python tools/release.py [--ref vX / --worktree] [--version X] [--launcher EXE]` | the release zip |
| `python tools/vcs_disc.py check ISO [--json]` | the disc check, for scripts |

The launcher's command line: `--check`, `--build [--iso X]`, `--play [--iso X]`, `--self-test`, `--out FILE` (a GUI
exe prints to the terminal that started it only through AttachConsole; `--out` for scripts and CI), and
`--screenshot F.png`: the window rendered offscreen into a texture and saved - tests never put a window on the desktop.
`--screen` picks what is shown (`setup:folder|disc|tools|game`, `home`, `task`, `sheet:saves|settings|help|folder`,
`dialog:restore|quit|cancel`, `gallery` - every widget in its states), `--demo` a sample state (`running`, `link`,
`firstlink`, `error`, `done`, `playing`, `update`, `updated` - an update in place with Restart / Later, `gameerror`,
`stale` - Home with the build out of date), `--after-update PID` waits for PID to end first (the Restart button's), `--then S --at T` switches after T seconds and `--frames N` saves N
consecutive frames (a transition, frame by frame), `--scale K` / `--size WxH` the pixels per unit and the logical size,
`--theme light|dark` the palette (dark when not given, whatever launcher.ini or Windows say), `--clock S` Home's moving
scene S seconds further on (its jets, clouds and windows at a given moment: the stills otherwise land wherever the
doctor's run left the clock);
`--step N` (the old page numbers), `--action build --every S`, `--iso X`, `--wait S`, `--pick-build-dir PATH` as
before. Screenshots run on a fixed 60 Hz clock, so the same arguments give the same picture. `--hidden --quit-after S`
runs the real window and frame loop without showing it and prints the frames drawn and the CPU used (the idle cost);
`--stats` (or Ctrl+Shift+F11) overlays frame times.

## Checked (2026-11-01, the native launcher)

- `--self-test` (the renderer and fonts included, offscreen): ok.
- Every mode, step, sheet, dialog and demo state as offscreen screenshots at scale 1 and 1.5, `--screen gallery`, and
  transitions frame by frame (`--frames`: Home's entrance, a sheet sliding in).
- With the real disc: verified on the Disc step, Setup moved on to Game by itself; a relative `iso=` is found.
- A full build through the hidden launcher from an empty build folder (`--action build`, Quick): every stage on the
  timeline, the rolling percentage with Ninja's steps, done in 9 min 10 s with *Play* offered; afterwards Home with
  *Play* and "Built just now".
- The real window loop, unshown (`--hidden --quit-after 30`): 67 frames in 30 s (the entrance and doctor's run), 0.4 ms
  per frame, 0 ms of CPU measured outside them.
- **Not checked**: the visible window by hand (drag, snap, maximise, DPI moves, a drop from Explorer), Windows 11.

## Checked (2026-10-30)

- `--self-test` in the dev tree and in the release folder (repository, Python - the bundled one there -, doctor's
  JSON, the disc checks).
- Disc checks: the USA disc ok, the Liberty City Stories (USA) disc named as such (both by vcs_disc.py and the
  launcher), a text file, a missing file, a `.chd` name.
- Release: `release.py --worktree` 1 min (deps cached), the guard refusing a planted disc image, MIPS ELF, generated
  file and SLUS name. From the zip, extracted into an empty folder: the launcher's first start (Disc step), then
  `--action build`: every step green, *Play* (see "Numbers"); `--play` started the game (hidden, VCS_DATA_DIR).
- Update: 0.1.1-test applied over a 0.1.0-test folder: version, a dropped file removed, `build\vcs.ini` and a player's
  own file kept, the in-use Python files set aside and gone at the next run; a clone refuses (`git_checkout`); the
  GitHub check answers "no release published yet" (404) for now.
- Backups on a copy of the card (VCS_DATA_DIR): create, a slot file deleted, restore - files and both times identical.
- setup_vs.py: status; `install --dry-run` for no Visual Studio, missing clang, old clang (the commands it would run);
  the bootstrapper downloaded and its signature accepted, a tampered copy refused. **Not run for real** (it installs
  ~6 GB on the dev machine).
- rip_disc.py from an image file: a byte-identical copy in 10 s; the LCS image refused after copying. **No DVD drive on
  the dev machine**: reading `\\.\E:` itself is untested.
- 2026-10-31, from a player's report ("A tool could not run" at *Configuring the build*, a Python traceback): with
  Build Tools only, cmake is in Visual Studio's own folders, on the PATH `build_env` makes but not on the launcher's.
  The checks look on the former, Windows starts a child's program from the latter (CreateProcess ignores the `env=`
  PATH): FileNotFoundError. `toolchain.resolve` now gives cmake its full path (configure, `cmake --build`, the IOP
  tests), and a program that can't start is a `fail` (`no_cmake`), not a traceback. Checked with cmake taken off the
  process PATH: the old code fails like the report, the new one runs. The dev machine never saw it: scoop's cmake is
  on its PATH.
- 2026-10-31, CMake and Ninja for any setup (`toolchain.build_tools`, DECISIONS.md): checked on the dev machine with
  every CMake / Ninja hidden but VS 2019's (CMake 3.20, too old: skipped with its reason; Ninja 1.10.2 taken), the
  launcher built from an empty build folder with a downloaded CMake 4.4.5; with VS 2019's Ninja hidden too, both
  downloaded and built; the downloads deleted under a configured tree: set up again with scoop's and built. Failures
  end in a `fail`, never a traceback: no network (`no_cmake`), a wrong SHA-256 (`download_failed`), a `VCS_CMAKE`
  that's missing or too old (`no_cmake`). The four Linux archives were downloaded, matched their pins and unpacked;
  **the Linux search and the downloaded programs running there are untested** (no Linux machine).

## Checked (2026-11-03, the Linux AppImage)

On the dev laptop (Mint 22.3, i7-6600U, HD 520), an AppImage made by `release.py --platform linux --worktree
--allow-new-glibc`:
- Its own self-test (through FUSE, as a player starts it): the files into a fresh folder, all checks ok. 36 MB, 37 s
  to make (the Linux `deps/` fetched the first time), 5,316 files / 174 MB inside, no `build/`, `generated/` or disc.
- The release refuses a launcher that needs glibc 2.38 without `--allow-new-glibc`.
- **End to end**, a fresh install folder: the files in place (a task in the window, ~3 s), then `--build` with the
  disc: recompile and a ThinLTO build of the game in **18 min** from the start, nothing downloaded (`deps/`); the
  generated C++ identical to the clone's (only `recompile_stamp.json` differs). `--play` with its own `VCS_DATA_DIR`:
  the intro movie (321 pictures, 0 dropped), the attract mode; the launcher returned when the game ended.
- **Update**: `update.py apply --appimage <0.1.3>` replaced the running 0.1.2 AppImage file; its next start put 8
  changed files in place (the edited docs, `VERSION`, `RELEASE.json`, and zlib's `zconf.h`, which zlib's configure
  moves aside) and `--build` took **1.7 s** in all: no recompile, two Ninja steps.
- The real window: first start shows "Putting the game's files in place" with its progress, then setup at the disc.
- CI (2026-11-03, manual run 41540234607, Ubuntu 22.04): the launcher built with 22.04's own compiler and the pinned
  wayland-protocols, needing **glibc 2.35**; self-test and validated screenshots on llvmpipe; the AppImage made
  (appimagetool without FUSE) and self-tested there. The same AppImage on the dev laptop: checksum, self-test through
  FUSE on the HD 520, the window - all ok. On real 22.04 doctor chose LLVM's repository for the old clang
  (`--llvm-repo jammy:20`). Two fixes from CI's first runs: `build.py --launcher` checked the game's packages too, and
  a fresh checkout had no `build/` for the configure log.
- Not run: the AppImage on an older distribution's desktop; apt.llvm.org added for real; a download from a real
  GitHub release (the first is v0.2.0).

## Checked (2026-11-01, Linux)

On the dev laptop (Mint 22.3, XFCE on X11, i7-6600U, HD 520, Mesa 25), the launcher built by `build.py --launcher`:
- `--self-test`: all ok (`renderer: Vulkan 1.4 (Intel(R) HD Graphics 520 (SKL GT2)), FreeType`).
- `--screenshot` of Home, the gallery, the setup steps, Settings and Help, with `VCS_LAUNCHER_VK_DEBUG=1`: the same
  pictures as Windows' (fonts aside: Noto Sans), **0 validation messages**.
- X11, in a nested Xephyr with xfwm4 driven through XTest: caption drag moves the window exactly, the right edge resizes
  it exactly, double-click maximises and restores, minimise hides it (`_NET_WM_STATE_HIDDEN`), a click opens Settings,
  the close button quits. The real window on the desktop: undecorated, centred, class `VCSLauncher`, the icon set.
- Wayland, in a nested weston (X11 backend inside Xephyr, pixman, lavapipe) driven the same way: move, resize,
  maximise, close; fixed on the way: un-maximising kept the maximised size (a 0x0 configure now means the floating size).
  On the desktop's weston (GPU): HD 520, 60 Hz pacing, one draw call.
- Not checked: GNOME / KDE Wayland by hand, a drop from a file manager, the portal's file dialog clicked for real, the
  shortcut, `--install-deps` for real (it installs packages), the Windows build (UTF-8 interfaces, the icons) - CI
  compiles and self-tests it on the next push.

## Numbers

| | |
|---|---|
| Launcher build from scratch | **17 s** (configure 5 s, 28 steps 12 s; 2026-11-01); was 81 s with SDL3 + Dear ImGui |
| VCSLauncher.exe | **1.0 MB**, static, no third-party code (was 4 MB) |
| Idle cost (`--hidden --quit-after 20`) | 62 frames in 20 s - the entrance animation and the doctor's run - then none; 94 ms of CPU in all (before Home moved) |
| Home, moving (2026-11-01) | 783 frames in 20 s - the entrance, then ~33 a second; 0.16 ms of CPU a frame (about 0.5% of one core); every other screen still draws nothing when idle |
| A frame | 0.67 ms CPU on average (the first ones, rasterising glyphs: up to 14 ms); one draw call, a few hundred instances |
| Linux AppImage | 36 MB; made in 37 s; first start puts 174 MB in place in ~3 s; the game built from it in 18 min (laptop, ThinLTO); an update's start + rebuild 1.7 s |
| Linux: build from scratch (laptop, clang 20, ThinLTO) | **27 s**; `VCSLauncher` 1.0 MB stripped (the X11 icon is 0.17 MB of it) |
| Linux: idle (reduce motion, `--hidden --quit-after 20`) | 7 frames in 20 s (the entrance and the doctor's answer), then none; **5 ms of CPU in all** |
| Linux: Home, moving (`--hidden --quit-after 10`, paced at 60 Hz) | 346 frames in 10 s (~33 a second after the entrance); 0.96 ms a frame on the laptop at its idle clock (0.33 of it the renderer: upload + submit); 40 ms of CPU a second |
| `doctor.py` | 0.8 s |
| Release zip | 82 MB; 1 min to make with the libraries cached, +17 s to fetch them |
| First build from the release, through the hidden launcher (i7-7700K, 8 threads, ThinLTO, offline) | **8 min 42 s**: configure 52 s (with the recompiler), MIPS -> C++ 36 s, configure again 41 s, compile + link 6 min 25 s (786 steps) |
| The release folder after it | 1.13 GB (build/ 0.75, generated/ 0.20, deps/ 0.12, python/ 0.02) |
| The built game started by `--play` (hidden, a test card) | boots from the disc, the intro movie 321 pictures, 0 dropped |

## Not done / next

- Linux: `rip_disc.py` on Linux; a clipboard (the terminal
  fallback stands in for *Copy the command*); xdg-foreign for the portal dialog's parent on Wayland; GNOME / KDE checked
  by hand.

- A run on a PC with nothing installed (Windows Sandbox: no Python, no Git, no Visual Studio) - the real test of
  setup_vs.py's install path.
- Code signing: SmartScreen warns about an unknown publisher (README says *More info > Run anyway*). An open-source
  signing service (SignPath's OSS programme) would remove that.
- The CI workflow is written, not run (it runs on the next push to GitHub); the first `v*` tag publishes the first
  release.
- Linux: the launcher's platform file, a Vulkan backend for its renderer, FreeType for its glyphs, the process /
  folder layer in `model/`, and a Linux release (an AppImage-style bundle needs a different Python story).
- The window itself, by a person: dragging, snapping, maximising, moving between monitors of different DPI, dropping
  an .iso from Explorer, how the motion feels (tests stay hidden). Windows 11's snap layouts flyout and rounded
  corners are untested (the dev machine is Windows 10).
- Art from the player's own disc behind Home (read at run time, never shipped): the backdrop has the slot.

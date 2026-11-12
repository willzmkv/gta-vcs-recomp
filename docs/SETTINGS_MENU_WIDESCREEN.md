# Settings menu and widescreen - plan

Status: **v1 built** (2026-10-22): spike done, settings menu and Hor+ widescreen (4:3, 16:10,
16:9) in; see "What v1 built" below for what differs from the plan.

Two pieces, built in that order:
1. An in-game **settings menu** of our own (not the PS2 game's menus), opened with a key binding,
   built so that new settings are one registration each.
2. **Widescreen** support as the menu's first real setting.

## Findings this plan is based on

From the user's ELF (`build/elf/SLUS_215.90`; addresses are in the game's code, names are ours):

| What | Where | Notes |
|---|---|---|
| FOV global | `0x487484` (float) | written by `FUN_002653e0` (a `CDraw::SetFOV`), read by the camera code at `0x21cd98`, `0x21ce90`, `0x21ec4c`, `0x21f9ac` and others |
| Aspect-ratio global | `0x487488` (float) | written only by `FUN_002653f0` (a `CDraw::CalculateAspectRatio`); read by the camera (`0x21cdcc`, `0x21cf20`, `0x21ec60`) and 2D code (`0x39fd2c`: world-to-screen with 640/448 and 448.0) |
| Aspect choice | `FUN_002653f0` | widescreen pref on: 16/9 (5/3 when `[0x6f4d14]` is set); off: 4/3 (1.25 when set). `[0x6f4d14]` is the camera's m_WideScreenOn (cutscenes; found 2026-11-06, game-internals.md 5a), not a PAL/NTSC flag |
| Widescreen preference | `*(int *)(*(0x48F3D0) + 0x30)` | read through the getter `FUN_002af658` (`lw v0, 0x30(a0)`); the object at `[0x48F3D0]` is created by `FUN_00470fa8` if missing (a menu manager / prefs object - 533 loads of that pointer) |
| HUD awareness | 106 calls of `FUN_002af658` | the first ones (`0x1147e0`, `0x1149e8`, `0x11589c`) sit next to the HUD's own 4/3 constants (`0x1147f8`, `0x114a18`, `0x1158ac`): the HUD already adjusts for widescreen |
| Camera view window | `FUN_001ab9e0(camera, flag, tan(fov/2), aspect)` | called at `0x21cdd4` with the aspect global; at `0x21f9d0` with a fixed 16/9 (a second camera - to identify) |

So **VCS has its own widescreen mode** (as the PS2 release did, for 16:9 TVs): with the preference
set it renders a horizontally squeezed (anamorphic) 640-pixel picture with a wider view and a HUD
laid out for it, expecting the TV to stretch the picture to 16:9. The game's camera, culling and
2D projection all follow the aspect global. The plan uses that path instead of patching
projection matrices, and treats other aspect ratios as an extension of it.

Also found: **our presentation shows the 640x448 frame at 1.43:1** (square pixels). A PS2 frame is
meant to be shown at 4:3 (non-square pixels), so today's picture is ~7% too wide. The widescreen
work fixes that too ("Display aspect" below).

## Spike result (2026-10-22) - the plan's widescreen route changes

Forcing the game's preference (`VCS_WIDESCREEN=1`, src/game/vcs_display.cpp) and logging the
globals showed:
- The writes stick (preference 1; aspect 16/9 in the frontend, 5/3 in game - `[0x6f4d14]` is 1 in
  game and 0 in the frontend, so it is not a PAL flag but a frontend / in-game switch; in 4:3 the
  aspect is 4/3 / 1.25).
- The FOV global is the same in both modes (45 frontend, 120 flyover, 70 in game).
- `FUN_001ab9e0` = set the camera's view window: x = tan(FOV / 2), y = x / aspect, raster
  640x448, through `FUN_0018f2d8` (RwCameraSetViewWindow). So the game's widescreen is **Vert-**:
  same horizontal view, vertical view cut by 25%, for a TV that stretches the picture.
- The HUD does not change with the preference (the radar keeps its 4:3 shape); the preference's
  readers are 3D-projected sprites (e.g. `0x1147e0`: size without the 4/3 factor).
- The camera at `0x21f9c8` (same camera object `[0x51E490]`) gets a fixed 16/9: probably the
  letterboxed cutscene view.

New route for 16:9 (and other ratios): **Hor+ through the view-window setter**. Replace
`FUN_001ab9e0` with a wrapper that widens the horizontal extent by r = display aspect / (4:3) and
keeps the vertical one (tan * r, aspect * r); RenderWare derives the frustum (EE culling) and the
VU matrices from the view window, so 3D, culling and both renderers follow. The game's own
preference stays off. The renderer squeezes 2D draws (no depth test, not full width) by 1/r so the
HUD keeps its shape (in the middle 4:3 of the screen), and frames without 3D (loading screens, the
pause map) are shown at 4:3, pillarboxed.

## What v1 built (2026-10-22)

- `src/settings/settings.{h,cpp}`: the registry (Toggle / Choice / Integer / Key; Live / Restart),
  values as ints, vcs.ini read / write (unknown lines and comments kept, temp file + rename, only
  non-default values appended), environment overrides per setting (read at load, shown greyed out,
  never written back), an `alias` for older keys (`scale=` -> `display.scale`) and a `read` hook
  for settings a hotkey also changes (F3 overlay). No separate ini module: it is ~60 lines.
- Saved state that is not a setting: `settings::state` / `setState` keep text values in vcs.ini
  without a menu entry. The window placement uses it (2026-10-23): `window.display` (monitor name),
  `window.display_x/_y` (that monitor's desktop origin, to tell two of one model apart),
  `window.x/y/width/height` (the restored client area relative to the monitor) and
  `window.maximized`. The platform layer creates the window hidden, places it (kept inside the
  monitor's usable area, frame on screen; the default place when the monitor is gone), then shows
  it; it reports a new placement 0.5 s after the last move / resize and at shutdown, so a killed
  process (tools/run.py) keeps it too. Hidden windows (`PS2X_HIDDEN`) are neither placed nor saved.
  Checked on the dev machine's two mixed-DPI monitors: moved, maximized, restored, each reopened at
  the same rectangles (Win32 GetWindowPlacement before / after).
- `src/ui/settings_menu.{h,cpp}`: the menu, drawn in the runtime's debug-UI callback. Tabs per
  page (LB / RB or Q / E switch), a two-column table per page, the selected row's help line,
  "applies after a restart" under changed Restart settings. F2 (`controls.menu_key`) or pressing
  Back + Start together toggles it (no half-second hold: the press is enough, see below); Escape /
  B closes it (not while a combo list is open). Dear ImGui keyboard / gamepad nav is switched on
  only while the menu is open.
- Input: `ps2control::setGameInputBlocked` - `physicalInputOn()` is false while the menu is open
  and after it closes until every key / button is released (so the B or Escape that closed it
  never reaches the game).
- Settings (main.cpp `registerSettings`): Display > Aspect ratio (`display.aspect` = 43 / 1610 /
  169, env `VCS_ASPECT`), HUD (`display.hud`, env `VCS_HUD`: 0 keep 4:3 / 2 screen edges / 1 stretch), Internal resolution
  (`display.scale`: 1-8 or 0 = Match window, the native frame; restart, env `PS2X_HW_SCALE`); Graphics > Performance overlay; Controls >
  Settings menu key.
- Widescreen: the Hor+ route from the spike - `vcs_display.cpp` wraps `FUN_001ab9e0`, the presenter
  shows 3D frames at the display aspect (2D-only frames - loading screens, the pause menu - at
  4:3), the HW renderer squeezes the HUD by (4:3) / aspect. What tells them apart (found with frame
  dumps, `PS2X_HW_DUMP`):
  - RenderWare keeps ZTE on for 2D (ZTE=0 is prohibited on the GS) and uses ALWAYS or GEQUAL, so
    "no depth test" finds nothing. VCS draws its whole HUD, text, radar and menus at the **largest
    Z** (0xFFFFFF; 0.99999994 after scaling, not 1.0), while projected sprites (clouds, coronas,
    the mission arrow's glow) take the same RwIm2D path at their real depth. HUD = at the far
    plane, not full width (sky, horizon band, fades), not sampling a render target (the colour
    filter is drawn in strips from the frame itself).
  - The radar map and menu labels are clipped by the scissor: squeezed draws get a squeezed
    scissor.
  - 3D = a depth-tested draw (GEQUAL / GREATER) not at the far plane, or a native draw. The game
    draws 3D in every other VBlank (30 fps), so "3D" is judged over the last 32 frames: 4 or more
    3D -> wide, none -> 4:3, otherwise unchanged. The game thread reads the same flag to decide
    whether to squeeze (a frame or two late at a switch).
  - Checked with `tools/scenes.py capture` at 16:9 and 4:3: gameplay HUD in the middle 4:3 with
    a round, clipped radar; flyovers and the intro cutscene wide (the cutscene keeps its own
    letterbox bars inside the 16:9 picture); loading screen and pause map at 4:3. Not yet looked
    at: vehicles' HUD, the phone / mission text boxes, the frontend menus over a 3D background.
- **HUD: Screen edges** (milestone 3 step 2, 2026-10-23): each HUD element keeps its 4:3 shape but is squeezed towards
  the screen edge it sits near (radar left; clock, weapon, money, zone name right; help text left; centred text in the
  middle) instead of all towards the middle. The game names its own HUD primitives (`src/game/vcs_hud.cpp`, found by
  their vertex positions; game-internals.md 6f), so the "far plane = HUD" heuristic only covers 2D outside CHud::Draw's
  frame. `gsHwSetHudClassifier` in the renderer; the pivot is x 0 / 640 / 320 of the 640-wide frame. Since 2026-11-01
  the radar and centred text lines are known by who draws them, not grouped by position: the radar always at the left
  edge, centred lines (subtitles, mission text, stunt results) always in the middle - before, bottom text near the
  radar jumped between left, middle and right (game-internals.md 6f "Radar and text", `tools/hud_check.py`).
- **Entity culling** (2026-10-27): the claim "RenderWare derives the frustum from the view window, so culling
  follows" held for the world sectors only. Buildings, peds and cars go through `CCamera::IsSphereVisible`, whose
  side planes the game builds from the FOV global (4:3); they vanished in the outer eighths of a 16:9 picture.
  `vcs_display.cpp` now widens those planes to the Hor+ edge too (game-internals.md 5; `VCS_WIDE_CULL=0` = before).
- **Ultrawide** (2026-10-27, asked for by the user): Aspect ratio 21:9 (`219`, exactly 21 / 9), 32:9 (`329`) and
  Match window (`0`: the window's shape, clamped to 4:3 .. 32:9, followed each host frame as the window is resized or
  goes full screen - for monitors such as 3440x1440 (2.39) or 2560x1080 (2.37) that are not exactly 21:9). Nothing
  else had to change: the Hor+ widening, the entity culling, the presenter's letterbox, the HUD squeeze / anchoring and
  the post effects' buffer widths all take the ratio as a number. Limits: narrower than 4:3 is shown letterboxed
  (Hor+ only widens; a portrait window would need Vert+), wider than 32:9 is not checked. At 32:9 the horizontal view
  is 124 degrees (tan 35 x 2.67), so near objects at the sides stretch - rectilinear projection, as in any game.
  Checked at 21:9 (2520x1080 frame) and 32:9 (3840x1080), Internal resolution Match window, HUD Screen edges: the
  reference scenes (flyover, cutscene with its bars inside the picture, gameplay HUD at the edges, loading screen and
  pause map at 4:3 pillarboxed), two AI drives through the city (buildings, traffic and peds out to the edges, no
  unstreamed cells - the streaming follows the camera's cell, not the view), entity planes at 32:9 edge 1.867 = view
  window. Cost (tools/bench.py, 144 fps setting, GTX 1080 Ti): the game thread about the same as 16:9 1920x1080; the
  render worker roughly doubles with the pixels and the extra draws (busiest spot city_1325_-325: worker 4.5 -> 10.3
  ms, 131 -> 97 fps; drive 113 -> 103 fps).
- **Integer resolutions in widescreen** (2026-10-27, the user's request): 1x-8x render the picture's targets as many
  times wider as the view (640 x scale x r by 448 x scale: 1x at 16:9 853x448, 3x at 32:9 5120x1344, at most 8192
  wide) through the native frame's per-target scales; before, 640 x scale was stretched across (2.67 times softer
  across than down at 32:9). 4:3 unchanged. `PS2X_HW_WIDE_SCALE=0` = before. Cost follows the pixels: tools/bench.py
  at 32:9 3x (4x MSAA, GTX 1080 Ti) 63-85 fps instead of 113-144 - a 32:9 3x is a 5120-wide picture; 1x / 2x stay
  cheap, Match window renders at the window's own size.
- **Radar streak at 32:9** (2026-10-27, reported by the user): a thin column of radar map right of the circle. The HUD
  layer squeezed the HUD before snapping it to PS2 pixels, and at a squeeze of 0.375 a PS2 pixel is 6 screen pixels:
  the map's scissor and its corner mask rounded to different ones. Now snapped at the 4:3 positions, then squeezed
  with the scissor exactly (NATIVE_RENDERER.md "HUD layer"). Checked on the reference scene that showed it.
- **4:3 frames inside the game's frame** (2026-10-27, the user's decision): frames without 3D (the pause menu, loading
  screens) stay 4:3 but are placed inside the rectangle the game's picture takes - the window letterboxed at the
  chosen ratio - instead of the whole window (`present::fitRect`; the window, screenshots and the HUD layer's output
  size use it). A 32:9 game on a 16:9 monitor shows its pause menu in the 32:9 strip between the bars, not at the
  window's full height. Intro movies keep the whole window. At Match window or 4:3 nothing changes (the frame is the
  window).
- **The pause menu switches at once** (same day): before, a frame was shown 4:3 only after 32 frames without 3D, so for
  half a second the menu's full-width backdrop was stretched across the wide picture, then the picture snapped to 4:3
  and for a frame or a few the menu was drawn 25% narrower (62.5% at 32:9) and popped back (the user's "resizes and
  scales up"). Why: each draw read the 3D flag as the main thread updated it, while the presenter used the newest
  value - frames recorded for a wide picture were shown at 4:3. Now (gs_hw.cpp): a frame keeps the flag it first read
  (`frameWide`), the presenter shows each frame with its own (`showing3d` = the shown frame's), and a frame with the
  game's menu primitives and no 3D (the classifier ors `GSHwAnchorMenu` into the anchors of FUN_003b53f8's
  primitives) empties the 3D history: 4:3 from the first menu frame. `PS2X_HW_MENU_SWITCH=0` = the 32-frame wait as
  before. Checked frame by frame (screenshots ~50 ms apart) entering and leaving the pause menu at 32:9 on a 16:9
  screen, 16:9 and 4:3, and the reference scenes at 32:9 on a 16:9 screen (loading screen and pause map in the strip).
- **No shifted frame at the switch** (same day, reported by the user: "for a frame the menu and the game view shift").
  Every presented frame captured (`burst`, below) showed, leaving the menu, about four host frames of the game squashed
  into the 4:3 box, then one wide frame with the HUD unsqueezed. Two causes:
  - **VCS shows the previous frame**: its finished picture (FBP 140) is copied into the displayed buffer (FBP 0) at the
    start of the next frame, so at a switch the picture on screen is the other shape's. The shape now belongs to the
    picture: each frame buffer keeps the shape of the frame that last drew into it (HUD-layer draws included - the pause
    menu draws nothing else), a plain copy carries it along like the HUD layer, and the window uses the displayed
    buffer's (`RhiExecutor::shapeOf`).
  - **The shape came from a history the frames had not caught up with**: decided when each frame is executed from all
    of it (`decideShape`): a menu frame without 3D is 4:3 at once, and the first 3D frame after it wide at once (before,
    4 frames with 3D were needed). The HUD layer's squeeze is recorded in any widescreen frame and applied when the
    frame is executed and its shape known; only draws squeezed into the buffer itself (unclassified 2D at the far plane)
    still go by the recording-time guess.
  Checked with `burst` (every presented frame, 50 entering and 50 leaving) at 32:9 three times, 21:9 and 16:9 on a 16:9
  screen: every frame's shape matches its picture (the build before: 3 of 100 did not) and the HUD is the same in the
  first gameplay frame as later; the reference scenes at 32:9 unchanged. Regression check: `tools/regress.py` step
  "menushape" (fails with `PS2X_HW_MENU_SWITCH=0`: 29 frames of the menu stretched, 3 of the game squashed).
- **The game's own Widescreen option retired** (2026-11-06, the user's report: turning it on in the pause menu removed
  the cutscene bars). Everything it does is in game-internals.md 5a: the PS2's anamorphic mode (Vert- 16:9; HUD, text
  and pause map 3/4 wide; `_WIDE` menu layouts), which squeezes everything a second time next to the Hor+ view and the
  HUD squeeze, plus no cutscene letterbox. It is now kept off (`vcs_display.cpp` hooks its setter, so a save's value
  is not taken), its row on the pause menu's Display page is hidden (`vcs_frontend.cpp`: the rows below move up into
  its place, up / down skip it), and the bars are Display > Screen > **Cutscene black bars** (`display.cutscene_bars`,
  default On, `VCS_CUTSCENE_BARS`; Off skips the game's letterbox draw `FUN_002804c8`). At 4:3 it is locked On - the
  row greyed out showing On, the saved value kept for widescreen: `Spec::locked` / `lockedValue` / `lockedNote`
  (settings.h), for any setting another one decides; `main.cpp` `cutsceneBarsLocked()`. DECISIONS.md has why;
  `tools/menu_check.py` "display" and `tools/regress.py` "widepref" check it.
- **Scope / camera mask** (same day): the black mask around the sniper scope, the camera and binoculars was squeezed
  into the middle 4:3 with the scope, so the world showed at the sides in widescreen; its side bars now reach the
  screen edges (game-internals.md 6f "Scope mask", regress step "scopemask").
- Not done from the plan: window mode, pausing,
  control-channel `menu` / `set` commands (environment overrides cover scripted captures:
  `VCS_ASPECT=169 python tools/scenes.py capture ...`).
- Gamepad chord: opening on the Back + Start press means the game may see Start for one frame
  (and open its pause menu) before the block applies. A hold time would not avoid that either; a
  later fix is to delay Start to the game by a frame while Back is held.

## Menu v2 (2026-10-24): stable layout, pages and sections

Reported by the user: hovering between two rows made the menu switch between two states every other frame, and
clicking the new "Skip intro movies" toggle did nothing. Causes and fixes (`src/ui/settings_menu.cpp`):
- **Layout feedback loop.** The window was `AlwaysAutoResize` and centred with pivot 0.5 every frame; the help line
  below the table showed the hovered row's help (one to four wrapped lines) or nothing. Hovering a row grew the window,
  re-centring moved the rows up under the still mouse, the row was no longer hovered, the help vanished, the window
  shrank, and so on - two layouts alternating. A click whose press and release fell in different layouts was lost
  (ImGui buttons and checkboxes fire on release over the same item). The "applies after a restart" line inserted
  under a changed Restart row moved the rows below it too.
- Now: the window size comes from the viewport only (0.92 x width up to 720 px, 0.88 x height up to 540 px, both
  times the menu scale); the rows scroll in a child of the remaining height; the help area is as tall as the longest
  help text of any setting at the current width, so nothing changes size with the selection or the page. The restart
  note is a `*` after the label and one footer line that always keeps its space.
- **Help is sticky**: it shows the row last hovered (only when the mouse moves or clicks, so a resting cursor doesn't
  fight the keyboard) or navigated to with keyboard / gamepad, highlighted, and stays when the mouse is between rows.
- **Whole-row hit area**: a row counts as hovered across its label, control and the gap; clicking anywhere on a
  toggle's row flips it.
- **Locked settings** (set by their environment variable for this run) show the variable beside the greyed label
  (`Skip intro movies (VCS_SKIP_MOVIES)`) and say how to unlock them in the help: `tools/run.py` sets
  `VCS_SKIP_MOVIES=1` for test runs, which locks that toggle - a click that does nothing by design looked like a bug.
- The menu window takes keyboard focus back when it lost it (a click on the game picture), except while a combo's
  list is open (focusing the window would close it). A key capture is cancelled by clicking elsewhere or by B.
- The F3 overlay draws on Dear ImGui's background draw list (under windows; it covered the menu's first tabs).

Pages and sections (`settings::Spec::section`; pages in order of first registration, sections likewise, rows in
registration order; the vcs.ini keys did not change):

| Page | Section | Settings |
|---|---|---|
| Display | Screen | Aspect ratio, HUD (Cutscene black bars since 2026-11-06) |
| Display | Frame rate | Game timing, Frame rate cap |
| Graphics | Image quality | Internal resolution, Anti-aliasing (MSAA), Texture filtering |
| Mods | Mod loader, Installed mods, Folder | Load mods (`mods.enabled`), Replace textures (`graphics.texture_replacements`), one switch per mod (`mods.<name>`) with its priority slider beside it (`mods.<name>.priority`: a Toggle's `partner` Integer, kept in modloader.ini - `saved = false`), Rescan, Open folder (docs/MODS.md) |
| Game | Start-up | Skip intro movies (`display.skip_intro`) |
| Controls | Menu | Settings menu key |
| Advanced | Diagnostics | Performance overlay (`graphics.perf_overlay`) |

Checked in the game with real input (a ctypes script moving the cursor and pressing keys in the window - it must be
DPI-aware, and arrow keys need scan codes and the extended flag or SDL sees keypad keys): a cursor parked on a row
boundary gives the same layout frame after frame; clicking a toggle's label flips it and writes vcs.ini; arrows move
the selection and the help follows; Enter opens a list, Escape closes the list, a second Escape the menu; Q / E
switch pages.

## Full screen (2026-10-27)

Display > Screen > **Window mode**: Windowed / Full screen (`display.window=0|1`, `VCS_FULLSCREEN=0|1` for one run),
and **Alt+Enter** switches at any time, also with a menu open; the choice is saved like any setting.
- Full screen is borderless at the desktop's resolution on the monitor the window is on (SDL3's fullscreen with no
  display mode, `platform::setFullscreen`). No mode change: switching and Alt+Tab are instant, other monitors stay as
  they are, and the D3D11 flip-model swapchain just resizes. The Internal resolution / Match window aspect follow the
  new size like any resize. Exclusive full screen with a chosen display mode is not offered (DECISIONS.md).
- The window's place (vcs.ini `window.*`) stays the windowed one: moves and resizes while in full screen are not
  recorded, so leaving full screen, or starting windowed next time, goes back to where the window was. A start in full
  screen places the window first, so it covers the monitor it was last on.
- The mouse cursor hides after 2 s without moving in full screen (Dear ImGui's cursor updates paused while hidden);
  it comes back on any move or click.
- Enter is Start on the keyboard: with Alt held the pad leaves it out (`ps2_pad.cpp`), and the trainer does not take it
  as "apply", so Alt+Enter only switches.
- Hidden test runs never rewrite vcs.ini (`settings::keepFile`): before, nothing in a hidden run changed a setting;
  Alt+Enter through the control channel (`key alt down`, `press return`) would have.
- Checked hidden (the window itself is never made full screen in a hidden run): Alt+Enter twice from gameplay switches
  the setting to full screen and back, the game stays in gameplay both times (no pause menu), Enter alone still opens
  it; same with `VCS_FULLSCREEN=1` (switches for the run only); vcs.ini byte-identical afterwards.

## Player settings (2026-10-28): controls, sound, presets

Asked for by the user ("substantially flexible settings that players can change"). Pages now: Display, Graphics,
Audio, Game, Keyboard, Gamepad, Advanced.
- **Display**: *Presets* first - Authentic (PS2) (the Display / Graphics defaults, but original timing at 30 fps: the default frame rate is 60 with frame-rate independent timing since 2026-10-30), Enhanced (Match window aspect,
  HUD at the screen edges, Match window resolution, 4x MSAA, 16x AF, independent timing at the display's refresh) and
  Widescreen, light (Match window, 1x, no MSAA, 4x AF, independent at 60). A preset sets only those keys; window
  mode, controls and sound stay. *Frame rate*: **Vertical sync** (`display.vsync`: Off = the flip model's present
  at once, never torn, as before; On = sync interval 1; Off, allow tearing = `DXGI_PRESENT_ALLOW_TEARING` for VRR and
  the lowest delay, Off where the system can't; `VCS_VSYNC`), **Show frame rate** (game frames per second over half a
  second, in a chosen corner).
- **Audio**: **Master volume**, **Movie volume** (0-100%, squared to a gain so the steps sound even), **Speakers**
  (stereo / mono / swapped). **Music volume** and **Sound effects volume** are the game's own levels (its pause menu's
  Music / SFX Volume, 0..127 as percent: defaults 63% / 88% = its 5 / 7 bars): a change here shows in the pause menu
  and a change there comes back here and into vcs.ini; a save load keeps the player's levels (game-internals.md 9c,
  `VCS_GAME_VOLUME=0` leaves the game's levels alone). Added 2026-10-28 (asked for by the user); a Voices volume was
  removed again the same day (DECISIONS.md).
- **Game**: **In the background** - keep running (as before), mute, or pause (the game thread held at the frame kick,
  vibration stopped, sound muted).
- **Keyboard** / **Gamepad**: the bindings and stick, mouse and vibration options (docs/CONTROLS.md).
- **Advanced**: **Reset all settings** (asks for a second click).
- **Enhancements** (2026-11-07, after this list was written): **Button prompts** (game-internals.md 6n) and, under
  *World*, **Traffic density**, **Pedestrian density** and **Draw distance** (100% = the original, up to 300%;
  `VCS_TRAFFIC` / `VCS_PEDS` / `VCS_DRAW_DISTANCE`; game-internals.md 6o / 12d, `src/game/vcs_world_detail.cpp`).
  Under *Combat* (2026-11-08): **Drive-by fire rate** - As on the PS2 (default) / The gun's own rate
  (`VCS_DRIVEBY_RATE=0/1`; game-internals.md 12c, `src/game/vcs_driveby.cpp`).

Registry additions (`settings::Spec`): `Type::Action` (a button: `button` text, `confirm` for a second click; no
value, nothing in vcs.ini, skipped by applyAll and the page reset); Key `optional` (0 = unbound; right-click clears),
`group` (exclusive keys: a control's binding takes the key from another control, a menu key refuses keys a control
has and is never taken), `partner` (a second slot drawn in the same row; the partner Spec is `hidden`). Binding
capture waits until every key / button is let go, then takes any named key, mouse button or wheel notch; while it
waits, and until the bound button is released, Dear ImGui gets no mouse (`ImGuiConfigFlags_NoMouse`), so a mouse
button bound on its press doesn't click what is under the cursor when released.

Layout: the help area is the longest help's height but at most four lines; a longer help is drawn smaller (down to
60%) to fit, so the rows keep about two thirds of the window whatever the texts. The "Keyboard & mouse" tab was cut
off at 4:3: the page is "Keyboard" (its "Mouse" section holds the mouse settings).

Checked: `tools/controls_check.py` (hidden; 21 checks: every default binding in the pad report, Vic moving on a
rebound key and not on the old one, walk, a wheel binding, the presets, Reset all settings, input back after the
menu, the game's vibration reaching the host) and screenshots of every page (`menu <page>`, `menu scroll`); regress
step "controls". Not checkable hidden (no window): the mouse camera's feel, the background pause / mute, vertical
sync, the menu driven by a real mouse.

## Part 1 - the settings menu

### What the player sees

- **F2** opens / closes the menu (default; rebindable in the menu, stored in vcs.ini). On a
  gamepad: hold **Back + Start** for half a second (a chord, so neither button's game meaning is
  lost). Escape / Circle (gamepad B) closes it.
- A centred window over the game: a list of pages on the left (Display, Graphics, Controls,
  Advanced), the selected page's settings on the right. Keyboard, mouse and gamepad navigation
  (Dear ImGui's keyboard / gamepad nav).
- Each setting shows its value, a one-line description of the selected item, and a marker when it
  needs a restart ("applies after restart"). "Reset page to defaults" at the bottom of each page.
- While the menu is open the game gets no physical input (it keeps running in v1; see "Pausing").
- Changes apply immediately where possible and are saved to vcs.ini right away.

### Architecture

```
 src/settings/   settings.h/.cpp   registry: typed settings, values, defaults, change callbacks
                 ini.h/.cpp        vcs.ini read / write (keeps unknown keys, comments and order)
 src/ui/         settings_menu.h/.cpp   the menu: input routing, layout generated from the registry
 src/game/       vcs_settings.cpp  VCS-specific settings (widescreen, ...) and what they do
 main.cpp        registers everything once at startup, hands the runtime a UI draw callback
```

- **Registry, not hand-written screens.** A setting is declared once with its metadata; the menu
  draws whatever is registered, grouped by page and section. Adding a setting = one
  `settings::add({...})` plus its apply callback; no menu code changes.

  ```cpp
  namespace settings
  {
      enum class Type { Toggle, Choice, Integer, Number, Key, Action };
      enum class Apply { Live, Restart };
      struct Choice { const char *label; int value; };
      struct Spec
      {
          const char *key;         // "display.aspect" - also the vcs.ini key
          const char *page;        // "Display"
          const char *section;     // optional group heading inside the page
          const char *label;       // "Aspect ratio"
          const char *help;        // one line, shown for the selected item
          Type type;
          int defaultValue = 0;    // Toggle / Choice / Integer / Key
          float defaultNumber = 0; // Number
          int min = 0, max = 0, step = 1;  // Integer (Number: as floats)
          std::vector<Choice> choices;     // Choice
          Apply apply = Apply::Live;
          std::function<void()> onChange;  // called after a change (Live) or ignored (Restart)
          std::function<bool()> visible;   // optional: hide when not applicable
      };
      void add(Spec spec);
      int value(const char *key);                       // and number(), for Number
      void set(const char *key, int value);             // validates, saves, notifies
      void load(const std::filesystem::path &ini);      // before anything reads a value
      void subscribe(const char *key, std::function<void()> fn);
  }
  ```
- **One source of truth for values**: vcs.ini (already holds `iso=` and `scale=`). Keys use
  `section.name`. Environment variables keep winning for testing (`PS2X_HW_SCALE` over
  `display.scale`), shown in the menu as "set by environment" and greyed out.
- **Writing vcs.ini**: rewrite the file keeping lines we do not own, update / append ours, write to
  a temporary file and rename (no half-written ini after a crash).
- **Rendering**: Dear ImGui is already up (platform layer context, RHI renderer, drawn over the
  game every frame - the F3 overlay uses it). The menu draws in the runtime's existing UI hook:
  `PS2Runtime::setDebugUiCallbacks` (draw callback per host frame; our `main.cpp` does not use it
  yet). A small style pass (larger font, game-like colours) - Dear ImGui loads TTF fonts if we
  want one later.
- **Input routing**: `ps2control::physicalInputOn()` is a fixed value today; it becomes "not hidden
  and not injected-only **and no menu open**" (an atomic flag the menu sets), so keyboard, mouse
  and gamepad stop reaching the PS2 pad while the menu is open. The menu itself reads input through
  Dear ImGui (SDL3 backend already feeds it) - gamepad nav needs `ImGuiConfigFlags_NavEnableGamepad`.
- **Key binding**: `controls.menu_key` (a `Key` type setting: the menu's key-capture widget waits
  for the next key). Debug keys (F3, F4, F12) can move into the registry the same way later.
- **Pausing**: v1 leaves the game running with input blocked (simple, no risk). v2: pause the EE
  thread at its next VBlank wait (the scheduler already sleeps there) and keep the IOP / audio
  ticking so the sound driver doesn't starve - a separate, testable step.
- **Live vs restart**: settings that change GPU resources the renderer allocates once (internal
  resolution today) start as `Apply::Restart`; making them live is per-setting work later.
- **Testing**: `tools/scenes.py` gets `--set key=value` (written to a temporary vcs.ini / passed as
  environment) so captures can compare settings; the control channel gets `menu open|close` and
  `set key value` commands for scripted checks of the menu itself.

### First settings (v1)

| Page | Setting | Values | Apply |
|---|---|---|---|
| Display | Aspect ratio | 4:3 (original), 16:9, 16:10, 21:9, Match window | live |
| Display | HUD | Game layout / Keep 4:3 in the middle (for non-16:9 ratios) | live |
| Display | Internal resolution | 1x (original), 2x-8x, Match window (the native frame) | restart |
| Display | Frame rate | 30 (original), 60 (needs a fast CPU) | live |
| Display | Anti-aliasing (MSAA) | Off (original), 2x, 4x, 8x | restart |
| Display | Texture filtering | Bilinear (original), Anisotropic 2x-16x | restart |
| Display | Window | Windowed / Borderless fullscreen | live |
| Graphics | Native renderer | On / Off (`PS2X_HLE`) | restart |
| Graphics | Performance overlay | On / Off (F3) | live |
| Controls | Menu key | key capture | live |

v1 ships the framework with Display > Aspect ratio (4:3 and 16:9), Internal resolution and the
overlay toggle; the rest are registrations once their behaviour exists.

## Part 2 - widescreen

### Modes

- **4:3 (original)**: today's picture, but presented at the correct 4:3 display aspect.
- **16:9**: the game's own widescreen mode (preference on), presented stretched to 16:9.
- **16:10, 21:9, Match window**: the game's widescreen mode with the aspect global overridden to
  the target ratio; presented at that ratio.

### How each part works

1. **Game side - the preference.** Write `*(int *)(*(0x48F3D0) + 0x30)` (1 / 0) when the setting
   changes and whenever the object is (re)created or the game loads its settings from the memory
   card (it would otherwise restore the saved value). Simplest robust form: a per-frame check in
   `vcs_overrides.cpp` (the frame-kick hook already exists) that sets the field if it differs.
   Calling `FUN_002653f0` afterwards recomputes the aspect global at once. Not hooking the getter
   `FUN_002af658`: it is a plain field accessor, so other objects may share it.
   - To check: whether the game's own Display menu (the pause menu has a "display" tab) exposes
     the option. If it does, our setting mirrors it: read the field back and show the game's value
     when the player changed it there.
2. **Game side - other ratios.** Replace `FUN_002653f0` (runtime `replaceFunction`) with a C++
   version: the game's choice for 4:3 / 16:9 (keeping the PAL variants), the setting's ratio
   otherwise. The camera view window, the EE's frustum planes and the VU clip / screen matrices are
   all built from the aspect global, so 3D, culling and both renderers (emulated and native) follow
   without further changes.
3. **Presentation.** The presenter (`render/presenter.cpp`) letterboxes the picture by its pixel
   size today. It gets a display aspect instead: 4:3, or the widescreen ratio, and scales the
   640x448 frame to it (pillar- / letterboxing the rest of the window).
4. **Resolution where it's needed.** In widescreen the game still draws 640 pixels across a wider
   view, so horizontal detail drops (by 25% at 16:9, 43% at 21:9). Give the HW renderer separate
   horizontal and vertical scales (`PS2X_HW_SCALE` -> scale x / y): render targets, scissor, the
   render-target texel scale in the GS shader and the native draws' targets all take a 2D scale.
   With "Internal resolution 3x" at 16:9 the targets become 2560x1344 instead of 1920x1344 and the
   presented pixels are square again.
5. **HUD and 2D.** At 16:9 the game's own HUD layout applies (106 places check the preference). At
   ratios beyond 16:9 the HUD would stretch; "HUD: keep 4:3 in the middle" squeezes the 2D draws in
   the renderer (draws with no depth test in screen space: sprites, text, the radar) by
   gameAspect / displayAspect around the centre. A later step can anchor HUD elements to the screen
   edges by finding the HUD's x-coordinate code around `0x1147e0`.
6. **Full-screen 2D** (loading screens, the legal screen, fades, cutscene letterbox bars): check
   each in widescreen. Loading screens are 640x448 images that would stretch; if the game does not
   handle them, the renderer keeps such full-screen texture blits at 4:3 (pillarboxed).

### Things to verify first (spike, before building the menu)

- Force the preference on (debug env var) and capture the reference scenes: is the picture
  anamorphic with a wider view, does the HUD move, does anything pop in at the edges, how do the
  loading screens, pause menu, radar and cutscenes look.
- Find what the second camera with a fixed 16/9 (`0x21f9b0`) is (radar? cutscene?), and whether
  `[0x6f4d14]` is the PAL flag.
- Check that the guard-band / clip constants (VU data 65, matrices 24-31) change with the aspect
  (they should, being built from the camera).

### Risks / open questions

- The game might also widen the field of view in widescreen differently from a pure aspect change
  (Hor+ vs Vert-); the spike shows which. A separate "Field of view" setting (scaling the FOV
  global through a `FUN_002653e0` override) can be added later.
- Cutscenes may use their own cameras / FOV and the fixed-16:9 camera.
- Ratios beyond 16:9: culling and LOD distances follow the camera, but sector streaming is driven
  by the hex cell around the camera (docs/game-internals.md "Level data"), so very wide views may reach
  unloaded cells at the edges - check at 21:9.
- Memory card: the game saves its preferences; with our setting authoritative the saved value is
  overwritten each session - fine, but the save then stores our value (harmless).

## Milestones

| # | What | Size |
|---|---|---|
| 1 | Spike: force the widescreen preference, capture, answer the questions above | S |
| 2 | Settings core (registry, vcs.ini read / write), menu shell (F2 / Back+Start, pages, input blocking, ImGui nav), control-channel and scenes.py hooks | M |
| 3 | Display aspect in the presenter (fixes today's 1.43:1), Aspect ratio 4:3 / 16:9 through the game's preference | S-M |
| 4 | Separate horizontal / vertical internal scale | M |
| 5 | Other ratios (16:10, 21:9, match window) via the `FUN_002653f0` replacement; HUD "keep 4:3" squeeze; full-screen 2D kept at 4:3 | M-L |
| 6 | More settings as registrations: window mode, native renderer, menu key, overlay | S each |

## Known issues noted for later (from play-testing, 2026-10-22)

Tracked in PROGRESS.md: objects / the world turning black for a few frames, and Vic falling
through the world in some places (collision).

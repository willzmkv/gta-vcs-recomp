# Controls

Keyboard, mouse and a gamepad (anything SDL3 knows: Xbox, DualShock / DualSense, Switch Pro, ...) all drive the
same PS2 controller (DualShock 2, analog mode), so they can be mixed. Every binding can be changed in the settings
menu (F2): the **Keyboard** and **Gamepad** pages (src/game/vcs_input.cpp).

## Default bindings

| PS2 | Keyboard / mouse | Gamepad |
|---|---|---|
| Left stick | W A S D | Left stick |
| Right stick (camera) | I J K L, the mouse | Right stick |
| D-pad | Arrow keys | D-pad |
| Cross | Shift | A / Cross |
| Square | Space | X / Square |
| Triangle | F | Y / Triangle |
| Circle | Ctrl, left mouse button | B / Circle |
| L1 | Q | LB |
| R1 | E, right mouse button | RB |
| L2 | 1, Z | LT |
| R2 | 3, X | RT |
| L3 / R3 | C / V | stick clicks |
| Start (pause menu) | Escape | Start / Menu |
| Select | Backspace | Back / View |
| Walk (hold) | - (unbound) | - (a small stick tilt) |

**While driving** (Keyboard page, Driving) these keys do what a PC driving game does, and nothing else meanwhile; on
foot they keep their bindings above (W walks, Space jumps):

| Driving | Keyboard | PS2 |
|---|---|---|
| Accelerate | W | Cross |
| Brake / reverse | S | Square |
| Handbrake | Space | R1 |
| Lean forward / nose down (bikes, aircraft) | Keypad 8 | Left stick up |
| Lean back / nose up | Keypad 5 | Left stick down |

Steering stays on A / D (the left stick); Shift still accelerates and E / the right mouse button is still R1. The
game's own PS2 controls apply on top. Start or Cross (Escape / Shift) skips the intro movie; the Rockstar logos before
it can't be skipped, as on the PS2 (Game > Skip intro movies skips both).

**Right mouse button = R1**: on foot it locks on to the ped nearest in front while held (with fists or a weapon), as R1
does on the PS2; left mouse button (Circle) attacks / fires. With **Free aim** on (below) it aims a gun instead.

## Free aim

Keyboard > **Free aim** (off by default) and Gamepad > **Free aim** (off), each for its own device (src/game/vcs_aim.cpp,
game-internals.md 12c):

- With a gun in hand (pistols, shotguns, SMGs, rifles, the M249, minigun and flame-thrower), holding the target button
  (R1's binding: the right mouse button / E; on the gamepad R1's button, RB unless changed) puts up the game's own
  white aiming circle in the middle of the screen instead of locking on to the nearest person. The mouse (or the right
  stick, 2 rad/s at full tilt, finer near the centre) turns the aim and the camera with it, 1:1 like the camera on foot
  (Mouse sensitivity, Invert mouse up / down apply; zoomed views turn slower).
- The left button (Circle) fires **through the circle**: a shot hits what is under it, whatever the gun's own line.
  Firing a gun from the hip with the left button aims too (the aim is held 0.6 s after the last shot). Letting go of
  the target button ends the aim at once (the camera leaves it 2 frames later), shots or not.
- **Moving while aiming** is what the game allows locked on with that gun - a pistol walks and strafes; no new movement
  is added. Letting go leaves the camera looking where the aim did. The camera moves into the aim in 0.1 s and out of
  it in 0.2 s, with every gun (the game blends 0.35-0.4 s, and cuts out of the aim with the guns that stand Vic still;
  its other camera changes keep their blends and cuts).
- When the gun runs dry while aiming the game puts the fists in Vic's hand: with the button still held nothing
  locks on until it is let go, and choosing another gun (the wheel) aims again at once.
- Fists, melee weapons, thrown weapons and the scoped guns (sniper rifles, rocket launcher, camera, binoculars) keep
  R1 as on the PS2.
- **Aim assist** (Gamepad, on by default; the gamepad's free aim only): the target button first **locks on** to the
  nearest person, as the game's own aim does (its lock-on camera and marker); pushing the right stick past about a
  third of its throw takes the aim **free from there** (the lock lets go for two frames, then the circle starts where
  the lock-on camera looked) - GTA V's assisted aim. With no one to lock on to after six frames it is free aim at
  once. Off: free aim only (the player's report, 2026-11-12: free aim on the DualSense had lost the lock-on).
- **Free aim reticle** (Enhancements > Combat; Circle by default): **Dot** shows a small white dot with a thin dark
  rim instead of the game's circle with a dot (made narrower in the texture by the picture's widening, so it stays
  round in widescreen - the game stretches the sprite sideways) - the game's own texture (`sitem16`, game-internals.md 12c) rewritten
  in memory while it is on, put back when it goes back to Circle. `VCS_RETICLE=0|1`.
- It is the game's own free aim (R1 then L3 on the PS2: the left stick moves an aiming point while Vic stands still),
  driven so that Vic can move and the mouse aims; `VCS_FREEAIM=0` leaves it out entirely.
- **Shooting from a car** (Jive Drive: Vic in the passenger seat while Lance drives - the game's own free aim, which the
  mission starts itself): with free aim on for either device, the mouse and the right stick turn the circle instead of
  the left stick (the game's help text still says the left stick), no button to hold; the left button fires through
  the circle. On the gamepad **R1 fires** there (as Circle, which still fires too): the right thumb is on the stick.
  Only in this car aim and Phil's shooting range (below) - R1 is the target button everywhere else. The circle keeps its direction while the car turns, up to 20 degrees above level and 5 below, as the game
  allows there. With free aim off it is the PS2's left stick.
- **Phil's shooting range** (the game's free aim forced on foot, `SET_PLAYER_FORCE_FREE_AIMING`): the same - the mouse
  and the right stick turn the circle, no button to hold, the left button fires; the right button does nothing there
  (as R1 on the PS2), and on the gamepad **R1 fires** (as Circle), as in Jive Drive's car. Up to 110 degrees either
  side of Vic's heading, 40 above level and 20 below, as the game allows.
- **A gun on a vehicle** (Farewell To Arms: Vic in the helicopter's door with an M249, the first-person view with the
  circle in the middle): the same again - the mouse and the right stick turn the view, the left button fires, on the
  gamepad R1 fires. Up to 70 degrees either side of the door, as the game allows. With free aim off the mouse still
  turns it (as in the scopes) and the gamepad's left stick does, as on the PS2.

**In the scopes** (sniper rifles, rocket launcher, M16 view, camera, binoculars) the mouse turns the view itself, 1:1,
with Mouse camera on (zoomed in it turns slower). **Mouse wheel** (Keyboard page, on by default): on foot down is the
next weapon and up the previous one (R2 / L2, one per notch); in the sniper scope, the camera and binoculars it zooms;
while driving down is the next radio station and up the one before (the game's own L1 steps forwards only: a step back
is the station written two back and L1 - game-internals.md 12a; nothing happens while the radio plays an announcement,
as with L1). It does nothing of this while a control is bound to the wheel or while aiming.

## Keyboard page

- Every PS2 button, both sticks' directions and **Walk** have two slots. Click a slot, then press a key, a mouse
  button (left, right, middle, 4, 5) or turn the wheel (each notch is a short press); Esc cancels, a right-click on a
  slot clears it. Shift / Ctrl / Alt bind both sides, Enter is also Keypad Enter.
- A key does one thing: binding it to a control takes it from the control that had it. The menu keys (Settings menu
  F2, Trainer F5, Free camera F6, Screenshot F8, same page) never take a control's key, and a control can't take theirs - the menu says which setting
  has it.
- **Walk (hold)**: the left stick at the **Walking speed** (45% by default) while held - GTA walks below about half
  a tilt, which keys can't do otherwise.
- **A driving key** may share its key with an on-foot control (W is Move forward on foot and Accelerate in a car); within
  the Driving section a key does one thing, and the menu keys stay out of both.
- **Mouse camera** (on by default): the mouse turns the camera, and the mouse is held in the window while playing (free
  in the menus and when the window is in the background). **On foot** it turns the camera itself, as in a PC game:
  every movement turns it by the same angle at any speed and frame rate (100%: 400 counts - half an inch at 800 DPI -
  a radian), moving the mouse forward looks up. Looking up / down goes through the game's own stick pitch, which has a
  top speed (about 1.5 rad/s at 100%); a faster movement still arrives, a few frames later. Turned toward a wall, the
  camera is pulled in toward the player, as the gamepad's is (`VCS_FOOT_WALL=0`: it stops at the wall's edge, as
  before 2026-11-11). **In the scopes and free
  aim** it turns the view itself too (above). **Behind a car, bike or boat** it turns the camera itself as well, and
  while the mouse moves the camera stays where it is put - the car neither drags it along nor pulls it back; **Car
  camera return** (1.5 s; 0.5-5 s or Never) after the mouse stops it eases back behind the vehicle (about 2 s) and the
  game's camera carries on. The gamepad's right stick (or the camera keys) takes it back at once, held or not.
  **Car camera sensitivity** (100%, 10-400%) scales its turn on top of Mouse sensitivity.
  `VCS_CARCAM=0` puts the old way back (the mouse as the right stick). **In the other vehicle
  cameras and locked on** the mouse's speed becomes the right stick's tilt (1,500 counts a second is full tilt), so very
  fast flicks there turn no faster. **Mouse sensitivity**, **Invert mouse up / down**, **Free aim** and **Mouse wheel**
  (above). (game-internals.md 12b, 12c)

## Gamepad page

- **Presets**: one click sets the driving / shooting scheme to match a GTA game (it moves only the two trigger options
  and gamepad free aim; the button map and the rest stay, so "Custom" is whatever you set by hand afterwards):
  - **Original (GTA III / VC / SA / VCS)** - the classic PS2 layout those games share: Cross / Square drive, Circle
    fires, R1 locks on; the triggers look around and change weapon.
  - **Modern (GTA IV / GTA V)** - R2 accelerates / fires, L2 brakes / aims, free aim on the right stick (no lock-on),
    look and change-weapon on the D-pad. Best with a DualSense's adaptive triggers.
- **Buttons**: the gamepad input for each PS2 button (any button, LT / RT, stick clicks, D-pad, or none); several PS2
  buttons may share one.
- **Sticks and triggers**: deadzone per stick (12%; the range past it is stretched, so a small tilt beyond it still
  counts), **Camera speed** (the right stick's tilt scaled), **Trigger press point** (LT / RT are L2 / R2 buttons),
  **Swap sticks**, **Invert camera** left / right and up / down, **Drive with the triggers** / **Shoot with the
  triggers** (below), **Free aim** (above).
- **Drive with the triggers** (off by default): while you drive, **R2 accelerates and L2 brakes / reverses** (instead
  of Cross / Square: the pad's Cross / Square then do neither; the keyboard keeps its keys), and look left / right (R2 /
  L2 on the PS2) move to the **D-pad**, its **down** looking behind (L2 + R2). On foot the buttons are
  unchanged. Works on any pad with triggers; it is what makes the DualSense's adaptive triggers (below) act as a live
  throttle and brake.
- **Shoot with the triggers** (off by default): on foot, **R2 fires and L2 aims** (instead of Circle / R1), and change
  weapon moves to the **D-pad** - the modern GTA IV / V layout. In a vehicle the buttons are unchanged. Pairs with the
  DualSense's weapon trigger feel (below).
- **GTA V buttons** (off by default; an option, nothing else changes with it off): more of GTA V's layout on a
  PlayStation pad. The **touchpad** changes the camera (Select on the PS2); while driving **R3 looks behind** (L2 +
  R2), the **D-pad's left / right** change the radio station (previous / next; the pad's L1 no longer does) and its
  **down** starts a sub-mission (taxi, vigilante... - R3 on the PS2). With **Drive with the triggers** on too, **L1 /
  R1 look / aim left / right** (L2 / R2 on the PS2: the drive-by's side) and the pad's **Cross is the handbrake** (R1
  on the PS2; Cross is free once R2 accelerates). The right stick stays the camera (a vehicle has no free aim: pushed
  to a side it only turned the camera - the player's report, 2026-11-12). With **Shoot with the triggers** on, **R2
  fires guns** and **Circle is melee only** (with a gun in hand the pad's Circle does nothing, so the two never fire
  together). L3 is the horn, as in GTA V. VCS has no reload button (it reloads by itself). Checked by
  `tools/controls_check.py`.
- **Vibration**: the DualShock 2's motors on the gamepad's rumble, 0-200% (100% as on the PS2, 0% off). The game
  drives them itself (crashes, explosions, gunfire; game-internals.md 12a).
- **DualSense** (src/game/vcs_dualsense.cpp): extras a PS5 pad has, driven from the game's state, each off a no-op on
  other pads (the light bar alone works on a DualShock 4). Like GTA V's DualSense support:
  - **Light bar**: its colour during play - **Health** (green full, red hurt), **Vice theme** (a steady neon pink) or
    **Off**. **Light bar brightness** scales it.
  - **Police flash**: the light bar flashes red / blue while you have a wanted level, faster the more stars (a full
    red + blue cycle of 470 ms at one star down to 250 ms at six; it was 1030 - 680 ms, too slow to read as a police
    light), back to its colour when you lose them.
  - **Adaptive triggers**: for shooting. On foot, with **Shoot with the triggers** on (so R2 is the trigger you pull),
    the fire trigger feels the gun, each kind its own (the player's ask, 2026-11-12): a wall that breaks at the shot,
    then a kick for **every round fired** (seen as the clip counting down). Every wall starts a fifth of the way into
    the pull and the shot goes as it gives way: a **pistol** a short wall (gives at ~45%) and a short, sharp kick; a
    **shotgun** and a **sniper rifle** a full-strength wall to ~65% and a heavier kick; the **rocket launcher** the
    longest wall (~75%) and the heaviest kick per rocket. **SMGs, rifles, the M249, minigun and flame-thrower** a
    lighter wall to ~55%, then a buzz while they fire. (Until 2026-11-12 the walls began near the end of the pull and
    every gun fired at 82%: the player found them late and weak.) **Reloading or out of ammo** the trigger goes slack
    - no wall, no kick - like a gun that does not fire. Driving leaves the triggers loose, and so does getting in or
    out of a vehicle: the triggers held then are nothing (no shot, no weapon change; it fired the gun's feel before).
  - **Triggers while driving** (off by default): R2 / L2 push back by the vehicle's weight - a moped barely, a sports
    car more, a truck or a tank a lot - and judder on a crash. With **Drive with the triggers** on (above) the throttle
    (R2) firms up the harder you accelerate and **buzzes when you lose grip** (wheelspin or a slide), and the brake
    (L2) firms up the harder you press.
  - **Sound through the controller**: play the game's sound - the radio, the police scanner, everything - through the
    DualSense's own speaker instead of your speakers or headphones. It is a small speaker, so it sounds thinner; turns
    itself off when the pad is unplugged. (The game mixes all its sound together, so this cannot be the radio alone.)

  (`VCS_WANTED_OFF=<hex>` overrides the ped offset the police flash reads the wanted level from; the control channel's
  `dualsense` command shows the live value - game-internals.md 12e.)

## The game's pause menu

Escape (Start) opens it - also the Save menu a safehouse opens - and it works as a PC game's menu (src/game/vcs_frontend.cpp,
game-internals.md 6m; before 2026-11-05 Start was Enter and the menu took the pad's buttons only):
- **Keyboard**: the arrow keys move, **Enter** selects (Cross), **Escape** goes back (Triangle) - on the tab bar
  (map / brief / game / stats / controls / audio / display) that closes the menu. Shift / F still work as Cross /
  Triangle, and Start's own key (Escape unless rebound) is the menu's back while it is up. Enter does nothing outside it.
- **Mouse**: the cursor shows while the menu is up. Pointing at a tab or an item moves the game's highlight there (a tab
  shows its page, as the D-pad does); a click selects it - a tab opens its page, an item of the page under the tab bar
  opens that page and selects it (Game > Load Game in one click). The **right button** goes back (closes the menu on
  the tab bar).
  - **Sliders** (Audio > Music / SFX volume, Display > Brightness): a click on a bar sets the level, holding the button
    and moving drags it, the wheel turns it a step a notch; a click on the label only moves the highlight there.
  - **Choices** (Speaker setup, Radio station, Subtitles, Radar, HUD, Widescreen, Trails, the Controls page): a click
    turns to the next one, the wheel turns either way (up: next).
  - **Map**: drag it with the left button (it stays in the range the D-pad scrolls it); the wheel zooms (up: in, as R1).
    Cross (Enter) shows the legend, Square (Space) places the target as on the PS2.
  - **Brief, Stats**: the wheel scrolls.
- **Gamepad**: unchanged (Start opens and closes, the D-pad / left stick move, Cross selects, Triangle goes back).
- What the menu takes stays out of the game: the mouse buttons and the wheel aren't Circle / R1 / weapon changes while
  it is up, nor while still held after it closed (a click on Load Game doesn't punch); Escape held from opening it
  isn't a back. Any key or gamepad button pressed while the mouse is on its way to an item takes over.
- It all goes through the game's own pad: the menu sees D-pad steps, Cross and Triangle, so it sounds, highlights and
  switches pages as on the PS2. `VCS_MENU_MOUSE=0` leaves the mouse out (Enter / Escape stay).
  `python tools/menu_check.py` checks all of it.

## Button prompts

F2 > Enhancements > **Button prompts** (default Original): the controls the game's help text and pause menu show.
Automatic follows the device used last - the keys and mouse buttons bound on the Keyboard page, or the gamepad's own
buttons (PlayStation or Xbox glyphs, from the pad's maker) as mapped on the Gamepad page. PlayStation, Xbox and
Keyboard & mouse fix a set; Original is the PS2's text. game-internals.md 6n, DECISIONS.md 2026-11-07.

## Menus

F2 (or Back + Start on a gamepad) opens the settings, F5 (or Back + RB) the trainer (docs/TRAINER.md), F6 (or Back + LB)
turns the trainer's free camera on and off. Flying: W / A / S / D, E or Space up, Q down, mouse look, Shift faster,
Ctrl slower, Z / X zoom out / in; gamepad left stick, right stick, RT / LT, A / B, D-pad up / down (fixed keys; the
game gets no input meanwhile). In the
trainer: Up / Down or numpad 8 / 2 or the D-pad select, Left / Right or numpad 4 / 6 change a value, Enter / numpad
5 / A applies, Backspace / numpad 0 / Escape / B goes back (and closes it on the first page). The game gets no input
while a menu is open.

## Screenshots

**F8** (Keyboard > Menus > Screenshot, rebindable) saves the game's picture as a PNG in a `screenshots` folder next to
vcs.exe, named `vcs_<date>_<time>.png` (`_2`, `_3`... for several in one second); a notice names the file once it is
written. The shot is the window pixel for pixel - its resolution (the monitor's in full screen), its letterboxing, the
UI where it was - and holds what is on screen: the settings or trainer menu, the F3 overlay, the frame-rate counter and
the trainer's notices, except the notices of earlier shots, so shots in a row stay clean. The PNG is written on a thread
of its own (a 2560x1440 one takes ~0.6 s to compress; the game keeps its frame rate meanwhile). It does nothing while
the settings menu waits for a key to bind. `VCS_SCREENSHOT_DIR=<dir>` saves them elsewhere (the test tools do).
(src/main.cpp `screenshotKey`; the control channel's `screenshot` stays at its fixed test size, `PS2X_CAPTURE_SIZE`)

## Window

Alt+Enter switches between full screen and the window (Display > Window mode; saved in vcs.ini). Enter with Alt
held is not Cross in the pause menu (nor a binding). Game > **In the background** decides what happens when another window is in front: keep running
(as before), mute, or pause.

## Debug keys
| Key | Effect |
|---|---|
| F3 | performance overlay: game fps and frame-time percentiles, EE thread time per phase (game / render / IOP / wait), GS and DMA counters, host frame and GPU submit times, audio buffer, and a frame-time graph; under it (beside it on a short screen) the game's state: the player's position, heading, state, health, armour, money and weapons, the vehicle (model, position, heading, health, speed), the camera (mode, position, heading, pitch, FOV), the clock, area, world level, game time, time step, pauses, pool use and the running scripts (the mission's with its IP). The same lines answer `python tools/gamectl.py gameinfo` |
| F4 | start / stop recording one CSV row per game frame to `perf_<date>_<time>.csv` (working directory); summarise with `python tools/perf_report.py <file>` |
| F12 | (hardware renderer, only with `PS2X_HW_DUMP_KEY=1`: F12 is Steam's and Bandicam's key) list the next frame's draws on stderr and dump its render targets |

## Test runs without a window
`python tools/run.py 600 test --hidden` starts the game with no window on the desktop, no sound, and the physical
keyboard / mouse / gamepad ignored (`PS2X_HIDDEN=1`). It listens on 127.0.0.1:47615 (`PS2X_CONTROL_PORT`) for
`tools/gamectl.py`: `screenshot out.png` (the picture plus the perf overlay, rendered off-screen), `press Return 150`,
`key W down` / `up` (also `mouseleft`, `mouseright`, `wheelup`, `rshift`, `kpenter`, ...), `perf`, `quit`, and from
the game layer `setting <key> [value]` (a setting read or changed; an action setting is pressed), `menu <page>` /
`menu scroll <0..1>` / `menu close` (the settings menu for screenshots), `padstate` (the last pad report the
bindings made, and the pad reads so far), `mouse <dx> <dy>` (mouse motion in counts, y down, as the mouse sends it),
`gamepad on|off`, `gamepad <a b x y lb rb back start ls rs up down left right> <0|1>`, `gamepad <lx ly rx ry lt rt>
<value>` (a gamepad set from the channel in place of the real one while on), `aimstate` (free aim on or not, and
the shots sent down the camera line so far) and `dualsense` (the pad's light bar / adaptive-trigger support and the
game reads that drive them - the wanted level, health, driving - for confirming the wanted-level address). `--control` (`PS2X_CONTROL=1`) opens the same channel with the window shown; `PS2X_INPUT=injected`
ignores physical input with the window shown. Injected keys go through the bindings like physical ones:
`tools/controls_check.py` checks them (also a `regress.py` step). `VCS_INPUT_MAP=0` falls back to the runtime's fixed
built-in mapping (`ps2_pad.cpp`, the old defaults).

# The trainer

A cheat menu for single-player fun, in the style of Red Dead Redemption 2's **Native Trainer** (Alexander Blade's
ScriptHookRDR2 trainer, the one most RDR2 trainers copy): a list menu at the side of the screen that you open with
a key and drive with the arrow keys / numpad or a gamepad, with pages for the player, weapons, vehicles, teleports
and the world - and here a mission launcher. Nothing changes until you use it; turning a toggle off undoes it.

Code: `src/game/vcs_trainer.{h,cpp}` (what it does), `src/ui/trainer_menu.{h,cpp}` (the menu). Check:
`python tools/trainer_check.py` (below).

## How RDR2's Native Trainer works, and what was taken from it

- **One key opens a list menu** (F4 there) drawn over the game at the side of the screen: a coloured header with the
  trainer's name, a line with the current page, then rows; the selected row is highlighted. Each row is a page
  (`>`), a switch (`ON` / `OFF`) or an action; some have a value changed with Left / Right.
- **Navigation without the mouse**: numpad 8 / 2 move, 5 selects, 0 goes back, 4 / 6 change values; the arrows and
  Enter / Backspace do the same. Held keys repeat.
- **Pages by subject**: Player (invincible, heal, wanted level, money), Vehicle (spawn any model by name - optionally
  getting in -, invincible, fix, flip, boost, paint), Teleport (a list of named places, plus to a marker), Weapon (all
  weapons, unlimited ammo, no reload), Time / Weather (set hour, freeze, weather types), Misc.
- **Short status messages** on screen saying what was done ("Vehicle spawned", "God mode on").
- **Switches are held**: the trainer reapplies them as the game runs, so missions, deaths and arrests do not quietly
  turn them off.

Here: F5 (setting `controls.trainer_key`) or **Back + RB** on a gamepad opens and closes it. Up / Down (numpad 8 / 2,
D-pad) select, Left / Right (numpad 4 / 6) change a value, Enter (numpad 5, A) applies, Backspace (numpad 0, Escape,
B) goes back a page and closes it from the first. While it is open the game gets no input (the arrows, Enter and
Backspace are the game's D-pad, Start and Select). Messages appear at the bottom right for four seconds.
There is no "teleport to marker": VCS has no map waypoint. "Save position" / "Return to saved position" take its
place.

## What is in it

| Page | Row | What it does |
|---|---|---|
| Player | God mode | proofs against bullets, fire, explosions, collisions / falls and melee (`SET_CHAR_PROOFS`), no drowning (`SET_CHAR_DROWNS_IN_WATER`), health kept at 100 |
| | Heal | health 100, armour + 100 (the game caps it) |
| | Never wanted | `CLEAR_WANTED_LEVEL` every 10 frames |
| | Set wanted level | 0..6 stars (`ALTER_WANTED_LEVEL_NO_DROP`) |
| | Add money | $1,000 / $10,000 / $100,000 / $1,000,000 (`ADD_SCORE`) |
| | Infinite sprint, Police ignore you, Everyone ignores you | `SET_PLAYER_NEVER_GETS_TIRED`, `SET_POLICE_IGNORE_PLAYER`, `SET_EVERYONE_IGNORE_PLAYER` |
| Weapons | Give weapon | any of 35 weapons (types 1..39, names from the game's text), with ammunition, and selects it |
| | Give loadout | Street, Military, Heavy |
| | Unlimited ammo | reserve ammo preserved across `CWeapon::Fire` / `FireFromCar` (`0x002C0F98` / `0x002C1BB0`, like the 100% reward, plus Camera `0x26`; excludes Detonator `0x24`), never overwriting existing counts |
| | No reload | the clip refilled to the fullest it has been seen (the weapon's clip size) |
| | Fast reload | `SET_PLAYER_FAST_RELOAD` |
| | Remove all weapons | `REMOVE_ALL_CHAR_WEAPONS` |
| Vehicles | Spawn vehicle | all 110 vehicle models (ids 171..280: cars, bikes, boats, helicopters, planes), sorted by the game's names; ahead of the player, facing his way |
| | Get in spawned vehicles | (on by default) puts the player behind the wheel; when already driving, the new vehicle appears 8 m ahead of the old one and he moves over (the camera is handed the new one with `RESTORE_CAMERA_JUMPCUT`, unless a mission's script camera holds it: from car to car it stays on the old one otherwise) |
| | Vehicle invincible | proofs, no visible damage, no upside-down fire, health kept at 1000 - for whatever the player drives |
| | Repair / Flip upright / Boost / Random paint | health 1000 / back on its wheels facing the same way / a push forward / two random colours |
| Teleport | 13 districts | Escobar International, Fort Baxter Air Base, Ocean Beach, Washington Beach, Vice Point, Downtown, Little Haiti, Little Havana, Starfish Island, Prawn Island, Leaf Links, Viceport, Junkyard - with his vehicle when driving |
| | Save position / Return to saved position | including the interior (area) he was in |
| Missions | Story missions | the 59 story missions, by the game's titles, in story order (Soldier .. In the Air Tonight); started where the player stands with the game's own `LOAD_AND_LAUNCH_MISSION_INTERNAL`, as a mission marker does - most open with their cutscene |
| | Side jobs | Paramedic, Vigilante, Fire Truck, Air Rescue, Taxi; the player is put in the job's vehicle first (ambulance, police car, fire truck, air ambulance, taxi) unless he already drives one |
| Camera | Free camera | (F6, or Back + LB, also with the menu closed) fly anywhere while the game runs and the player stands: W / A / S / D along the view, E or Space up, Q down, the mouse looks, Shift x4, Ctrl x0.25, Z / X zoom out / in; gamepad: left stick moves, right stick looks, RT / LT up / down, A fast, B slow, D-pad up / down zoom. Starts where the game's camera is; off, the camera jumps back behind the player (`RESTORE_CAMERA_JUMPCUT`) unless a mission's script camera holds it |
| | Free camera speed | 5 / 10 / 20 / 40 / 80 m/s (Enter: 20), in real time: the same at any frame rate and game speed |
| | Teleport player here | the player (and his vehicle) to the ground below the camera, in the area shown; the free camera ends |
| | Hide HUD | no radar, clock, money, weapon, health, stars (CHud::Draw skipped); the zone name, help text and subtitles, drawn after it, stay |
| World | Time of day | each Left / Right sets the hour |
| | Freeze time | the clock held at the time shown when it was turned on |
| | Weather | Normal (changes by itself), Sunny, Cloudy, Rainy, Foggy, Extra sunny, Hurricane, Extra colours, Ultra sunny |
| | Game speed | 25 % .. 200 % (`SET_TIME_SCALE`); Enter: back to 100 % |

The switches are not saved: every start is the original game (DECISIONS.md 2026-10-26).

## How it works

- **The game's own script commands.** Each action is a few MAIN.SCM opcodes run as a short script on the game's
  script VM (`vcs::rig::startCodeScript`, game-internals.md 11d), between frames at the frame kick. The game applies
  them the way its own missions do (wanted level, proofs, model streaming, vehicle creation). Opcode numbers are the
  VCS ones from the Sanny Builder Library, each checked in the game (game-internals.md 11e).
- **Held switches** are applied every 10 game frames (`tick`): values that drift are written directly into the ped
  or vehicle at the frame kick - health (+0x4E4), the weapon slots' ammunition (+0x578), the vehicle's health
  (+0x278) - and the commands that a mission, death or arrest can reset are run again every 60 frames.
- **One action per frame, in order**: an action that starts a script waits for it to finish (it does in the frame it
  starts) before the next runs, so each sees the previous one's result, and the control channel's answer comes after.
- **Models are streamed first.** Weapons and vehicles need their model: the trainer requests it (`REQUEST_MODEL`),
  then waits frame by frame until the streaming has it (the byte `HAS_MODEL_LOADED` reads), up to ~10 s, before the
  script that uses it. The game's `LOAD_ALL_MODELS_NOW` would load it inside the frame - and when the streaming memory
  is full (28 different vehicles parked in one street) it never returns and the game freezes. A model that does not
  load in time gives the message "the game's memory for models is full here".
- **Nothing reaches a save.** A save stores the running scripts. Trainer scripts start only while the game runs (its
  millisecond clock 0x4CD104 advanced since the last frame) and one still waiting when the game pauses (pause menu,
  save screen) is taken back out of the active list (`vcs::rig::cancelCodeScript`) and run again after. A script
  slot counts as the trainer's only while its IP is in the slot's own code: the game reuses a finished script's slot,
  and a save load fills them all with the save's scripts - taking one of those for the trainer's and cancelling it
  hung Load Game on "Loading..." (2026-10-28, game-internals.md 11d).
- **Vehicle commands only in a vehicle.** A car command on the handle -1 stops the script for good (it never reaches
  its end), so they run only when the ped's state is 0x37 (driving) and its vehicle pointer is set. The handle comes
  from `STORE_CAR_CHAR_IS_IN_NO_SAVE` (0x024B): `STORE_CAR_CHAR_IS_IN` (0x007B) answers -1 for a car made by
  `CREATE_CAR`.
- **The free camera** (`src/game/vcs_freecam.cpp`) lets the game's per-camera update, CCam::Process (FUN_00294540),
  run as usual and takes the place of the height routine right after it for the active camera while it flies: there
  it writes the camera's source, front, up and FOV (game-internals.md 12b), and CCamera::Process builds the frame from
  them as from any camera - so culling, LODs and the world's streaming follow it. The game's update has to keep
  running: a cutscene ends only when its camera has played its path. The first version replaced CCam::Process itself,
  and a mission started under the free camera sat on "Loading..." for good, its script waiting in the cutscene
  (the user's report, 2026-10-30; game-internals.md 12b). No script runs per frame. The pad reads give the
  game a pad left alone meanwhile (`src/game/vcs_input.cpp`) and the mouse is left for the camera; the trainer menu
  open, the camera holds still. `VCS_FREECAM=0` leaves the game's camera code alone (the switch does nothing).
- **Missions** are listed from MAIN.SCM, read from the disc at start (`vcs::trainer::setMissionScript`): each
  mission opens with `LOAD_MISSION_TEXT "<name>"` (0x033A) or `SCRIPT_NAME` (0x0238), and a story mission's title is
  the text key of that name with a two-digit number (`JER_A1` -> `JER_A01` "Soldier"). A launch is refused while a
  mission runs (`0x4CD3E7`): VCS has no script command that fails the current mission.
- **Names** come from the game's text at run time (the GXT table: weapon names `WEAPnn`, district names by zone key,
  vehicle names by the model's game name), so they are the game's own and in its language. The list is built when the
  text and the vehicle models are both in memory.

## Limits

- **Repair** restores health (engine, fire) but not dents or missing panels: no script command fixes the body (a
  Pay 'n' Spray does).
- With a wanted level the police pull the player out of a stopped vehicle, spawned or not.
- A mission started out of turn plays as usual; passing it sets what passing it sets, and saving afterwards keeps
  that (the story's own order may then be odd). A running mission cannot be cancelled from the trainer.
- **Last Stand** (the story's last mission) ends with its final cutscene and the screen faded out for the credits,
  which the story starts only once every chapter is done (game-internals.md 11g). Launched from here before that,
  nothing would fade the screen in again - the game's "Loading..." over black for good (the user's report,
  2026-10-29). So when a Last Stand launched from the trainer passes and the story does not roll the credits, the
  trainer runs the game's own credits subroutine (main script 0x24DC3, as a script of its own named `credits`) 5.5 s
  after the end, as the story would; at
  their end the player is back in control below the Mendez building. Story flags are not touched (no "credits
  seen", no chapter change). `python tools/finale_check.py [--story] [--full]` plays the end.
- Boats and planes appear where the player stands, on land too; helicopters and the Airliner can be flown.
- The developer performance overlay (F3) and the trainer share the top left corner.
- While the free camera flies the game gets no input at all: no pause menu (Enter) until it ends. A vehicle the player
  drives rolls on and slows down by itself. It works in cutscenes (the cutscene plays out behind it, the game's camera
  carries on when it ends). Starting a mission from the trainer turns it off, so the mission's start is seen and ends by itself when the game leaves play (Load Game, the title screen).
- The free camera's keys are fixed (not on the Keyboard page); only its on / off key is (Keyboard > Menus).

## The check

`python tools/trainer_check.py [OUT] [--vehicles all|N]` loads the Fort Baxter rig save (a copy, hidden) and checks,
in the game's memory: the name lists; god mode (health refilled, an explosion at his feet survived); wanted level set,
cleared, never wanted; money; weapon, loadout, unlimited ammo, no reload; spawning (`--vehicles all`: all 110, in
batches at eight places in the city) and getting in, the camera following the new vehicle; repair, flip, boost, paint, vehicle invincibility; a teleport
while driving and to every district on foot; save / return; time, freeze, weather, speed; the free camera (50 m in 2.5 s at 20 m/s with the player
still, the game's camera at its position, a radian for 400 mouse counts, 400+ m away, Hide HUD stopping CHud::Draw, the
player teleported below it, the camera back on him); no trainer script in the
script list while paused; the menu opened and a row toggled with keys while the game behind it keeps running; the
mission lists, a side job launched on foot (he ends up in its vehicle, the job running) and a second launch refused.
2026-10-26: 30 checks, 0 failed (all 110 vehicles); with missions 33 checks, 0 failed. 2026-10-28 (camera check
added): 33 checks, 0 failed (12 vehicles). 2026-10-30 (free camera added): 40 checks; the one failure was the check's own
flight path (tilted down first, it flew 274 m under the map); fixed, `--freecam` (only the free camera's checks): 7
checks, 0 failed. After the "Loading..." fix (a mission launch turns the free camera off; `--freecam` also plays
Soldier's opening cutscene out under it, ~4 min): 41 checks, 0 failed; `--freecam` 8 checks, 0 failed.

`python tools/finale_check.py [OUT] [--story] [--full]` (~5 min, `--full` ~15): Last Stand launched from the trainer,
skipped to its roof, the fight won, then the final cutscene, the mission's end and the credits - started by the trainer,
or with `--story` (the chapter flags set) by the story itself and only once; `--full` waits for the credits' end.

# Handoff - Frente 10: FramerateVigilante (SA/VC) + MAIN.SCM high-FPS audit

Updated 2026-11-25 11:25 (-03:00) after completing all remaining fixes, final `--opt lto` build, and LTO verification.

## 0. Where things are

| What | Where |
|---|---|
| Worktree | `D:\Games\vcs-agent` (NOT `D:\Games\vcs-hd-world-impl` - always `Set-Location D:\Games\vcs-agent ; ...`) |
| Branch | `agent/framerate-vigilante-audit` (on top of `1ff0eed`, PR #116 merge) |
| Build dir | `D:\Games\vcs-agent-build` (`vcs.exe` ThinLTO built there; `VCSLauncher.exe` in `D:\Games\vcs-agent\`) |
| PR target | `willzmkv/gta-vcs-recomp` branch `hd-world-merge` (`origin`; `upstream` = gothamor, never push there) |
| Current binary | `--opt lto` (ThinLTO `vcs.exe` + `VCSLauncher.exe`) of all changes below |
| A/B probes | `D:\Games\vcs-agent\build\audit\verify_fv_fixes.py`, `scratch\probe_brake_regression.py`, `scratch\probe_droplets.py` (untracked) |
| MAIN disassembly | `build\analysis\scm\dis\main_section.txt` (29,092 insns, `0x780C..0x3CAE3`, never commit) |

Already merged before this front (do not redo): #112 rain streak phase (#100), #113 Hunter / #183 note / etc. per `PROGRESS.md`, #115 side-mission HUD in Oswald + Vigilante 4:3 fade snap, #116 Shader Stochastic. Project 2DFX coronas (`vcs_world_hd_coronas.*`) untouched.

## 1. Code changes (`src/game/`)

All behind existing switches (`=0` restores original path; bit-exact at 30 fps `scale = step / (50/30) = 1`):

### `src/game/vcs_physics_timing.cpp` (`VCS_PHYSICS_TIMESTEP=0` = off)
- `0x00348628` `CPhysical::ApplyAirResistance`: `airRes <= 0.1f` branch (`0x348740`) `m_vecTurnSpeed` `+0x70..+0x78` `0.99f` per frame -> `powf(0.99f, scale)`.
- `0x00347E78` `ApplyTurnForce` unscaled call sites scaled by `frameScale`: `0x00152FF8` (`FUN_00152E18` burnout steer pivot when `*(s16*)(veh+0x714)!=0`), `0x0033CDA0` (fallen bike uprighting `-0.1`), `0x0033CE34` (dismounted bike roll torque `-0.15`).
- `0x00152E18` `CAutomobile` airborne free-wheel spin (`veh+0x6FC..+0x708`: `*0.95` -> `powf(0.95f, scale)`, `-=0.1` / `+=0.05` -> `* scale`).
- `0x00337208` `CBike::ProcessControl` airborne wheel spin (`bike+0x598 *= 0.95`, `+0x590 += raw598`, `+0x59C += +0.1/-0.2`, `+0x594 += raw59C`), skipped for `STATUS_SIMPLE` or when `ProcessWheel` (`$t3`) wrote `+0x598/+0x59C`.
- `0x00348128` / `0x0034E1D8` / `0x0034E348`: accumulate bike suspension spring impulse deltas (`+0x140`, `+0x70`) and add `(1/scale - 1)*dV` during `CVehicle::ApplySuspensionDamper` so bikes preserve PS2 suspension compliance under braking.
- `0x003E7618` `CVehicle::ProcessWheel` (bikes): scale lateral component of `*speed` orthogonal to `fwd` by `frameScale` strictly while `ctx->f[13] <= adh` (`adh = f[14] * step * surfaceGrip_if_nonzeroState`), preventing `state = 2` braking spinouts above 30 fps while leaving `brake > adh` handbrake locks (`20000.0`) intact.

### `src/game/vcs_droplets.cpp` (`VCS_DROPLETS=0` = off)
- Replaced erroneous `kCameraInterior = 0x006F4DEC > 0.5f` (`CCamera::m_fCameraAverageSpeed` m/frame, which cleared all drops above ~15 m/s at 30 fps) with `CGame::currArea` (`0x00489F7Cu != 0`) + timecycle `tc+0x3378 > 0.01f`.
- Capped trail beads below `kMaxTrailCap = 480` within `kMaxDrops = kMaxSend = 640` (plus `reserve(kMaxDrops)`) so high-speed trails never evict fading primary drops, and changed end-of-life fade-out to smooth quadratic ease-out `(1 - lifeFrac)^2`.

### `src/game/vcs_script_timing.cpp` (`VCS_SCRIPT_TIMESTEP=0` = off)
- `Site` got `bool unscaleTime`; `kTimeScale = 0x004CD168` (ms_fTimeScale).
- New mission sites (operand types checked in raw MAIN.SCM bytes, not the listing - it prints int/float alike):
  `M061 +0x3F43` (`L(8), F(0.5f), true` = divide out slow-motion), `M081 +0x1968` (`G(0x1625), F(1.0f)`),
  `M092 +0x41A2` (`G(0x173B), G(0x173C)`), `M098 +0x2DB1` and `+0x2E36` (`G(0x1576), F(4.0f)`).
- `collectParamsHook` on CollectParameters `0x00254B90`: scales `ScriptParams[2]` (`ra == 0x0030DB54`,
  `0x020B ROTATE_OBJECT` step) and `ScriptParams[4..6]` (`ra == 0x0030E348`, `0x020C SLIDE_OBJECT` speeds) by
  `step / ps2step` when step < ps2step. Covers 2 calls in MAIN (`SHOPMON` `0x1F715`/`0x1F7CD`) + 14 slide / 3 rotate
  calls in missions.

### `src/game/vcs_overrides.cpp`
- `vcs::installScriptTimingHooks(runtime)` moved after the `VCS_NATIVE_PARAMS` block that replaces `0x00254B90` with
  `collectParamsNative`, so the hook chains on top and still works with `VCS_NATIVE_PARAMS=0`.

## 2. Audit result (what was checked and ruled out)

- All 101 missions + every MAIN thread (91 threads) read. Safe / not changed: `PAGER`/`WARPSPO` (WAIT debounce),
  `SRANGEH`/`EXPORTB`/`MARKCAM`/`DBG*`/`THREAD_3A3F4` (dev tools), `AMMUNAT`, `JANDW` (TIMERA based), `EMMON`
  (`GET_GAME_TIMER + 5000`), `GAMHELP`/`REWMON`/`FERRIS`/`USJ`/`RAMPAGE`/`AMBTAXI`/`99RED` (loop indices / one-shot).
- EE functions ruled out: CFire `FUN_002bddc8`/`002c0f98`/`002beab8`/`002bd6f0` (already use `0x4CD110`/`0x4CD104`);
  `FUN_0013A0A0` `*0.75` (visual suspension height); `FUN_001F25F0`, `FUN_003F6418` (`*0.6` visual flaps),
  `FUN_001F4BF0` stuck-ped threshold (clamped; matters only > 100 fps; not changed).

## 3. Measured (final ThinLTO `--opt lto` build, 2026-11-25)

| Probe (`verify_fv_fixes.py`) | 30 fps fixed | 144 fps fixed | 144 fps old (`=0`) | Verdict |
|---|---|---|---|---|
| `air_turnZ_ratio` (0.5 s decay) | 0.8609 | 0.8601 | 0.4850 | verified |
| `car_coast_spin_04` (0.8 after 0.4 s) | 0.3921 | 0.4218 | 0.0369 | verified |
| `car_gas_spin_015` / `car_gas_rate_s` | -0.596 (`-2.985/s`) | -0.501 (`-3.000/s`) | -2.400 (`-14.379/s`) | verified |
| `bike_front_spin_02` (after `0x560` settle) | 0.5321 | 0.5747 | 0.1631 | verified |
| `slide_m_s` (`SLIDE_OBJECT` 0.2/frame) | 5.9395 | 5.9989 | 28.5645 | verified |
| `rotate_deg_s` (`ROTATE_OBJECT` 2/frame) | 59.6562 | 59.9976 | 291.6641 | verified |

- `probe_brake_regression.py` (`30 / 60 / 144` fps fixed vs `144` unfixed):
  - `S_and_A` `max_dhdg`: `62.80° / 61.87° / 56.94°` (vs `81.62°`, `end 36.19°` unfixed).
  - `A_hold_S` `max_dhdg`: `71.29° / 67.12° / 61.89°` (vs `60.52°`, `end 60.52°` unfixed).
  - `A_hold_Space` (handbrake): `93.76° / 92.65° / 92.24°` (vs `69.10°` unfixed; `161.96°` before `brake <= adh` guard).
  - `S_and_D_tap`: `13.15° / 12.00° / 12.60°` (`off=0`, vs `178.92°` wipeout `min_upz=-0.192` unfixed).
  - `bspin_diag` (moving PCJ-600 lifted `+35 m`): `598` decay `t+0.10..0.80s` `-1.565 -> -0.175` (`s30`), `-0.515 -> -0.069` (`s144_fix`), `-0.245 -> -0.00045` (`s144_old`).
- `probe_droplets.py` (`30` and `144` fps): `suppressed=0`, `active ~455..484`, `sent ~396..450` through `39.4 m/s` (`30 fps`) and `38.1 m/s` (`144 fps`); gradual rain-off fade-out at `~40 m/s` over `2.10..2.80 s`; `currArea=1` suppresses (`suppressed=4, sent=0`) and `currArea=0` resumes (`sent=198..200`).
- Other LTO checks (all `exit=0`):
  - `VCSLauncher.exe --self-test`, `VCS_SELFTEST=1` (`0 failure(s)`).
  - `python tools/framerate_audit.py --sites` (`[sites] all found`).
  - `python tools/hose_check.py --step 144` (`girls_per_s=1.20, fuse1_s=10.01, fuse2_s=14.16`), `shakedown.py` (`PASS`), `haze_check.py` (`PASS`).
  - `python tools/framerate_audit.py --steps 15,30,60,100,144` (`20261125_102329`): three transient stuck-at-spawn collision glitches on `1/15 coast`, `1/60 brake/bikebrake`, `1/144 bikebody` re-verified directly with `--only brake,bikebrake,bikebody --steps 30,60,100,144` (`20261125_111345`: all 4 accelerated normally; `bikebrake` `nose_fwd_z=-0.253..-0.256`, `pitch_rate=0.0290..0.0297`, `rider_off=0`, `end_up_z=1.0`).

## 4. Status & Next Priorities

1. **Frente 10 (`agent/framerate-vigilante-audit` -> `hd-world-merge` `e676a68`):** merged cleanly into `hd-world-merge`.
2. **Issue `#113` (`agent/issue-113` in `D:\Games\vcs-agent`, built to `D:\Games\vcs-agent-build\vcs.exe`):**
   - Explosion audio fix (`CExplosion` `0x002E1140`/`0x002E23A0` ungated from parked-car suppression) + CPU/GS/RHI/D3D12/Vulkan Rounds 1–3 fixes implemented, compiled with `--opt lto`, and verified across `bench_avenue`, `repro_113_stutter`, `run_r3_verify`, `scenes`, `fire_check`, `soundcheck`, `VCS_SELFTEST=1`, and `VCSLauncher.exe --self-test`.
   - Docs updated and committed on `agent/issue-113`; **held from merging per user request until visual/in-game review completes**.
3. **Next queued work (after `#113` review):**
   - **Moon bug + HD option:** fix moon not always appearing at night & stretched aspect ratio on original moon, plus add LCS HD moon texture from `D:\Games\LibertyStories-ipad-test\assets` as an official toggleable mod option.

## 5. Traps found on the way

- **Rig scripts and GOTO:** `rig.cpp` `startCodeScript` puts code at `idle_slot + 0x114`, below ScriptSpace, so its
  IPs are negative and VCS `GOTO` (`0x0002`) / `GOTO_IF_FALSE` read negative targets as mission-relative. Never branch
  inside a `g.scm(...)` script: unroll `WAIT 0` steps and time them with `0x010E GET_GAME_TIMER`. A rig script holds at
  most 176 bytes; outputs are locals read back by `g.scm_outputs(code, n)`.
- **Probe timing:** use the game clock (`0x4CD104`, `wait_sim`) and per-frame traces (`trace <file> <addr> <n>`), not
  wall-clock sleeps; at 144 fps the hidden window runs much faster than real time.
- **PowerShell 5.1:** `;` not `&&`; no `$var` inside double-quoted `python -c`; write UTF-8 files from Python.
- `generated/` and `build/analysis/` are game code / dumps: never commit them.

## 6. Ideas noted but not started

- `FUN_001F4BF0` stuck-ped threshold above ~100 fps.
- The earlier survey's per-frame visual items (text grow / scroll in CRASH, SRANGE) are still looks-only.

# Handoff - Frente 10: FramerateVigilante (SA/VC) + MAIN.SCM high-FPS audit

Written 2026-11-25 07:15 (-03:00) for the next agent. Read `AGENTS.md` first; this file only says where Frente 10
stands and what is left. Everything quoted as "measured" below was seen in a tool output on 2026-11-25.

## 0. Where things are

| What | Where |
|---|---|
| Worktree | `D:\Games\vcs-agent` (NOT `D:\Games\vcs-hd-world-impl`: the shell starts there - always `Set-Location D:\Games\vcs-agent ; ...`) |
| Branch | `agent/framerate-vigilante-audit` (on top of `1ff0eed`, PR #116 merge) |
| Build dir | `D:\Games\vcs-agent-build` (`vcs.exe` there; `VCSLauncher.exe` is in `D:\Games\vcs-agent\`) |
| PR target | `willzmkv/gta-vcs-recomp` branch `hd-world-merge` (`origin`; `upstream` = gothamor, push disabled - never push there) |
| Current binary | `--quick` (no LTO) build of the 3 code changes below |
| A/B probe | `C:\Users\dejot\.gemini\antigravity-cli\brain\de244906-36ee-478c-8348-6eed093ecefe\scratch\verify_fv_fixes.py` (copy: `build\audit\verify_fv_fixes.py`, not committed) |
| MAIN disassembly | `build\analysis\scm\dis\main_section.txt` (29,092 insns, `0x780C..0x3CAE3`, 0 bad branch targets; never commit) |
| Audit notes | `...\scratch\main_threads_audit.txt`, `...\scratch\main_threads_ctx.txt` (same brain folder) |

Already merged before this front (do not redo): #112 rain streak phase (#100), #115 side-mission HUD in Oswald +
Vigilante 4:3 fade snap, #116 Shader Stochastic. Project 2DFX coronas (`vcs_world_hd_coronas.*`) are done: the user
said not to touch them.

## 1. Code changes (uncommitted until this handoff's commit; all in `src/game/`, no runtime change)

All are behind the existing switches, `=0` gives the old code path, bit-exact at 30 fps (scale = step / (50/30) = 1).

### `src/game/vcs_physics_timing.cpp` (`VCS_PHYSICS_TIMESTEP=0` = off)
- `0x00348628` CPhysical::ApplyAirResistance: when air resistance (`+0xDC`) `<= 0.1`, turn speed `+0x70..+0x78` is
  multiplied by a literal `0.99` per frame -> now `0.99^(step/ps2step)`.
- `0x00347E78` ApplyTurnForce unscaled call sites: `0x00152FF8` (car burnout steer pivot, when `*(s16*)(veh+0x714)!=0`),
  `0x0033CDA0` (fallen bike uprighting, `-0.1`), `0x0033CE34` (dismounted bike roll torque, `-0.15`).
- `0x00152E18` CAutomobile airborne free-wheel spin (`veh+0x6FC..+0x708`: `*0.95`, `-=0.1`, `+=0.05` when
  `veh+0x690+4*w <= 0` and not `modelInfo+0xD0 & 0x2000000`).
- `0x00337208` CBike airborne wheel spin (`bike+0x598 *= 0.95; +0x590 += raw598`, `+0x59C += 0.1 / -0.2;
  +0x594 += raw59C`), skipped for `STATUS_SIMPLE` and when processWheelBike (`$t3` = GPR 11) wrote `+0x598/+0x59C`.

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

## 3. Measured (quick build, `verify_fv_fixes.py`, second run 2026-11-25 07:07)

| Probe | 30 fps fixed | 144 fps fixed | 144 fps old (`=0`) | Verdict |
|---|---|---|---|---|
| air_turnZ_ratio (0.5 s decay) | 0.8609 | 0.8601 | 0.4850 | fixed |
| car_coast_spin_04 (0.8 after 0.4 s) | 0.3921 | 0.4218 | 0.0350 | fixed (within 8 %) |
| car_gas_spin_015 | -0.696 | -0.498 | -2.400 | rate fixed, end value is a sampling artefact (see below) |
| slide_m_s (SLIDE_OBJECT 0.2/frame) | 5.94 | 6.00 | 28.56 | fixed |
| rotate_deg_s (ROTATE_OBJECT 2/frame) | 59.66 | 60.00 | 285.71 | fixed |
| bike_front / rear_spin_02 | 0.532 / -1.59 | ~0 / ~0 | ~0 / ~0 | **inconclusive - probe broken at 144 fps** |

- car_gas: the first run's traces showed the per-frame rate: 30 fps `-0.099` per 33 ms, 144 fps fixed `-0.021` per
  7 ms (both ~3/s), 144 fps old `-0.1` per 7 ms (~14/s). The end values differ because the throttle starts before the
  trace; read the per-frame slope, not the end value.
- bike: at 144 fps (both fixed and old) `+0x598` decays for ~8 frames and then is overwritten with ~`-2e-6` and stays
  near 0 (`build\audit\trace_fv_s144_*_bspin.txt`) - something (likely processWheelBike treating the bike as on
  the ground, or the bike's status) writes it. Not yet understood. **Do not claim the bike fix is verified.**
- `python tools/framerate_audit.py --sites` (quick build): every mission `N of N found`, `[sites] all found`.

## 4. What is left (in order)

1. **Bike probe.** Find why `+0x598/+0x59C` collapse at 144 fps in the probe (check `bike+0x48` status, the
   processWheelBike `$t3` path in `vcs_physics_timing.cpp`, spawn higher / zero `+0x140` every frame). Fix the probe
   or the hook; get 30 vs 144 fixed matching and 144 old differing. If it cannot be made to work, say so in the docs.
2. **Final build:** `Set-Location D:\Games\vcs-agent ; python tools/build.py --opt lto ; python tools/build.py --launcher`
   (~10 min; never edit sources while it runs).
3. **Verification on the LTO build** (all hidden, copied cards; report exact numbers, name any failure):
   `verify_fv_fixes.py`; `python tools/framerate_audit.py --sites`; `python tools/framerate_audit.py --steps 15,30,60,100,144`;
   `python tools/hose_check.py --step 144`; `python tools/shakedown.py`; `python tools/haze_check.py`;
   `VCS_SELFTEST=1` run of `D:\Games\vcs-agent-build\vcs.exe` with `VCS_DATA_DIR` on a card copy;
   `D:\Games\vcs-agent\VCSLauncher.exe --self-test`. Then `python tools/clean.py --apply --keep-days 7`.
4. **Docs** (same commit as any further code change):
   - `docs/game-internals.md` section 4 physics list (around line 440): the five physics sites of section 1 with the
     FramerateVigilante SA/VC mapping, and the ruled-out list of section 2.
   - `docs/game-internals.md` section 11f (around lines 3305-3325): the full audit, the new mission sites and
     `0x020B`/`0x020C` through `0x00254B90`; the line "Per frame but looks only ... object slides and spins" is now
     wrong - object slides and spins are fixed.
   - `docs/PROGRESS.md`: milestone 4 (around 662-672), the switch table (`VCS_SCRIPT_TIMESTEP`, `VCS_PHYSICS_TIMESTEP`,
     around 1269-1275), and a dated entry at the end (`## 2026-11-25 - Frente 10: ...`) with the LTO numbers.
5. **Commit, push, PR, merge:** `git diff --check`; commit; `git push -u origin agent/framerate-vigilante-audit`;
   `gh pr create --repo willzmkv/gta-vcs-recomp --base hd-world-merge`; `gh pr merge`. Leave the LTO binaries built.

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

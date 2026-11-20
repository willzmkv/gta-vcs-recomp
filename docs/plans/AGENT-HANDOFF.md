# Working on this fork as a second agent

Instructions for any agent (another model, another session) that helps with the 1.0.0 work **without touching the main
work**. Read all of it before doing anything; then read `AGENTS.md` (the contract) and the plan of your task in
`docs/plans/`.

## 1. Where you work - never in the main tree

The main work lives on the branch `hd-world-merge`, in its own folder, built into the player's game folder. You never
work there, never build into the player's folder, never push to `hd-world-merge` or `main`.

Your setup (the player runs these once, from the main repository folder; `<agent>` is a folder of your own):

```
git fetch origin
git worktree add <agent> -b agent/<task> origin/hd-world-merge
cd <agent>
python tools/build_dir.py set <agent>-build      # your own build output: the player's game folder is never touched
python tools/build.py --recompile <the ISO>      # generated/ is not in git: make your own (slow, once)
```

- **One branch per task**: `agent/<issue number>-<short-name>` (e.g. `agent/61-building-switch-survey`), always started
  from the latest `origin/hd-world-merge`.
- Before starting a task: `git fetch origin && git rebase origin/hd-world-merge` on **your** branch only.

## 2. What you must never do

- Never commit to, push to, rebase, merge into or force-push `hd-world-merge` or `main`. Never delete branches or tags.
- Never edit the player's game folder, its `vcs.ini`, its `modloader/`, or the player's memory card
  (`%LOCALAPPDATA%/VCSRecomp/mc0`). Tests run on card **copies** (`build/rig/saves/...` in your worktree,
  `VCS_DATA_DIR`), hidden (the tools do this).
- Never commit game code or data: no ISO, ELF, `generated/`, disassembly, extracted assets, game text (GXT / TSV
  tables), screenshots of the game in git. Analysis output goes to `build/analysis/` (ignored).
- Never publish releases, tags, the mods pack, or anything else on GitHub. Never close issues - comment on them.
- Never change `docs/DECISIONS.md` (the player's decisions) or the launcher's look (`launcher/` is "perfect": only what
  a task asks).
- Never run tests while the player is playing or while another session is running tests: one game at a time on this
  machine (the control port is shared). Ask the player first.
- Never weaken a check to make it pass. If a test fails, find out why; if it is the test's fault, say so in the PR.

## 3. How a task is done

1. Pick **one** open issue of milestone 1.0.0 that the player assigned to you (see section 5). Read its plan section.
2. Measure / reproduce first (the plan says how), with scripts under `build/analysis/` in your worktree.
3. Change the least code that fixes it, in the style of the surrounding code (comments say *why*, with the player's
   report or the measurement). Runtime changes (`third_party/PS2Recomp`) get a `[gta-recomp]` marker and a row in
   `docs/PS2RECOMP_PATCHES.md`. What you learn about the game goes into `docs/game-internals.md`.
4. Build (`python tools/build.py` in your worktree) and run the checks the plan names, plus the ones near your change
   (`tools/prompts_check.py`, `tools/hud_check.py`, `tools/cutscene_check.py`, `tools/chase_check.py`, ...).
5. Commit on your branch with a message that says why (see `git log` on `hd-world-merge` for the style), push **your
   branch** (`git push origin agent/<task>`), and open a pull request **into `hd-world-merge`** with: what was wrong,
   the evidence (numbers, before / after), the checks run and their results, and anything left. Comment the PR's link
   on the issue.
6. Stop there. The player (or the main agent) reviews and merges. Do not start a second task on the same branch.

## 4. The state on 2026-11-20 (what is done, what is waiting)

- **Done and merged into `hd-world-merge`:**
  - Subtitles (Oswald, options, sub-pixel letters, launcher rows: #42-#44), dark cutscene sky (#38), cutscene HD world (#76), `menushape` (#55).
  - `#61` (PR #77): building switch survey across the disc + `tools/hdworld_missions.py` (6/6 mission switches verified).
  - `#54` automation (PRs #78, #79): `saves.py` menu navigation on translated discs, `PHI_A2` state 3/4, vehicle spawn stability, `controls_check.py` trigger independence + DualSense trigger feel.
  - `#80`: F2 settings menu and F5 trainer restyled in the launcher's Miami Vice look (Global menu `#48` moved to 2.0.0, see `docs/DECISIONS.md`).
  - `#35` (PR #81): vehicle density standing still (`clear20A` and extended-range `not_toward` relaxed in car spawner).
  - `#36` (PR #82, closed): HD world overlaps & dupes — same-id multi-bucket sectors (`r >= 20`, e.g. Downtown `914`/`915` opaque + glass) kept in `sameSphere()` (fallback `VCS_HD_SAME_ID_DUPES=1`), `0x8000` filtering in `detailed`/`[hddupe]`, Option B for game-list pairs, `[hdprobe]` skip reasons, `tools/hd_overlap_scan.py` ocean/freefall skip; wide night + noon scans, `hdworld_missions.py` 6/6, and `cruise.py` ThinLTO draw checks all 0.
- **v1.0.0 published (`2026-11-20`):** PRs `#83`/`#84`/`#85` merged to `main`, tag `v1.0.0` live with Windows standalone `.exe`, Windows `.zip`, and Linux `.AppImage` (all bundling HD Hands 14 Vic outfits + HD Hud + PT-BR). Closed: `#31`, `#34`, `#36`, `#40`, `#41`, `#46`, `#47`, `#56`, `#57`, `#59`.
- **Remaining open in 1.0.0 milestone (follow-ups):** `#54` (full regression baseline rerun on ThinLTO) and `#58` (PGO profile training + full story playthrough).

## 5. Tasks suited to a second agent

Always assign **one** open issue at a time on a fresh branch `agent/<issue>-<short-name>` from `origin/hd-world-merge`:

1. **#54 Full regression suite + framerate audit (baseline verification)** - run `tools/regress.py` (no `--quick`) and `tools/framerate_audit.py --steps 15,30,60,100` on a ThinLTO build in the agent worktree now that PRs #78/#79 fixed the automation failures; report every pass/fail and update `docs/PROGRESS.md` (do not overwrite `build/regress/baseline` in the main tree).
2. **#47 Translation PT-BR mod packaging** - only after the player finishes #46; package the loader/mod structure and checks without committing any game text (GXT/TSV).
3. **Post-1.0.0 self-contained tasks** (after 1.0.0 ships, from `docs/plans/README.md`): HD characters checks, live mod reload, performance phases, DualSense police radio, graphics tasks `#3`-`#7` in `docs/FORK.md`.

The release itself (#56-#59), `docs/DECISIONS.md`, and anything the player must decide stay with the player and the main agent.

## 6. Workflow & prompt guide for future agents

The two-phase workflow below worked cleanly across PRs #77-#82 and should be used for any non-trivial task:

### Two-phase execution (required for C++ / engine / renderer tasks)
1. **Phase A — Investigate & propose (no C++ edits yet):**
   - Create a fresh branch from `origin/hd-world-merge` inside `D:\Games\vcs-agent` (never touch `D:\Games\vcs-hd-world-impl`, `D:\Games\vcsrec`, or `%LOCALAPPDATA%\VCSRecomp\mc0`).
   - Reproduce and measure first (`build/analysis/<task>/`). Disprove hypotheses with evidence (`VCS_WATCH`, `VCS_HD_DRAWMODE`, disassembly, screenshots) rather than guessing.
   - Report a concise table of findings + proposed fix + any design trade-off (Option A vs Option B) to the orchestrator/player and **wait for approval** before editing C++ sources.
2. **Phase B — Implement, verify & open PR:**
   - Apply the minimal fix following the existing code style and **Rule 1** (keep an env-var fallback switch to the old behavior and test that the fallback reproduces the old state).
   - Update `docs/game-internals.md` (with exact addresses and how they were found) and the relevant plan file in `docs/plans/`.
   - Build with `--quick` while iterating, then run a final **ThinLTO** build (`python tools/build.py --opt lto`) and run the final smoke/cruise checks on that ThinLTO binary.
   - Rebase onto `origin/hd-world-merge`, push `agent/<issue>-<short-name>`, open a PR into `hd-world-merge` with root cause + before/after evidence + validation results, comment the PR link on the issue, and **stop**.

### Reusable handoff prompt checklist
When briefing a new agent, always include:
1. **Workspace isolation:** exact worktree path (`D:\Games\vcs-agent`), build dir (`D:\Games\vcs-agent-build`), new branch name (`git switch -c agent/<N>-<name> origin/hd-world-merge`), and forbidden paths (`D:\Games\vcs-hd-world-impl`, `D:\Games\vcsrec`, `%LOCALAPPDATA%\VCSRecomp\mc0`).
2. **Forbidden actions:** no push/merge/rebase on `hd-world-merge`/`main`, no closing issues, no game data/GXT/screenshots in git, no edits to `docs/DECISIONS.md` or `launcher/`, no running the game while the player or main agent is running it, no weakening tests.
3. **Reading order:** `docs/plans/AGENT-HANDOFF.md`, `AGENTS.md`, the task's plan in `docs/plans/`, the relevant `docs/game-internals.md` sections, `gh issue view <N> -R willzmkv/gta-vcs-recomp`, and the target source files.
4. **Task scope & Phase A / Phase B gate:** explicit deliverables, required checks (e.g. `hdworld_missions.py`, `cruise.py ... VCS_HD_DRAWCHECK=1`, `controls_check.py`), and stop-for-review before Phase B.
5. **Delivery:** PR to `willzmkv/gta-vcs-recomp` base `hd-world-merge`, comment link on issue `#N`, then stop.

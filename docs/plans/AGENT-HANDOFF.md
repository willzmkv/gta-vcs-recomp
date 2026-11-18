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

## 4. The state on 2026-11-18 (what is done, what is waiting)

- Done and closed: subtitles (Oswald, options, sub-pixel letters, launcher rows: #42-#44), the dark cutscene sky
  (#38), HD world duplicates and the warehouse gap (#37, #32), cutscene HD world (#76, already so), menushape (#55).
- Fixed, waiting for the player to check in play: police chase (#41), Vic at the airport via PS2 cutscene framing
  (#40), the distant proxies' wrecks and seated poses (#34).
- Investigated, not fixed: Vehicle density standing still (#35) - the cap and the spawner's tries are ruled out (numbers
  in the issue); next suspect: the spawn's later gates and the removal.
- Waiting for the player: #36 (needs F3 positions), the translation's read and its two decisions (#46 / #47).
- Regression suite: scopemask (fixed), widepref (test updated), haze / hose / finale (script space from its pointer),
  controls (card slot) pass; `saves` and `meters` fail on test automation (a translated disc's menus; PHI_A2 stops in
  state 3) - see #54.

## 5. Tasks suited to a second agent

In order of how self-contained they are (assign one at a time):

1. **#61 Other missions that switch buildings (survey)** - read-only research first: list the mission scripts' building
   switches, then picture pairs (HD radius 3 vs 0). Writes a list; fixes only if a place differs.
2. **#54 test automation** - make `tools/saves.py` work with a translated disc's menus and `framerate_audit.py`'s
   PHI_A2 walk reach state 4. Test code only.
3. **#35 Vehicle density standing still** - measurement first (`build/analysis/hd/cars_probe.py` in the main tree shows
   the method: copy it), one gate at a time; a fix only with numbers that show more moving cars at 100 than at 0.
4. **#48 Global menu phase 1** (the UI kit) - only after the player approves you for the menu; follow
   `docs/plans/2026-11-17-global-menu.md` phase by phase, one PR per phase.

The release itself (#56-#59) and anything the player must decide stay with the player and the main agent.

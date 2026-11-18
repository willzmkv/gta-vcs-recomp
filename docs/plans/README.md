# Plans

The work plans of this fork, one per front. Each task is an issue of this repository with the plan's link, a label
for its area and a milestone: **1.0.0** (before the fork's first release) or **After 1.0.0**.

**New here? Start with [Release 1.0.0](2026-11-17-release-1.0.0.md)**: the order of everything left before the
release. To take a task: an open issue of milestone 1.0.0 with no assignee - assign yourself, read `AGENTS.md` (the
contract: hooks, tests on card copies, no game data in git) and the `docs/game-internals.md` sections the plan names,
then follow the plan's steps. Close the issue with the commit that finishes it ("done when" met), and write what you
learnt into `docs/game-internals.md`.

| Plan | What | Milestone |
|---|---|---|
| [Release 1.0.0](2026-11-17-release-1.0.0.md) | The path to the release: the order, the police chase check, the test suite, CI, notes, release candidate, tag | 1.0.0 |
| [HD world overlaps](2026-11-17-hd-world-overlaps.md) | A second version of a building near the camera (the spots still seen), sectors drawn twice under two ids | 1.0.0 |
| [HD world in cutscenes](2026-11-18-hd-world-cutscenes.md) | Outdoor cutscenes show the detailed city as far as gameplay (today it falls back to ~260 m while the cutscene's cell is forced) | 1.0.0 |
| [HD world visual fixes](2026-11-14-hd-world-visual-fixes.md) | Phil's warehouse the first time, other missions that switch buildings, distant buildings sunk at sunset, the distant proxies' missing parts, the glow in free-camera shots | 1.0.0 (tasks 1, 3, 4) |
| [Population tuning](2026-11-14-population-tuning.md) | Vehicle density standing still, the crowd bands standing still, the benchmark, bands on by default | 1.0.0 (vehicle density), the rest after |
| [Cutscene loading](2026-11-17-cutscene-loading.md) | The dark sky in a cutscene's first frames, Vic missing in the airport cutscene's first frame | 1.0.0 |
| [Subtitles](2026-11-17-subtitles.md) | Oswald (done); size / shadow / background options, even letter gaps at 4K, the launcher's rows; speaker names later | 1.0.0 (S4 after) |
| [Translation PT-BR](2026-11-17-translation-ptbr.md) | Our own Portuguese from a fan translation (Gledson999, Phoda300), revised, as an optional mod - never game text in git | 1.0.0 |
| [Global menu](2026-11-17-global-menu.md) | ESC / Start: one PC menu, real pause, full screen, the launcher's look; F2 / F5 gone (in Portuguese) | 1.0.0, the last part |
| [HD characters](2026-11-14-hd-characters.md) | The cutscene hands as a tool, the checks, Vic's other outfits, the story characters | After 1.0.0 |
| [Mods switched live](2026-11-14-mods-live.md) | "Applies after a restart", the models in the scene reloaded at once, Load Game after a switch | After 1.0.0 |
| [Performance](2026-11-14-performance.md) | GPU skinning, RSL submission phases 3-4, the next costs | After 1.0.0 |
| [DualSense police radio](2026-11-17-dualsense-police-radio.md) | VC's police radio, read from the player's own VC PC install, on the controller's speaker while chased | After 1.0.0 |

The graphics tasks after 1.0.0 (a pre-HUD pass, height fog, SMAA, simulated HDR, sun rays: #3-#7) keep their specs in [FORK.md](../FORK.md#work-list).

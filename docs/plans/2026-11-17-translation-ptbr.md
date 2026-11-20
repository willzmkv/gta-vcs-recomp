# PT-BR translation: our own, revised, shipped as an optional mod

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax. **Game text is game data: never commit it to the fork
> or to this repository.** Work files live in the fork's `build/translation/` (ignored) and the finished file goes to
> the mods pack (a release asset), not to git.

**Goal:** a Brazilian Portuguese translation of the whole game that is ours: based on the player's disc's fan
translation (by **Gledson999 and Phoda300** - their names are in the game's own text, key `BAL_COL`: credit them),
revised and improved - consistent terms, punctuation, accents, natural Brazilian speech - offered in the launcher as an
optional mod. The player: "use it as a base, not as it is: we review, adapt and improve it, so it becomes our own".

## What exists

- The player's disc: its `ENGLISH.GXT` **is** the Portuguese translation (the official English is gone from it);
  `AMERICAN.GXT` is English (but not the text the game shows in English - its control texts differ; game-internals
  6n), `FRENCH`, `GERMAN`, `ITALIAN`, `SPANISH` official.
- `tools/gxt.py` (fork): `export GXT OUT.tsv [--ref REF.GXT]` (rows: table, key, text, reference),
  `import IN.tsv BASE.GXT OUT.GXT`, `info`, `--self-test`. Same texts back on all six of the disc's files.
- The working table: `build/translation/ptbr_base.tsv` (6,888 texts; reference = AMERICAN.GXT's English). Make it
  again: `python tools/vcs_disc.py extract <ISO> /ENGLISH.GXT build/analysis/gxt/ENGLISH.GXT` (set
  `MSYS_NO_PATHCONV=1` in Git Bash), same for AMERICAN, then `gxt.py export ... --ref ...`.
- The style reference: GTA San Andreas's official Brazilian Portuguese text, in `build/translation/text` (the player's
  files; how a GTA's Portuguese should read - terms, tone, slang).
- An automatic first pass (2026-11-17): **515 texts still in English**, **33 with formatting tokens unlike the
  English** (colours `~y~ ~w~`, line breaks `~n~`, button tokens `~k~ ~ACTION~`: a missing `~w~` paints the rest of
  a line; a lost `~n~` merges lines), **42 with extra spaces**. Some control texts name the wrong button (C0FLUP,
  C0VEWE2, C3DOARM) - the button prompts take the controls from the English / French now, but the words should be
  right too.

## Rules for the text

- Tokens are code: every `~x~` of the English stays, in the same order where the grammar allows; never translate a
  token's letters. `~k~ ~PDLT~` is one unit.
- Length: the help box wraps, the subtitles wrap at 24..456 units; keep lines close to the English length (the game
  has no room for very long lines in menus: check menu texts' keys - `FE*`, `MENU*` - against their English length).
- Latin-1 only (the game's font has no other letters: `gxt.py info` lists anything outside Latin-1).
- Terms: one glossary (below) for the whole game; Brazilian, informal where the English is.

## Task T1: the revision (a helper agent, then the player)

- [ ] **Step 1: the glossary.** From the SA translation and the base: weapons, vehicles, places (keep proper names:
  Vice City, Little Havana, Fort Baxter), ranks, police terms, mission verbs. `build/translation/glossary.md`.
- [ ] **Step 2: mechanical fixes** (a script in `build/translation/`): token mismatches restored from the English,
  double / trailing spaces, punctuation spacing (no space before `?` `!` `,` `.`; one after), `...` consistent.
- [ ] **Step 3: the 515 English texts** translated.
- [ ] **Step 4: the revision**, table by table (MAIN first, then each mission's table): accents, agreement, natural
  speech, the glossary's terms; every change in the TSV (the `text` column), the reason in a `note` column when not
  obvious. Work in batches of a mission at a time; a reviewer reads each batch.
- [ ] **Step 5: build and look**: `gxt.py import ptbr.tsv ENGLISH.GXT build/translation/ENGLISH.GXT`, put it in a
  mod folder (`modloader/PT-BR/ENGLISH.GXT`: the mod loader takes loose `.gxt` files by name), play: the menus, a few
  missions' cutscenes and help boxes; `prompts_check` must stay green (the button prompts read this text).
- [ ] **Step 6: the player reads** a sample (the opening missions) and approves the tone.

**Done when:** no token mismatch (the check script prints 0), no English left, the player approves the sample.

## Task T2: shipped as an optional mod

- [ ] The mod folder `PT-BR (VCS.rec)` with `ENGLISH.GXT` and a README (credits: Gledson999 and Phoda300 for the
  base; the revision: VCS.rec). Into the mods pack (the fork's release `mods-pack`, `VCS-Recomp-mods.zip`, which CI
  unpacks next to HD Hands and HD Hud - `tools/release.py` `BUNDLED_MODS`).
- [ ] The launcher's Mods page lists it (off by default? the player's call: the game's language is the player's).
- [ ] Notes: the release notes name it and its authors.

**Done when:** a fresh install from the release zip turns the mod on in the launcher and plays in Portuguese.

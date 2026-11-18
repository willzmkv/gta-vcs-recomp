# Subtitles: Oswald, its options, the launcher, the speaker names

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax. Read the fork's AGENTS.md and
> `docs/game-internals.md` 6n (button prompts) and **6n2** (the subtitles' font) first: this work extends that
> machinery. Never use ImGui for it (the player's rule: ImGui leaves with F2 when the global menu comes).

**Goal:** the subtitles (CHud's message at the bottom: cutscene lines, mission text) readable and modern - the
player's verdict on the Oswald version: "this font is sensational, it modernises the game" - with options for size,
outline, shadow, a translucent background, and later the speaker's name before the line.

## How it works now (done 2026-11-17: 6e14daa, d7eb735, 6564398)

`src/game/vcs_prompts.cpp` "the subtitles' font", `src/game/vcs_glyph_art.cpp` "sub:<char>" glyphs:
- The game keeps laying the text out; only the letters change. A fifth font style (4): the font list copied to guest
  scratch 0x64000 with a copy of "Game" at 0x64040 whose letter table (0x64100) holds Oswald SemiBold's advances
  (4 x finer measures; the scale over 4 in the `FUN_00262570` hook), a 28-unit letter box (accented capitals fit),
  texture coordinates on row 253 (column = 2 x the letter). The message drawers `FUN_0031CA80` / `FUN_003219E0` ask
  for style 1 via `FUN_003F4000` - our hook turns that into 4.
- The renderer knows a letter by its texture coordinates (`GSHwGlyphFn(xy, uv, ...)`, gs_hw) and draws its cell from
  the glyph atlas in the text's colour (`GSHwGlyphHit::tint`). Each cell is an Oswald SDF (stb_truetype) with a black
  outline, drawn at the window's size; a unit across is 0.81 of one down (`kUnitWide`), so cells are drawn in the
  screen's proportion.
- Settings: Enhancements > Subtitles: **Subtitle font** (Original / Oswald; default Original) and **Subtitle outline**
  (0-100 %, default 50). Font: `assets/fonts/oswald/Oswald-SemiBold.ttf` (4.103, OFL), embedded with the prompt icons
  by `tools/kenney_prompts.py` at configure.
- Check by hand: `prompts subtitles` on the control channel prints in / ready / calls / switched / letters / style.

---

## Task S1: options - size, shadow, background, colour

**Files:** `src/game/vcs_prompts.cpp` (settings, the scale hook, a background quad), `src/game/vcs_glyph_art.cpp`
(shadow in the cell), `docs/game-internals.md` 6n2.

- [ ] **Size** (Subtitle size: 80-150 %, default 100): multiply the scale in the `fontSetScale` hook for the copy
  (`ctx->f[12]`) - the game then wraps and centres for the bigger letters itself. Check that two-line lines still
  fit the 24..456 box (they wrap earlier).
- [ ] **Shadow** (Subtitle shadow: Off / Soft / Hard): the game draws its own drop shadow pass (`g_shadow` in
  `fontRender`, the font's shadow flag `FUN_003F4180`); with Oswald, decide: keep the game's (a dark copy of our
  letters, offset) or bake a soft shadow into the cell (blur of the SDF) and switch the game's off for style 4.
- [ ] **Background** (Subtitle background: Off / 25-75 % opacity): a translucent dark box behind each line, as in
  modern games (the player's Alan Wake 2 reference). Draw it as one more glyph primitive: the measured line box from
  the game's own layout (`FUN_003F4280` returns lines / width) or the union of the line's letter quads on the GS
  stage (classify collects them; draw the box first). Prefer the GS-stage union: no new game hooks.
- [ ] **Colour** of the text: keep the game's colours (it colours names and key words with `~y~` etc.); optionally a
  "White" choice that ignores them (classify sets the vertex colour).
- [ ] Each option live (the atlas is remade when the cell's look changes: the signature includes it).
- [ ] Pictures: Shakedown's opening cutscene (mission 50 from the Fort Baxter card, script in Task S2) at each option.

**Done when:** the options work live, checked with pictures, defaults unchanged (Original font).

## Task S2: whole-pixel letter positions at high resolutions

The game places each letter at a whole pixel of the 640 x 448 picture (Im2D's integer positions), so at 4K the gaps
between letters vary by up to 6 screen pixels. The renderer already supports sub-pixel 2D for the lamp coronas:
a vertex carrying its fraction (1/16 pixel) in the low bits of its texture coordinates (bit 4 set in both as the mark,
alpha 0x7F; gs_hw.cpp `recordPrimitive`, game-internals 9k).

- [ ] Find where the glyph routine (`FUN_00261600` -> `FUN_003B8EA0` -> Im2D `FUN_003DFCE8`) turns the float pen
  position into the vertex's integer x; hook there (before the original) to carry the fraction for style 4's letters
  in the texture coordinates' low bits - our letters' coordinates are ours anyway (row 253), so the mark cannot clash.
- [ ] The classifier reads the letter from the coordinates' high part (`lround((u0 - 0.5) / 2)` stays right).
- [ ] Pictures at 3840 x 2160 (`PS2X_HIDDEN_WINDOW=3840x2160`) before / after: even gaps.

**Done when:** at 4K the gaps between letters are even (measure: the spaces between letter ink in a line vary by at
most 1 screen pixel where the font's own spacing is equal).

## Task S3: the launcher's settings for the new options

The launcher writes the game's keys (`launcher/model/game_config.cpp`, aligned on 2026-11-17: 4f671e1). Add rows:
- Subtitles: font (Original / Oswald), outline, and S1's options as they land (`enhance.subtitle_*`).
- Button prompts: icon style (Kenney / Classic, `enhance.prompt_style`).

- [ ] Rows in the launcher's sheet (`launcher/screens/sheets.cpp`, next to "Controller Button Prompts"), tr() texts
  in pt / en, the inspector's description.
- [ ] `--self-test` covers the keys' round trip; the launcher's other pages pixel-identical
  (`--screenshot --screen ...` before / after; the launcher is "perfect": change nothing else).

## Task S4 (after 1.0.0): the speaker's name before the line

"Alan  Todos entramos em uma história..." (the player's reference, Alan Wake 2: the name in a colour, then the line).
The game's text has no speaker. Two ways:
- A table, key by key: each cutscene line's GXT key (the mission tables: `JER_A1`, ...) -> the character's name.
  Built once with help: the line's audio (the mission audio slot that plays with it, game-internals 12) names the
  speaker's ped model; a helper tool lists every line with its audio and model for a person to confirm.
- Shipped as data in the port (names only, ours: no game text).

**Done when:** a cutscene shows "Name  line", the name coloured, behind a setting (Subtitles > Speaker names).

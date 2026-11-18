# DualSense: the police radio on the controller's speaker (after 1.0.0)

> **For agentic workers:** steps use checkbox (`- [ ]`) syntax. **Never ship VC's audio**: the port reads it from the
> player's own Vice City PC install, as it reads the game from the player's disc.

**Goal:** while the police are after the player, their radio chatter comes out of the DualSense's speaker in the
player's hands, the rest of the game from the speakers - the player's wish. VCS has no police scanner (two sessions of
the player's play logged every sound and stream: none), so the radio comes from **Vice City PC** (the player: "we can
take it from the original Vice City").

## Done (40a9d4b, 2026-11-17)

The second output: the SPU2 can take chosen voices out of the mix (`IopSpu2::setAuxVoices` / `setAuxSink`), a second
stream plays on a named device (`ps2AudioAux*`, `platform::audioOpenAux`), the native sound driver picks voices per
voice block (`vcs::setSecondOutput`). `pad.ds_speaker` value 2 ("Police radio") is hidden until there is a radio.
`VCS_SOUND_VOICES=1` logs every sound a voice is given, every stream opened and the wanted level's changes.

## Task D1: VC's police radio, rebuilt

VC's police radio (`CPoliceRadio` in re3/reVC: `audio/PoliceRadio.cpp`) puts a report together from short samples:
a crime ("we have a 2-11 in progress"), the area ("in Little Havana"), the suspect ("suspect last seen in a red
sedan"), on a scanner bed with clicks. Its samples are in VC's `audio/sfx.raw` / `sfx.SDT` (or the `.wav` files of
some releases) - read them from the player's install.

- [ ] **Step 1:** read re3's `PoliceRadio.cpp` (MIT-licensed? check: re3's code has no license grant - read it as a
  reference only, write our own) for the report's grammar, timing and which sample ids form each part.
- [ ] **Step 2:** a reader for VC's `sfx.SDT` / `sfx.raw` (sample table: offset, size, rate) in `src/sound/`, the
  install's path a setting (Audio > Vice City PC folder), the files' presence checked.
- [ ] **Step 3:** the report built from the game's state: the wanted level (`ped + 0x920`), the crime (what raised
  it: VCS's `CWanted` crime list - find with eedecomp), the player's area (the zone name: VCS's `CTheZones`), the
  vehicle's colour and class (the player's car). Map VCS's zones to VC's area samples (same city).
- [ ] **Step 4:** played on the second output (`ps2AudioAuxPush`), mixed host-side (not through the SPU2): a small
  mixer of the samples at their rate to 48 kHz.
- [ ] **Step 5:** `pad.ds_speaker` "Police radio" shown again (game and launcher), help text, a check with the voice
  log.

**Done when:** with three stars and a DualSense, a report plays from the controller every so often while the chase
lasts, and none without stars.

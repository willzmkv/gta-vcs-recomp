#!/usr/bin/env python3
"""Game files from mods (src/game/vcs_stream.cpp setStreamFiles, src/mods/modloader.cpp; docs/MODS.md "Game files"):
one GTA3PS2.IMG entry replaced from a mod's file, the archive untouched.

    python tools/gamefiles_check.py [OUT] [--save DIR]

Needs build/img/streaming.json (python tools/img.py table). Three hidden runs from the card copy (default the rig's
user copy), each spawning an ADMIRAL (model 171) at a city street and taking a picture: no mod; a mod holding
admiral.mdl = the disc's own entry (must look the same: the overlay path changes nothing); a mod holding the CHEETAH's
entry as admiral.mdl (must look different: the ADMIRAL drawn as a Cheetah, no crash); and the ADMIRAL's entry - larger -
as cheetah.mdl, a CHEETAH spawned: the record moved past the disc, the game reads it there and goes on (bigger.png
shows the Admiral's body under the name Cheetah; another model spawns with another camera, so it is not compared). A fourth run takes the text
files from a mod (AMERICAN.GXT / ENGLISH.GXT with "Admiral" written "ADMIRAL"): the game's text for the key ADMIRAL
must read "ADMIRAL" (its text table in memory, *0x48A2A0). Mod folders under OUT; the
extracted entries stay there (game data: never in git). Results: OUT/result.json. Exit code 0 = pass.
"""
import argparse
import json
import os
import shutil
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import img  # noqa: E402
import mods_check  # noqa: E402
import rig  # noqa: E402
import toolchain as tc  # noqa: E402

STREET = (575.0, 175.0, 0.0)
texts = {}


def game_text(g, key):
    """The game's text for a key: *(0x48A2A0) = {entries, count, data, size}; 12-byte entries {text ptr, char key[8]}."""
    table = g.u32(0x48A2A0)
    entries, count = g.u32(table), g.u32(table + 4)
    raw = b''.join(g.peek(entries + i, 4096) for i in range(0, count * 12, 4096))
    for i in range(count):
        if raw[i * 12 + 4:i * 12 + 12].split(b'\0')[0] == key.encode():
            text = struct.unpack_from('<I', raw, i * 12)[0]
            if text < 0x100000:  # an offset into the data
                text += g.u32(table + 8)
            return g.peek(text, 64).decode('utf-16-le', 'replace').split('\0')[0]
    return None


def run(out, name, mods, save, model=171):
    env = {'VCS_TEST_MODS': '1', 'VCS_MODLOADER_DIR': str(mods)} if mods else {}
    g = rig.Game.start(save=save, name='gamefiles_' + name, seconds=300, env=env)
    try:
        g.wait_ingame(timeout=180)
        g.set_time(12)
        g.weather(0)
        g.teleport(STREET[0], STREET[1], -100.0, heading=STREET[2])
        time.sleep(4)
        g.spawn_car(model, ahead=4.0)
        time.sleep(5)
        shot = out / (name + '.png')
        g.cmd('screenshot ' + str(shot))
        texts[name] = game_text(g, 'ADMIRAL')
        alive = g.alive()
    finally:
        g.quit()
    return shot, Path(g.log_path).read_text(encoding='utf-8', errors='replace'), alive


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?')
    ap.add_argument('--save', default=os.path.join(tc.ROOT, 'build', 'rig', 'saves', 'user'))
    a = ap.parse_args()
    out = Path(a.out or os.path.join(tc.ROOT, 'build', 'gamefiles_check', time.strftime('%Y%m%d-%H%M%S'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    table = img.load_table()
    by_name = {e.get('name'): e for e in table['entries'] if e.get('kind') != 'txd'}  # models (a TXD has its car's name)
    entries = out / 'entries'
    with open(img.iso_path(None), 'rb') as iso:
        def entry(name):
            e = by_name[name]
            iso.seek((table['img']['lsn'] + e['offset']) * 2048)
            return iso.read(e['sectors'] * 2048)
        admiral, cheetah = entry('admiral'), entry('cheetah')
        gxts = {}
        for name, lsn, size in (('AMERICAN.GXT', 1053706, 586214), ('ENGLISH.GXT', 755863, 621324)):  # tools/vcs_disc.py ls
            iso.seek(lsn * 2048)
            gxts[name] = iso.read(size).replace('Admiral'.encode('utf-16-le'), 'ADMIRAL'.encode('utf-16-le'))
    text_mod = out / 'modloader_text' / 'Test text'
    if text_mod.parent.is_dir():
        shutil.rmtree(text_mod.parent)
    text_mod.mkdir(parents=True)
    for name, data in gxts.items():
        (text_mod / name).write_bytes(data)
    bigger = out / 'modloader_bigger' / 'Test bigger'  # the ADMIRAL's entry (37 sectors) as cheetah.mdl (30): larger
    if bigger.parent.is_dir():
        shutil.rmtree(bigger.parent)
    bigger.mkdir(parents=True)
    (bigger / 'cheetah.mdl').write_bytes(admiral)
    for mod, data in (('same', admiral), ('swap', cheetah)):
        folder = out / ('modloader_' + mod) / ('Test ' + mod)
        if folder.parent.is_dir():
            shutil.rmtree(folder.parent)
        folder.mkdir(parents=True)
        (folder / 'admiral.mdl').write_bytes(data)
    shots, logs, alive = {}, {}, {}
    for name, mods in (('original', None), ('same', out / 'modloader_same'), ('swap', out / 'modloader_swap'),
                       ('text', out / 'modloader_text'), ('bigger', out / 'modloader_bigger')):
        shots[name], logs[name], alive[name] = run(out, name, mods, a.save, 172 if name == 'bigger' else 171)
    result = {name: mods_check.picture_stats(shots[name], shots['original']) for name in shots}
    result['resolved'] = {n: 'admiral.mdl -> GTA3PS2.IMG entry 171' in logs[n] for n in ('same', 'swap')}
    result['alive'] = alive
    result['texts'] = texts
    result['moved'] = ('cheetah.mdl -> GTA3PS2.IMG entry 172' in logs['bigger'] and 'larger than the disc' in logs['bigger'] and
                       "read of a mod's moved file" in logs['bigger'])
    result['text_file'] = [l.split(' -> ')[1] for l in logs['text'].splitlines() if 'GXT -> disc file' in l]
    result['pass'] = (all(result['resolved'].values()) and all(alive.values()) and
                      result['swap']['diff_from_original'] > max(3 * result['same']['diff_from_original'], 1.0) and
                      texts.get('original') == 'Admiral' and texts.get('text') == 'ADMIRAL' and result['moved'] and
                      not any('[crash]' in t for t in logs.values()))
    (out / 'result.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    print(json.dumps(result, indent=1))
    print('PASS' if result['pass'] else 'FAIL', '-', out)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())

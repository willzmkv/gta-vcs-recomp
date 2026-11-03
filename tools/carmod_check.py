#!/usr/bin/env python3
"""PC VC car mods in the game (FORK.md T9, docs/MODS.md "PC VC cars in VCS"): spawned and photographed.

    python tools/carmod_check.py [OUT] [--dff FILE] [--txd FILE] [--vehicle admiral] [--model 171] [--save DIR]
    python tools/carmod_check.py [OUT] --direct [...]               the .dff / .txd as they are: the game converts them
    python tools/carmod_check.py [OUT] --folder DIR --models 266,172  a whole PC mod folder; those models spawned
    ... --views                                                       each parked and photographed from four sides

Default: the .dff converted onto the VCS vehicle's model by tools/dff2mdl.py, its .txd by tools/txd2xtx.py, the results
(`admiral.mdl`, `admiral.xtx`) in a mod. --direct: the PC files themselves, named as the vehicle (`admiral.dff`,
`admiral.txd`) - the mod loader converts them (src/mods/rwconv.cpp). --folder: a copy of a PC mod folder as one mod
(the mod loader takes each `.dff` named as a VCS vehicle, with its `.txd`).

Needs the extracted vehicles for the default mode (python tools/img.py extract). Hidden runs from the card copy
(default the rig's user copy) at a city street: the original vehicle (not with --folder), then the mod (mod folder
under OUT - game data: never in git), the player put in each car spawned and the picture taken from behind. Pass =
the files resolved (converted, for PC files) by the mod loader, the game alive, no crash, a different picture.
Results: OUT/result.json. Exit code 0 = pass.
"""
import argparse
import json
import math
import os
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dff2mdl  # noqa: E402
import gamefiles_check  # noqa: E402
import mods_check  # noqa: E402
import rig  # noqa: E402
import toolchain as tc  # noqa: E402
import txd2xtx  # noqa: E402

DEFAULT_DFF = os.path.join(tc.ROOT, 'Mods', 'Fixed_Xbox_Vehs_1.3', 'Comet with PC style body and lights', 'comet.dff')
DEFAULT_TXD = os.path.join(tc.ROOT, 'Mods', 'Fixed_Xbox_Vehs_1.3', 'Fixed Xbox Vehicles', 'gta3.img', 'comet.txd')


VIEWS = (("front", -4.5, 4.5, 1.8), ("rear", 4.5, -4.5, 1.8), ("side", -6.5, 0.0, 0.8), ("top", 0.0, -0.3, 8.0))


def park_and_look(g, out, name, models):
    """Each model parked in a row on the sand (facing north, 9 m apart) and photographed by the free camera from four
    sides (the rig's campose): front-left, rear-right, the left side, from above. [pictures]"""
    S = rig.Scm
    local = [bytes([0x0D + k]) for k in range(4)]
    _, (px, py, pz), _ = g.player()
    shots = []
    for n, model in enumerate(models):
        x, y = px + 9.0 * n, py + 9.0
        ints, floats = g.scm_outputs(
            S.op(S.REQUEST_MODEL, S.i(model)) + S.op(S.LOAD_ALL_MODELS_NOW) +
            S.op(S.CREATE_CAR, S.i(model), S.f(x), S.f(y), S.f(pz + 1.0), local[0]) +
            S.op(S.SET_CAR_COORDINATES, local[0], S.f(x), S.f(y), S.f(-100.0)) +
            S.op(0x00D1, local[0], S.f(0.0)) +  # SET_CAR_HEADING: north
            S.op(0x004D, local[0], local[1], local[2], local[3]) +  # GET_CAR_COORDINATES
            S.op(S.MARK_MODEL_AS_NO_LONGER_NEEDED, S.i(model)), 4)
        cx, cy, cz = floats[1], floats[2], floats[3]
        time.sleep(2)
        for view, dx, dy, dz in VIEWS:
            yaw = math.degrees(math.atan2(-dy, -dx)) if (dx or dy) else 90.0
            pitch = math.degrees(math.atan2(-dz + 0.3, math.hypot(dx, dy)))
            g.cmd('campose %.3f %.3f %.3f %.2f %.2f' % (cx + dx, cy + dy, cz + dz, yaw, pitch))
            time.sleep(1.5)
            shot = out / ('%s_%d_%s.png' % (name, model, view))
            g.cmd('screenshot ' + str(shot))
            shots.append(shot)
        g.cmd('campose off')
    return shots


def run(out, name, mods, save, models, views=False):
    """One hidden run: each model spawned at gamefiles_check's street with the player in it, photographed from behind.
    ([pictures], log, alive)"""
    env = {'VCS_TEST_MODS': '1', 'VCS_MODLOADER_DIR': str(mods)} if mods else {}
    g = rig.Game.start(save=save, name='carmod_' + name, seconds=150 + 40 * len(models), env=env)
    x, y, heading = gamefiles_check.STREET
    shots = []
    try:
        g.wait_ingame(timeout=180)
        g.set_time(12)
        g.weather(0)
        # the game puts the player back at the save's place in its first seconds, after a teleport too: until the
        # player stays there (two looks 3 s apart)
        for _ in range(6):
            g.teleport(x, y, -100.0, heading=heading)
            there = []
            for _ in range(2):
                time.sleep(3)
                _, (px, py, _pz), _ = g.player()
                there.append(abs(px - x) + abs(py - y) < 10.0)
            if all(there):
                break
        if views:
            shots = park_and_look(g, out, name, models)
        for model in ([] if views else models):
            g.spawn_car(model, ahead=4.0)
            time.sleep(5)
            shot = out / ('%s_%d.png' % (name, model) if len(models) > 1 else name + '.png')
            g.cmd('screenshot ' + str(shot))
            shots.append(shot)
        alive = g.alive()
    finally:
        g.quit()
    return shots, Path(g.log_path).read_text(encoding='utf-8', errors='replace'), alive


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', nargs='?')
    ap.add_argument('--dff', default=DEFAULT_DFF)
    ap.add_argument('--txd', default=DEFAULT_TXD, help="the car's textures ('' for none: the template's names stay)")
    ap.add_argument('--vehicle', default='admiral', help='the VCS vehicle replaced (its model is the template)')
    ap.add_argument('--model', type=int, default=171, help="the vehicle's model number (spawned)")
    ap.add_argument('--direct', action='store_true', help='the PC files in the mod: the game converts them')
    ap.add_argument('--folder', help='a whole PC mod folder as one mod')
    ap.add_argument('--models', default='', help='with --folder: the model numbers spawned (comma separated)')
    ap.add_argument('--views', action='store_true', help='park each model and photograph it from four sides')
    ap.add_argument('--save', default=os.path.join(tc.ROOT, 'build', 'rig', 'saves', 'user'))
    ap.add_argument('--no-original', action='store_true', help='skip the run without the mod')
    a = ap.parse_args()
    out = Path(a.out or os.path.join(tc.ROOT, 'build', 'carmod_check', time.strftime('%Y%m%d-%H%M%S'))).resolve()
    out.mkdir(parents=True, exist_ok=True)
    mods = out / 'modloader'
    if mods.is_dir():
        shutil.rmtree(mods)
    models = [int(m) for m in a.models.split(',') if m] if a.folder else [a.model]
    if a.folder:
        shutil.copytree(a.folder, mods / ('Test ' + Path(a.folder).name))  # its own name: the player's F2 switch for theirs stays theirs
    else:
        folder = mods / ('Car ' + Path(a.dff).stem)
        folder.mkdir(parents=True)
        if a.direct:
            shutil.copyfile(a.dff, folder / (a.vehicle + '.dff'))
            if a.txd:
                shutil.copyfile(a.txd, folder / (a.vehicle + '.txd'))
        else:
            template = os.path.join(tc.ROOT, 'build', 'img', 'GTA3PS2', a.vehicle + '.mdl')
            dff2mdl.convert(a.dff, template, str(folder / (a.vehicle + '.mdl')), a.txd or None)
            if a.txd:
                txd2xtx.convert(a.txd, str(folder / (a.vehicle + '.xtx')))
    runs = {} if (a.no_original or a.folder) else {'original': None}
    runs['converted'] = mods
    shots, logs, alive = {}, {}, {}
    for name, m in runs.items():
        shots[name], logs[name], alive[name] = run(out, name, m, a.save, models, a.views)
    log = logs['converted']
    result = {'alive': alive, 'crash': any('[crash]' in t for t in logs.values()),
              'converted': len(re.findall(r'\] .* converted in ', log)), 'failed': re.findall(r'\[mods\] .* FAILED.*', log)[:10],
              'entries': len(re.findall(r'-> GTA3PS2\.IMG entry', log))}
    if not a.folder:
        model_ext, tex_ext = ('.dff', '.txd') if a.direct else ('.mdl', '.xtx')
        result['resolved'] = ('%s%s -> GTA3PS2.IMG entry %d' % (a.vehicle, model_ext, a.model)) in log
        result['textures'] = ((a.vehicle + tex_ext + ' -> GTA3PS2.IMG entry') in log) if a.txd else None
    if 'original' in shots:
        result['picture'] = mods_check.picture_stats(shots['converted'][0], shots['original'][0])
    result['pass'] = (result.get('resolved', True) and result.get('textures') is not False and all(alive.values()) and
                      not result['crash'] and not result['failed'] and
                      ('picture' not in result or result['picture']['diff_from_original'] > 1.0))
    (out / 'result.json').write_text(json.dumps(result, indent=1), encoding='utf-8')
    print(json.dumps(result, indent=1))
    print('PASS' if result['pass'] else 'FAIL', '-', out)
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    sys.exit(main())

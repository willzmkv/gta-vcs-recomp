#!/usr/bin/env python3
"""The mod loader's converters (src/mods/rwconv.cpp, run as `vcs.exe --rwconv-dff / --rwconv-txd`) against the tools
they port (tools/dff2mdl.py, tools/txd2xtx.py): every .dff and .txd under a folder converted by both, byte for byte.

    python tools/rwconv_check.py [FOLDER] [--out DIR] [--limit N]

FOLDER: default Mods/ (the player's PC mods). A .dff goes onto the VCS vehicle of its name when there is one (else the
ADMIRAL), with the .txd beside it when there is one; a .txd keeps the textures of the VCS dictionary of its name;
extracted vehicles and dictionaries needed (python tools/img.py extract build/img/GTA3PS2 "*"). Outputs
under build/rwconv_check (game data: never in git). Exit code 0 = every file the same (files both refuse count as
the same).
"""
import argparse
import contextlib
import io
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dff2mdl  # noqa: E402
import toolchain as tc  # noqa: E402
import txd2xtx  # noqa: E402

VEHICLES = os.path.join(tc.ROOT, 'build', 'img', 'GTA3PS2')


def python_side(fn, *args):
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            fn(*args)
        return True
    except Exception as e:  # noqa: BLE001 - a file the tool refuses: compared as a refusal
        return 'refused: %s' % e


def native_side(exe, *args):
    r = subprocess.run([exe] + list(args), capture_output=True, text=True, timeout=300)
    return r.returncode == 0, (r.stdout or r.stderr).strip()


def same(a, b):
    try:
        return open(a, 'rb').read() == open(b, 'rb').read()
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folder', nargs='?', default=os.path.join(tc.ROOT, 'Mods'))
    ap.add_argument('--out', default=os.path.join(tc.ROOT, 'build', 'rwconv_check', time.strftime('%Y%m%d-%H%M%S')))
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    exe = os.path.join(tc.BUILD, 'vcs.exe' if os.name == 'nt' else 'vcs')
    os.makedirs(a.out, exist_ok=True)
    files = []
    for root, _dirs, names in os.walk(a.folder):
        for name in sorted(names):
            if name.lower().endswith(('.dff', '.txd')):
                files.append(os.path.join(root, name))
    files.sort()
    if a.limit:
        files = files[:a.limit]
    results = {'same': 0, 'differ': [], 'refused_both': 0}
    for i, path in enumerate(files):
        stem, ext = os.path.splitext(os.path.basename(path))
        tag = '%04d_%s' % (i, stem)
        py, cc = os.path.join(a.out, tag + '.py' + ext), os.path.join(a.out, tag + '.cc' + ext)
        if ext.lower() == '.dff':
            template = os.path.join(VEHICLES, dff2mdl.VC_NAMES.get(stem.lower(), stem.lower()) + '.mdl')
            if not os.path.isfile(template):
                template = os.path.join(VEHICLES, 'admiral.mdl')
            txd = os.path.join(os.path.dirname(path), stem + '.txd')
            txd = txd if os.path.isfile(txd) else None
            p = python_side(dff2mdl.convert, path, template, py, txd)
            ok, msg = native_side(exe, '--rwconv-dff', path, template, cc, *([txd] if txd else []))
        else:  # with the VCS dictionary of its name, when there is one: its other textures are kept
            keep = os.path.join(VEHICLES, dff2mdl.VC_NAMES.get(stem.lower(), stem.lower()) + '.xtx')
            keep = keep if os.path.isfile(keep) else None
            p = python_side(txd2xtx.convert, path, py, 128, None, keep)
            ok, msg = native_side(exe, '--rwconv-txd', path, cc, *([keep] if keep else []))
        if p is not True and not ok:
            results['refused_both'] += 1
            continue
        if p is True and ok and same(py, cc):
            results['same'] += 1
            for f in (py, cc):
                os.remove(f)
        else:
            results['differ'].append({'file': os.path.relpath(path, a.folder), 'python': p, 'native': msg})
    print('rwconv_check: %d files - %d the same, %d refused by both, %d differ' % (
        len(files), results['same'], results['refused_both'], len(results['differ'])))
    for d in results['differ'][:20]:
        print('  differ:', d)
    print('PASS' if not results['differ'] else 'FAIL', '-', a.out)
    return 0 if not results['differ'] else 1


if __name__ == '__main__':
    sys.exit(main())

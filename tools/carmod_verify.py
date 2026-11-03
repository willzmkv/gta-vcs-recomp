#!/usr/bin/env python3
"""A converted VC car checked triangle by triangle against its .dff (tools/dff2mdl.py, src/mods/rwconv.cpp; FORK.md T9).

    python tools/carmod_verify.py FOLDER [--out DIR] [--native]   every .dff in FOLDER with a VCS vehicle of its name
    python tools/carmod_verify.py CAR.dff CONVERTED.mdl [--txd CAR.txd]

Both cars are put together the way they are drawn intact: the VC one from its frames (every part but the damaged ones,
the extras and the low-detail ones), the converted one from its node tree as the converted file now holds it (each
element's PS2 meshes decoded; an X_dummy's intact element - object flag 4 -, no LOD, no extras, one of each
helicopter rotor pair). Every VC triangle has to be in the converted car at the same place (5 mm), with the same
texture and the same UVs (whole repeats aside; the 8-bit UV step allowed), and the converted car must hold no
triangle the VC car does not. --native converts with vcs.exe --rwconv-dff instead of the Python tool. Extracted
vehicles needed (python tools/img.py extract). Exit code 0 = every car matches.
"""
import itertools
import math
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dff2mdl  # noqa: E402
import mdlinfo  # noqa: E402
import rwinfo  # noqa: E402
import toolchain as tc  # noqa: E402

TOL = 0.005
GRID = 0.05


def vc_triangles(dff, textures, damaged=()):
    """[(3 positions, 3 uvs, texture, part)] of the VC car in the root's space: intact, or with the X_hi_dam parts of
    the dummies in `damaged` in place of their X_hi_ok."""
    frames = dff["frames"]
    names = {frames[a["frame"]]["name"].lower() for a in dff["atomics"]}
    out = []
    for a in dff["atomics"]:
        name = frames[a["frame"]]["name"].lower()
        base = name.rsplit("_hi_", 1)[0]
        if name.endswith("_hi_dam") and base + "_dummy" in damaged:
            pass
        elif name.endswith("_hi_ok") and base + "_dummy" in damaged and base + "_hi_dam" in names:
            continue
        elif name.endswith("_dam") or name.startswith("extra") or name.endswith("_vlo"):
            continue
        mat, i = dff2mdl.IDENTITY, a["frame"]
        while frames[i]["parent"] >= 0:
            mat = dff2mdl.mat_mul(mat, (tuple(frames[i]["rot"]), tuple(frames[i]["pos"])))
            i = frames[i]["parent"]
        g = dff["geometries"][a["geometry"]]
        verts = [dff2mdl.mat_apply(mat, v) for v in g["verts"]]
        for x, y, z, mi in g["tris"]:
            if x == y or y == z or x == z:
                continue
            tex = g["materials"][mi]["texture"].lower() if mi < len(g["materials"]) else ""
            if textures is not None and tex and tex not in textures:
                tex = dff2mdl.VC_GENERIC.get(tex, "")
            out.append(((verts[x], verts[y], verts[z]), (g["uvs"][x], g["uvs"][y], g["uvs"][z]), tex, name))
    return out


def damaged_nodes(m):
    """The dummies with a damaged element (object flag 4 clear)."""
    group = m.ptr(0x20)
    return {m.cstr(m.ptr(m.ptr(e + 4) + 0xA8)).lower() for e in m.elements(group) if not m.d[e + 2] & 4}


def vcs_triangles(m, damaged=False):
    """The converted car's triangles in the root's space: [(3 positions, 3 uvs, texture, node)] - intact, or with every
    dummy that has a damaged element (object flag 4 clear) showing that instead."""
    group = m.ptr(0x20)
    root = m.ptr(group + 4)
    extras = {m.ptr(m.ptr(0x28) + 4 * k) for k in range(m.u32(0x24))} if m.ptr(0x28) and m.u32(0x24) <= 64 else set()

    def rr(n):
        mat = dff2mdl.IDENTITY
        while n and n != root:
            rows = [struct.unpack_from("<3f", m.d, n + 0x10 + 16 * k) for k in range(4)]
            mat = dff2mdl.mat_mul(mat, (rows[0] + rows[1] + rows[2], rows[3]))
            n = m.ptr(n + 4)
        return mat

    out = []
    for e in m.elements(group):
        node = m.ptr(e + 4)
        name = m.cstr(m.ptr(node + 0xA8)).lower()
        # intact: an X_dummy's elements with object flag 4; damaged: those without, where the dummy has any
        shown = bool(m.d[e + 2] & 4)
        if damaged and name.endswith("_dummy") and name in damaged_nodes(m):
            shown = not shown
        if e in extras or name.endswith("_vlo") or name.startswith(("moving_rotor", "extra")) or (name.endswith("_dummy") and not shown):
            continue
        geom = m.ptr(e + 0x14)
        if not geom:
            continue
        g = m.geometry(geom)
        mat = rr(node)
        for mesh in g["meshes"]:
            matinfo = g["materials"][mesh["material"]] if 0 <= mesh["material"] < len(g["materials"]) else None
            tex = (matinfo["texture"] or "").lower() if matinfo else ""
            vs = mesh["verts"]
            step = max(mesh["uv_scale"]) / 127.5  # one 8-bit UV step
            for a, b, c in mesh["faces"]:
                p = [dff2mdl.mat_apply(mat, vs[k][:3]) for k in (a, b, c)]
                if p[0] == p[1] or p[1] == p[2] or p[0] == p[2]:
                    continue
                out.append((tuple(p), tuple(vs[k][3:5] for k in (a, b, c)), tex, name, step))
    return out


def area(p):
    u = [p[1][k] - p[0][k] for k in range(3)]
    v = [p[2][k] - p[0][k] for k in range(3)]
    c = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    return math.sqrt(c[0] ** 2 + c[1] ** 2 + c[2] ** 2) / 2


def sliver(p):
    """No width to see: area under a thousandth of the longest edge squared (exporters leave such triangles; the
    16-bit positions give them a hair of area)."""
    edge = max(sum((p[i][k] - p[(i + 1) % 3][k]) ** 2 for k in range(3)) for i in range(3))
    return area(p) <= 1e-3 * edge


def key(p):
    return tuple(int(math.floor(sum(q[k] for q in p) / 3 / GRID)) for k in range(3))


def same_place(p, q):
    """The same three corners in any order, within TOL; returns the order of q matching p (the closest of the six -
    corners of a small triangle can be closer to each other than TOL), or None."""
    best = None
    for order in itertools.permutations(range(3)):
        d = [max(abs(p[n][k] - q[order[n]][k]) for k in range(3)) for n in range(3)]
        if max(d) <= TOL and (best is None or sum(d) < best[0]):
            best = (sum(d), list(order))
    return best[1] if best else None


def uv_fits(t, q, order):
    """The same UVs up to whole repeats (one shift for the triangle), within one 8-bit step and a little."""
    du = [q[1][order[n]][0] - t[1][n][0] for n in range(3)]
    dv = [q[1][order[n]][1] - t[1][n][1] for n in range(3)]
    su, sv = round(du[0]), round(dv[0])
    tol = q[4] + 0.005
    return max(abs(d - su) for d in du) <= tol and max(abs(d - sv) for d in dv) <= tol, du, dv


def near(index, tris, t, skip=()):
    """(i, order) of the triangles of `tris` (indexed by key) at t's place, `skip` aside."""
    k = key(t[0])
    out = []
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for i in index.get((k[0] + dx, k[1] + dy, k[2] + dz), ()):
                    if i not in skip:
                        order = same_place(t[0], tris[i][0])
                        if order is not None:
                            out.append((i, order))
    return out


def compare(vc, vcs):
    """Each VC triangle to a converted one at its place (its texture and UVs checked); what is left over on either side
    is reported only where the other car has nothing at that place (coplanar copies are matched one way or the other)."""
    index, vc_index = {}, {}
    for i, t in enumerate(vcs):
        index.setdefault(key(t[0]), []).append(i)
    for i, t in enumerate(vc):
        vc_index.setdefault(key(t[0]), []).append(i)
    used = set()
    missing, bad_tex, bad_uv = [], [], []
    for t in vc:
        if sliver(t[0]):
            continue
        hits = near(index, vcs, t, used)  # unused converted triangles at the same place; same texture and UVs first
        hits.sort(key=lambda h: (vcs[h[0]][2] != t[2], not (not t[2] or uv_fits(t, vcs[h[0]], h[1])[0])))
        hit = hits[0] if hits else None
        if not hit:
            if not near(index, vcs, t):
                missing.append(t[3])
            continue
        i, order = hit
        used.add(i)
        if vcs[i][2] != t[2] or (t[2] and not uv_fits(t, vcs[i], order)[0]):
            # coplanar copies: another converted triangle at this place (taken by a copy) may be the right one
            for j, o in near(index, vcs, t):
                if vcs[j][2] == t[2] and (not t[2] or uv_fits(t, vcs[j], o)[0]):
                    i, order = j, o
                    break
        if vcs[i][2] != t[2]:
            bad_tex.append((t[3], t[2], vcs[i][2]))
            continue
        if t[2]:
            fits, du, dv = uv_fits(t, vcs[i], order)
            if not fits:
                bad_uv.append((t[3], [round(d, 3) for d in du], [round(d, 3) for d in dv]))
    extra = [vcs[i][3] for i in range(len(vcs)) if i not in used and not sliver(vcs[i][0]) and not near(vc_index, vc, vcs[i])]
    return missing, bad_tex, bad_uv, extra


def summarise(names):
    out = {}
    for n in names:
        out[n] = out.get(n, 0) + 1
    return ", ".join("%s %d" % kv for kv in sorted(out.items(), key=lambda kv: -kv[1])[:6])


def verify(dff_path, mdl_path, txd_path):
    dff = dff2mdl.read_dff(open(dff_path, "rb").read())
    textures = None
    if txd_path:
        textures = {t["name"].lower() for t in rwinfo.read_txd(open(txd_path, "rb").read())["textures"]}
    m = mdlinfo.Mdl(open(mdl_path, "rb").read())
    ok = True
    for state in ("intact", "damaged"):
        vc = vc_triangles(dff, textures, damaged_nodes(m) if state == "damaged" else ())
        missing, bad_tex, bad_uv, extra = compare(vc, vcs_triangles(m, state == "damaged"))
        good = not missing and not bad_tex and not bad_uv and not extra
        ok = ok and good
        report(dff_path, state, len(vc), good, missing, bad_tex, bad_uv, extra)
    return ok


def report(dff_path, state, count, ok, missing, bad_tex, bad_uv, extra):
    print("%-10s %-7s %5d triangles: %s%s%s%s%s" % (
        os.path.basename(dff_path)[:-4], state, count, "ok" if ok else "",
        (" missing %d (%s)" % (len(missing), summarise(missing))) if missing else "",
        (" texture %d (%s)" % (len(bad_tex), bad_tex[0])) if bad_tex else "",
        (" uv %d (%s)" % (len(bad_uv), bad_uv[0])) if bad_uv else "",
        (" extra %d (%s)" % (len(extra), summarise(extra))) if extra else ""))
    return ok


def main():
    args = sys.argv[1:]
    native = "--native" in args
    args = [a for a in args if a != "--native"]
    txd = args[args.index("--txd") + 1] if "--txd" in args else None
    out = args[args.index("--out") + 1] if "--out" in args else os.path.join(tc.ROOT, "build", "carmod_verify")
    pos = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] not in ("--txd", "--out"))]
    if len(pos) == 2:
        return 0 if verify(pos[0], pos[1], txd) else 1
    if len(pos) != 1:
        print(__doc__)
        return 1
    os.makedirs(out, exist_ok=True)
    folder, vehicles = pos[0], os.path.join(tc.ROOT, "build", "img", "GTA3PS2")
    exe = os.path.join(tc.BUILD, "vcs.exe" if os.name == "nt" else "vcs")
    total = good = 0
    for name in sorted(os.listdir(folder)):
        stem, ext = os.path.splitext(name)
        template = os.path.join(vehicles, dff2mdl.VC_NAMES.get(stem.lower(), stem.lower()) + ".mdl")
        if ext.lower() != ".dff" or not os.path.isfile(template):
            continue
        txd_path = os.path.join(folder, stem + ".txd")
        txd_path = txd_path if os.path.isfile(txd_path) else None
        mdl = os.path.join(out, stem.lower() + ".mdl")
        if native:
            subprocess.run([exe, "--rwconv-dff", os.path.join(folder, name), template, mdl] + ([txd_path] if txd_path else []),
                           capture_output=True, timeout=300)
        else:
            with open(os.devnull, "w") as null:
                saved, sys.stdout = sys.stdout, null
                try:
                    dff2mdl.convert(os.path.join(folder, name), template, mdl, txd_path)
                finally:
                    sys.stdout = saved
        total += 1
        good += verify(os.path.join(folder, name), mdl, txd_path)
    print("carmod_verify: %d / %d cars match their .dff" % (good, total))
    return 0 if good == total and total else 1


if __name__ == "__main__":
    sys.exit(main())

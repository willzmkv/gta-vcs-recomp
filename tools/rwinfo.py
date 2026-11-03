#!/usr/bin/env python3
"""PC GTA RenderWare files, read on the host (FORK.md T9, docs/MODS.md ".dff / .txd"): what a .dff / .txd holds.

    python tools/rwinfo.py FILE.dff|FILE.txd [...]   frames (names, parents), geometries (vertices, triangles,
                                                     materials and their texture names), atomics; textures (name,
                                                     size, format, levels)
    python tools/rwinfo.py --self-test               writes a small clump and texture dictionary under build/ and
                                                     reads them back

RenderWare's binary stream: chunks of {type u32, size u32, library id u32} then `size` bytes; containers hold chunks,
data sits in a Struct (1). Clump 0x10 -> Struct, FrameList 0x0E (Struct of frames, Extensions with the frame name
0x0253F2FE), GeometryList 0x1A (Geometry 0x0F: Struct, MaterialList 0x08 -> Material 0x07 -> Texture 0x06 -> String
0x02 names), Atomics 0x14. TexDictionary 0x16 -> TextureNative 0x15 (D3D8 for VC: platform 8; D3D9 for SA: 9).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

STRUCT, STRING, EXTENSION, TEXTURE, MATERIAL, MATLIST, FRAMELIST, GEOMETRY, CLUMP, ATOMIC, TEXNATIVE, TEXDICT, GEOMLIST = (
    0x01, 0x02, 0x03, 0x06, 0x07, 0x08, 0x0E, 0x0F, 0x10, 0x14, 0x15, 0x16, 0x1A)
FRAME_NAME = 0x0253F2FE


def version(libid):
    if libid & 0xFFFF0000:
        return ((libid >> 14) & 0x3FF00) + 0x30000 | ((libid >> 16) & 0x3F)
    return libid << 8


def chunks(data, at, end):
    while at + 12 <= end:
        kind, size, libid = struct.unpack_from("<3I", data, at)
        yield kind, at + 12, min(at + 12 + size, end), libid
        at += 12 + size


def child(data, at, end, kind):
    for k, a, e, _ in chunks(data, at, end):
        if k == kind:
            return a, e
    return None


def string(data, at, end):
    s = child(data, at, end, STRING)
    return data[s[0]:s[1]].split(b"\0")[0].decode("latin1") if s else ""


def read_dff(data):
    out = {"frames": [], "geometries": [], "atomics": []}
    top = next(chunks(data, 0, len(data)))
    if top[0] != CLUMP:
        raise ValueError("not a clump (chunk 0x%x)" % top[0])
    out["version"] = "%x" % version(top[3])
    for kind, a, e, _ in chunks(data, top[1], top[2]):
        if kind == FRAMELIST:
            s = child(data, a, e, STRUCT)
            n = struct.unpack_from("<I", data, s[0])[0]
            for i in range(n):
                parent = struct.unpack_from("<i", data, s[0] + 4 + i * 56 + 48)[0]
                out["frames"].append({"parent": parent, "name": ""})
            i = 0
            for k, xa, xe, _ in chunks(data, s[1], e):
                if k == EXTENSION and i < n:
                    for pk, pa, pe, _ in chunks(data, xa, xe):
                        if pk == FRAME_NAME:
                            out["frames"][i]["name"] = data[pa:pe].decode("latin1")
                    i += 1
        elif kind == GEOMLIST:
            for k, ga, ge, libid in chunks(data, a, e):
                if k != GEOMETRY:
                    continue
                s = child(data, ga, ge, STRUCT)
                flags, tris, verts, morphs = struct.unpack_from("<I3I", data, s[0])
                geom = {"vertices": verts, "triangles": tris, "flags": "%x" % flags, "native": bool(flags & 0x01000000),
                        "materials": []}
                ml = child(data, ga, ge, MATLIST)
                for mk, ma, me, _ in chunks(data, ml[0], ml[1]) if ml else ():
                    if mk == MATERIAL:
                        tex = child(data, ma, me, TEXTURE)
                        rgba = struct.unpack_from("<4B", data, child(data, ma, me, STRUCT)[0] + 4)
                        geom["materials"].append({"colour": "%02x%02x%02x%02x" % rgba,
                                                  "texture": string(data, *tex) if tex else None})
                out["geometries"].append(geom)
        elif kind == ATOMIC:
            s = child(data, a, e, STRUCT)
            frame, geometry = struct.unpack_from("<2I", data, s[0])
            out["atomics"].append({"frame": frame, "geometry": geometry})
    return out


def read_txd(data):
    top = next(chunks(data, 0, len(data)))
    if top[0] != TEXDICT:
        raise ValueError("not a texture dictionary (chunk 0x%x)" % top[0])
    textures = []
    for kind, a, e, _ in chunks(data, top[1], top[2]):
        if kind != TEXNATIVE:
            continue
        s = child(data, a, e, STRUCT)
        platform = struct.unpack_from("<I", data, s[0])[0]
        name = data[s[0] + 8:s[0] + 40].split(b"\0")[0].decode("latin1")
        raster, fmt = struct.unpack_from("<2I", data, s[0] + 72)
        w, h, depth, levels, _, compression = struct.unpack_from("<2H4B", data, s[0] + 80)
        if platform == 9:
            fourcc = struct.pack("<I", fmt)
            kind_ = fourcc.decode("latin1") if fourcc.isalnum() else "d3dfmt %d" % fmt
        else:
            kind_ = "DXT%d" % compression if compression else "raster 0x%x, %d bit" % (raster & 0xF00, depth)
        textures.append({"name": name, "width": w, "height": h, "format": kind_, "levels": levels, "platform": platform})
    return {"textures": textures}


# ---- the self-test's writer (minimal, enough for the reader) -----------------------------------------------------
def chunk(kind, payload, libid=0x1003FFFF):  # 3.4.0.3 (VC PC's)
    return struct.pack("<3I", kind, len(payload), libid) + payload


def write_dff():
    frames = struct.pack("<I", 2)
    for parent in (-1, 0):
        frames += struct.pack("<12f", 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0) + struct.pack("<iI", parent, 0)
    names = b"".join(chunk(EXTENSION, chunk(FRAME_NAME, n)) for n in (b"chassis_dummy", b"wheel_lf_dummy"))
    framelist = chunk(FRAMELIST, chunk(STRUCT, frames) + names)
    geom_struct = struct.pack("<I3I", 0x76, 1, 3, 1) + b"\0" * 64
    material = chunk(MATERIAL, chunk(STRUCT, struct.pack("<I4BI3fI", 0, 255, 255, 255, 255, 0, 1, 1, 1, 1)) +
                     chunk(TEXTURE, chunk(STRUCT, struct.pack("<I", 0x1106)) + chunk(STRING, b"admiral868bit\0\0\0") +
                           chunk(STRING, b"\0\0\0\0") + chunk(EXTENSION, b"")) + chunk(EXTENSION, b""))
    matlist = chunk(MATLIST, chunk(STRUCT, struct.pack("<Ii", 1, -1)) + material)
    geometry = chunk(GEOMETRY, chunk(STRUCT, geom_struct) + matlist + chunk(EXTENSION, b""))
    geomlist = chunk(GEOMLIST, chunk(STRUCT, struct.pack("<I", 1)) + geometry)
    atomic = chunk(ATOMIC, chunk(STRUCT, struct.pack("<4I", 1, 0, 5, 0)) + chunk(EXTENSION, b""))
    return chunk(CLUMP, chunk(STRUCT, struct.pack("<3I", 1, 0, 0)) + framelist + geomlist + atomic + chunk(EXTENSION, b""))


def write_txd():
    body = struct.pack("<II32s32sII2H4B", 8, 0x1106, b"admiral868bit", b"", 0x0100, 0, 4, 4, 16, 1, 4, 1)
    body += struct.pack("<I", 8) + b"\0" * 8  # one DXT1 4x4 level
    return chunk(TEXDICT, chunk(STRUCT, struct.pack("<2H", 1, 0)) + chunk(TEXNATIVE, chunk(STRUCT, body) + chunk(EXTENSION, b"")))


def self_test():
    import toolchain as tc
    out = os.path.join(tc.ROOT, "build", "rwinfo")
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, "test.dff"), "wb").write(write_dff())
    open(os.path.join(out, "test.txd"), "wb").write(write_txd())
    d = read_dff(open(os.path.join(out, "test.dff"), "rb").read())
    t = read_txd(open(os.path.join(out, "test.txd"), "rb").read())
    assert [f["name"] for f in d["frames"]] == ["chassis_dummy", "wheel_lf_dummy"], d["frames"]
    assert d["frames"][1]["parent"] == 0 and d["version"] == "34003", d
    assert d["geometries"][0]["vertices"] == 3 and d["geometries"][0]["triangles"] == 1, d["geometries"]
    assert d["geometries"][0]["materials"][0]["texture"] == "admiral868bit", d["geometries"]
    assert d["atomics"] == [{"frame": 1, "geometry": 0}], d["atomics"]
    assert t["textures"] == [{"name": "admiral868bit", "width": 4, "height": 4, "format": "DXT1", "levels": 1, "platform": 8}], t
    print("rwinfo self-test passed (%s)" % out)


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for path in sys.argv[1:]:
        data = open(path, "rb").read()
        info = read_txd(data) if path.lower().endswith(".txd") else read_dff(data)
        print("==", path)
        for f in info.get("frames", []):
            print("frame  %-24s parent %d" % (f["name"], f["parent"]))
        for i, g in enumerate(info.get("geometries", [])):
            print("geometry %d: %d vertices, %d triangles%s, textures %s" % (i, g["vertices"], g["triangles"],
                  " (PS2 native)" if g["native"] else "", sorted({m["texture"] for m in g["materials"] if m["texture"]})))
        for a in info.get("atomics", []):
            print("atomic geometry %d on frame %d (%s)" % (a["geometry"], a["frame"], info["frames"][a["frame"]]["name"]))
        for t in info.get("textures", []):
            print("texture %-24s %4dx%-4d %-10s %d levels" % (t["name"], t["width"], t["height"], t["format"], t["levels"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

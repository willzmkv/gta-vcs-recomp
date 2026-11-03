#!/usr/bin/env python3
"""A VCS model (`.mdl`: an RSL `ldm` resource, docs/game-internals.md 9d) read on the host - FORK.md T9 step 1.

    python tools/mdlinfo.py FILE.mdl [--obj OUT.obj]   the node tree (names, positions), the elements on it, each
                                                        geometry's materials (texture names) and PS2 meshes decoded
                                                        into triangles (checked against the counts the file states);
                                                        --obj writes the decoded geometry for a 3D viewer
    python tools/mdlinfo.py --vehicles [--img ...]      every named vehicle of build/img/GTA3PS2 (tools/img.py extract)

The resource: a 0x20-byte chunk header {ident 'ldm\\0', shrink, file size, data size, relocation table, relocation
count, global table, class count u16, function count u16}; pointers inside are offsets from the file's start (the
relocation table lists where they are; the game adds the load address). A vehicle's data starts at 0x20:
{element group, extra count, extras, primary materials[], secondary materials[]}. RSL objects (aap's description of
the formats, github.com/aap/librwgta tools/storiesconv - read as a reference, nothing copied): object {type u8,
subtype u8, flags u8, private u8, parent}; element group (type 2) {object, element list}; node {object, object list,
modelling matrix (0x10), LTM (0x50), child 0x90, next 0x94, root 0x98, id, ?, hierarchy, name 0xA8, hierarchy id};
element (type 1) {object, node link, sync, geometry 0x14, group 0x18, group link 0x1C, render callback, model id, ...};
geometry {object, ref count, materials {list 0x0C, count 0x10}, skin, pad} then the PS2 header (bound, size: meshes
<< 20, flags, vertex count, DMA offset, box, scale 0x28, position 0x34) and 0x30-byte meshes (bound, UV scale, ?, DMA
packet offset, triangle count, material) followed by the DMA data: per batch UNPACKs - V3-16 positions, V2-8 UVs,
V4-5 colours, V3-8 normals - and an MSCAL; batches of a strip share two vertices.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MDL_IDENT = 0x006D646C  # 'ldm\0' read little-endian


class Mdl:
    def __init__(self, data):
        self.d = data
        (self.ident, self.shrink, self.file_size, self.data_size, self.reloc_tab, self.num_relocs, self.global_tab,
         self.num_classes, self.num_funcs) = struct.unpack_from("<7I2H", data, 0)
        if self.ident != MDL_IDENT:
            raise ValueError("not an ldm resource (ident %08x)" % self.ident)
        self.relocs = set(struct.unpack_from("<%dI" % self.num_relocs, data, self.reloc_tab))

    def u32(self, at):
        return struct.unpack_from("<I", self.d, at)[0]

    def ptr(self, at):
        """A pointer field: its target offset, or 0 when the field is not in the relocation table (null)."""
        return self.u32(at) if at in self.relocs else 0

    def cstr(self, at):
        return self.d[at:self.d.index(b"\0", at)].decode("latin1") if at else ""

    # ---- nodes ----------------------------------------------------------------------------------------------------
    def node(self, at, depth=0, out=None):
        out = [] if out is None else out
        name = self.cstr(self.ptr(at + 0xA8))
        pos = struct.unpack_from("<3f", self.d, at + 0x10 + 48)
        out.append({"at": at, "name": name, "depth": depth, "pos": pos})
        child = self.ptr(at + 0x90)
        while child:
            self.node(child, depth + 1, out)
            child = self.ptr(child + 0x94)
        return out

    # ---- elements and geometry ---------------------------------------------------------------------------------------
    def elements(self, group):
        """The group's elements: its list's links sit at +0x1C inside each element."""
        out, head = [], group + 8
        link = self.ptr(head)
        seen = set()
        while link and link != head and link not in seen:
            seen.add(link)
            out.append(link - 0x1C)
            link = self.ptr(link)
        return out

    def materials(self, geom):
        mats, n = self.ptr(geom + 0x0C), self.u32(geom + 0x10)
        out = []
        for i in range(n):
            m = self.ptr(mats + i * 4) if mats else 0
            if not m:
                out.append(None)
                continue
            rgba = struct.unpack_from("<4B", self.d, m + 4)
            out.append({"texture": self.cstr(self.ptr(m)), "colour": "%02x%02x%02x%02x" % rgba, "matfx": bool(self.ptr(m + 12))})
        return out

    def geometry(self, geom):
        h = geom + 32
        size, flags, num_verts, dma_offset = struct.unpack_from("<IiHH", self.d, h + 16)
        scale = struct.unpack_from("<3f", self.d, h + 0x28)
        pos = struct.unpack_from("<3f", self.d, h + 0x34)
        n = size >> 20
        meshes_at = h + 0x40
        dma_base = meshes_at + n * 0x30
        meshes = []
        for i in range(n):
            m = meshes_at + i * 0x30
            uv_scale = struct.unpack_from("<2f", self.d, m + 16)
            packet, tris, mat = struct.unpack_from("<IHh", self.d, m + 28)
            verts, faces, kinds = self.decode(dma_base + packet, scale, pos, uv_scale)
            meshes.append({"material": mat, "triangles": tris, "uv_scale": uv_scale, "decoded_vertices": len(verts), "decoded_triangles": len(faces),
                           "attributes": kinds, "verts": verts, "faces": faces})
        return {"num_verts": num_verts, "flags": flags, "scale": scale, "pos": pos, "meshes": meshes,
                "materials": self.materials(geom)}

    def decode(self, at, scale, pos, uv_scale):
        """One mesh's DMA data: batches of UNPACKs ended by MSCAL; returns its vertices (x, y, z, u, v) and the strip's
        triangles (two consecutive batches share two vertices)."""
        d = self.d
        qwords = (struct.unpack_from("<I", d, at)[0] & 0xFFFF) + 1
        end, w = at + qwords * 16, at + 16
        verts, faces, kinds = [], [], set()
        first = True
        while w < end:
            code = struct.unpack_from("<I", d, w)[0]
            if code != 0x6C018000:  # the batch's UNPACK of its strip header
                break
            nvert = struct.unpack_from("<I", d, w + 16)[0] & 0x7FFF
            w += 20
            batch = {}
            while True:
                code = struct.unpack_from("<I", d, w)[0]
                cmd = code >> 24
                if cmd == 0x14:  # MSCAL: the batch ends
                    w += 4
                    while w < end and struct.unpack_from("<I", d, w)[0] == 0:
                        w += 4
                    break
                if cmd == 0x20:  # STMASK + mask
                    w += 8
                    continue
                if cmd == 0x30:  # STROW + 4 words
                    w += 20
                    continue
                if cmd & 0x60 != 0x60:  # not an UNPACK: stop
                    w = end
                    break
                vnvl = cmd & 0x0F
                num = (code >> 16) & 0xFF
                size = {0x9: 6, 0x6: 2, 0xF: 2, 0xA: 3, 0xC: 16}.get(vnvl)  # V3-16, V2-8, V4-5, V3-8, V4-32
                if size is None:
                    w = end
                    break
                batch[vnvl] = w + 4
                kinds.add({0x9: "position", 0x6: "uv", 0xF: "colour", 0xA: "normal", 0xC: "skin"}[vnvl])
                w += 4 + (num * size + 3) // 4 * 4
            skip = 0 if first else 2  # a later batch repeats the strip's last two vertices
            p, t = batch.get(0x9), batch.get(0x6)
            base = len(verts)
            for i in range(skip, nvert):
                x, y, z = struct.unpack_from("<3h", d, p + i * 6) if p else (0, 0, 0)
                u, v = struct.unpack_from("<2B", d, t + i * 2) if t else (0, 0)
                verts.append((x / 32767.5 * scale[0] + pos[0], y / 32767.5 * scale[1] + pos[1], z / 32767.5 * scale[2] + pos[2],
                              u / 127.5 * uv_scale[0], v / 127.5 * uv_scale[1]))
            # strip -> triangles over this batch's vertices (with the two carried over)
            strip = list(range(base - (0 if first else 2), len(verts)))
            for k in range(2, len(strip)):
                a, b, c = strip[k - 2], strip[k - 1], strip[k]
                if a != b and b != c and a != c:
                    faces.append((a, b, c) if k % 2 == 0 else (b, a, c))
            first = False
        return verts, faces, sorted(kinds)


def vehicle(m):
    group = m.ptr(0x20)
    if not group or m.d[group] != 2:
        raise ValueError("no element group at the vehicle data (0x20)")
    root = m.ptr(group + 4)
    nodes = m.node(root) if root else []
    by_at = {n["at"]: n["name"] for n in nodes}
    elements = []
    for e in m.elements(group):
        geom = m.ptr(e + 0x14)
        elements.append({"node": by_at.get(m.ptr(e + 4), "?"), "geometry": m.geometry(geom) if geom else None})
    return {"nodes": nodes, "elements": elements, "extras": m.u32(0x24)}


def report(path, obj=None):
    m = Mdl(open(path, "rb").read())
    v = vehicle(m)
    print("== %s: ldm, file %d bytes, data %d, %d relocations, %d nodes, %d elements" % (
        path, m.file_size, m.data_size, m.num_relocs, len(v["nodes"]), len(v["elements"])))
    for n in v["nodes"]:
        print("%s%-24s (%.2f %.2f %.2f)" % ("  " * n["depth"], n["name"] or "-", *n["pos"]))
    ok = True
    lines, base = [], 1
    for e in v["elements"]:
        g = e["geometry"]
        if not g:
            continue
        stated = sum(mm["triangles"] for mm in g["meshes"])
        decoded = sum(mm["decoded_triangles"] for mm in g["meshes"])
        tex = sorted({mt["texture"] for mt in g["materials"] if mt and mt["texture"]})
        print("element on %-20s %d meshes, %d vertices, triangles %d stated / %d decoded, %s, textures %s" % (
            e["node"], len(g["meshes"]), g["num_verts"], stated, decoded,
            "+".join(sorted({k for mm in g["meshes"] for k in mm["attributes"]})), tex))
        ok &= decoded > 0
        if obj:
            lines.append("o %s" % e["node"])
            for mm in g["meshes"]:
                for x, y, z, u, vv in mm["verts"]:
                    lines.append("v %.5f %.5f %.5f" % (x, y, z))
                for a, b, c in mm["faces"]:
                    lines.append("f %d %d %d" % (a + base, b + base, c + base))
                base += len(mm["verts"])
    if obj:
        open(obj, "w").write("\n".join(lines) + "\n")
        print("geometry -> %s" % obj)
    return ok


def wireframe_png(path, elements, out, size=400):
    """The decoded triangles as wireframes: a side view (y, z) above a top view (y, x) - a look at the decode."""
    import zlib
    tris = []
    for e in elements:
        g = e["geometry"]
        if g and not e["node"].endswith("vlo"):
            for mm in g["meshes"]:
                tris += [(mm["verts"][a], mm["verts"][b], mm["verts"][c]) for a, b, c in mm["faces"]]
    if not tris:
        return
    ext = max(max(abs(c) for v in t for c in v[:3]) for t in tris) or 1.0
    w, h = size, size
    img = bytearray(w * h * 2)

    def line(view, x0, y0, x1, y1):
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(n + 1):
            x, y = int(x0 + (x1 - x0) * i / n), int(y0 + (y1 - y0) * i / n)
            if 0 <= x < w and 0 <= y < h:
                img[(view * h + y) * w + x] = 255

    for view, (ax, ay) in enumerate(((1, 2), (1, 0))):  # side: y across, z up; top: y across, x down
        def p(v):
            return (v[ax] / ext * 0.48 + 0.5) * w, (0.5 - v[ay] / ext * 0.48) * h
        for t in tris:
            a, b, c = p(t[0]), p(t[1]), p(t[2])
            line(view, *a, *b)
            line(view, *b, *c)
            line(view, *c, *a)
    raw = b"".join(b"\0" + bytes(img[y * w:(y + 1) * w]) for y in range(2 * h))
    chunk = lambda k, d: struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d))
    open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, 2 * h, 8, 0, 0, 0, 0)) +
                          chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
    print("wireframe -> %s (%d triangles)" % (out, len(tris)))


def main():
    args = sys.argv[1:]
    if "--png" in args:
        i = args.index("--png")
        out = args[i + 1]
        del args[i:i + 2]
        wireframe_png(args[0], vehicle(Mdl(open(args[0], "rb").read()))["elements"], out)
        args = [a for a in args]
    if not args:
        print(__doc__)
        return 1
    if args[0] == "--vehicles":
        import toolchain as tc
        folder = args[args.index("--img") + 1] if "--img" in args else os.path.join(tc.ROOT, "build", "img", "GTA3PS2")
        bad = 0
        for name in sorted(os.listdir(folder)):
            if name.endswith(".mdl"):
                try:
                    v = vehicle(Mdl(open(os.path.join(folder, name), "rb").read()))
                    tris = sum(mm["decoded_triangles"] for e in v["elements"] if e["geometry"] for mm in e["geometry"]["meshes"])
                    print("%-14s %2d nodes %2d elements %5d triangles" % (name, len(v["nodes"]), len(v["elements"]), tris))
                except Exception as e:
                    bad += 1
                    print("%-14s FAILED: %s" % (name, e))
        return 1 if bad else 0
    obj = args[args.index("--obj") + 1] if "--obj" in args else None
    return 0 if report(args[0], obj) else 1


if __name__ == "__main__":
    sys.exit(main())

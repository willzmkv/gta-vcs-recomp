#!/usr/bin/env python3
"""PC VC texture dictionary (.txd) -> VCS texture dictionary (`.xtx`, the game's `tex` resource), FORK.md T9.

    python tools/txd2xtx.py convert IN.txd OUT.xtx [--max 128] [--only name,name] [--keep VCS.xtx]
    python tools/txd2xtx.py info FILE.xtx [--png DIR]      a VCS texture dictionary: its textures (PNG each with --png)
    python tools/txd2xtx.py --self-test

A VCS texture dictionary (docs/game-internals.md 9d; GTA3PS2.IMG entry 7534 + the TXD's slot) is a relocatable
resource like a model: a 0x20 header {ident 'tex\\0' (0x00746578), shrink, fileSize, dataSize, relocTab, numRelocs,
globalTab, u16 classes, u16 functions}, pointers as file offsets listed in the relocation table. At 0x20 the RslTexList
{object {type 6, ...}, texturesInDict {next, prev}, lInInstance}; each texture {raster, dict, lInDictionary {next, prev},
name[32], mask[32]}; each raster {0, upload word, texels, flags}: flags = log2 width | log2 height << 6 | depth << 12 |
levels << 20 | swizzle << 24; the upload word 0x450000 | row bytes (4 bit) or 0x250000 | row bytes (8 bit). The texels
(linear, not swizzled: every texture on the disc), then the palette: RGBA with alpha 0..0x80; an 8-bit palette in the
GS's CSM1 order (index bits 3 and 4 swapped). The disc's textures are all 4 or 8 bit, one level, 8..256 pixels.

A PC texture (D3D8 native: DXT1/3/5, 8-bit palettes, 8888 / 888 / 1555 / 565 / 4444 / LUM8) is decoded, its top level
halved down to --max, and quantised (median cut) to 16 colours when it has at most 16, else 256. --keep: the VCS
dictionary this one replaces - its textures the .txd has no name for follow, copied as they are (parts of the VCS model
the conversion keeps use them: a helicopter's rotor blur).
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rwinfo as rw  # noqa: E402

TEX_IDENT = 0x00746578


# ---- PC textures -------------------------------------------------------------------------------------------------
def _565(c):
    r, g, b = c >> 11 & 31, c >> 5 & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def _dxt_colours(block, at, four):
    c0, c1, bits = struct.unpack_from("<2HI", block, at)
    a, b = _565(c0), _565(c1)
    if c0 > c1 or four:
        cols = [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
    else:
        cols = [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]
    return [cols[bits >> (2 * i) & 3] for i in range(16)]


def decode_dxt(data, w, h, kind):
    """DXT1 / 3 / 5 -> RGBA rows (lists of (r, g, b, a))."""
    out = [[(0, 0, 0, 0)] * w for _ in range(h)]
    size = 8 if kind == 1 else 16
    at = 0
    for by in range(0, max(h, 4), 4):
        for bx in range(0, max(w, 4), 4):
            if kind == 1:
                px = _dxt_colours(data, at, False)
            else:
                px = _dxt_colours(data, at + 8, True)
                if kind == 3:
                    alpha = int.from_bytes(data[at:at + 8], "little")
                    al = [(alpha >> (4 * i) & 15) * 17 for i in range(16)]
                else:
                    a0, a1 = data[at], data[at + 1]
                    bits = int.from_bytes(data[at + 2:at + 8], "little")
                    if a0 > a1:
                        table = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
                    else:
                        table = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
                    al = [table[bits >> (3 * i) & 7] for i in range(16)]
                px = [p[:3] + (a,) for p, a in zip(px, al)]
            for i in range(16):
                x, y = bx + (i & 3), by + (i >> 2)
                if x < w and y < h:
                    out[y][x] = px[i]
            at += size
    return out


def read_pc_txd(data):
    """[{name, width, height, rows (RGBA)}] of a PC (D3D8, VC) texture dictionary: each texture's top level."""
    top = next(rw.chunks(data, 0, len(data)))
    if top[0] != rw.TEXDICT:
        raise ValueError("not a texture dictionary (chunk 0x%x)" % top[0])
    out = []
    for kind, a, e, _ in rw.chunks(data, top[1], top[2]):
        if kind != rw.TEXNATIVE:
            continue
        s = rw.child(data, a, e, rw.STRUCT)
        at = s[0]
        platform = struct.unpack_from("<I", data, at)[0]
        name = data[at + 8:at + 40].split(b"\0")[0].decode("latin1")
        raster, _alpha = struct.unpack_from("<2I", data, at + 72)
        w, h, depth, levels, _type, compression = struct.unpack_from("<2H4B", data, at + 80)
        if platform != 8:
            raise ValueError("%s: platform %d (only D3D8, VC PC's)" % (name, platform))
        at += 88
        palette = None
        if raster & 0x2000:
            palette = [tuple(data[at + 4 * i:at + 4 * i + 4]) for i in range(256)]
            at += 1024
        elif raster & 0x4000:
            palette = [tuple(data[at + 4 * i:at + 4 * i + 4]) for i in range(32)]
            at += 128
        size = struct.unpack_from("<I", data, at)[0]
        px = data[at + 4:at + 4 + size]
        fmt = raster & 0xF00
        if compression:
            rows = decode_dxt(px, w, h, compression)
        elif palette:
            if size >= w * h:
                idx = px
            else:
                idx = bytes(v for b in px for v in (b & 15, b >> 4))
            rows = [[palette[idx[y * w + x]] for x in range(w)] for y in range(h)]
            if fmt == 0x600:  # 888: no alpha
                rows = [[p[:3] + (255,) for p in r] for r in rows]
        elif fmt in (0x500, 0x600):
            rows = [[(px[(y * w + x) * 4 + 2], px[(y * w + x) * 4 + 1], px[(y * w + x) * 4],
                      px[(y * w + x) * 4 + 3] if fmt == 0x500 else 255) for x in range(w)] for y in range(h)]
        elif fmt in (0x100, 0x200, 0x300, 0xA00):
            rows = []
            for y in range(h):
                row = []
                for x in range(w):
                    c = struct.unpack_from("<H", px, (y * w + x) * 2)[0]
                    if fmt == 0x200:
                        row.append(_565(c) + (255,))
                    elif fmt == 0x300:
                        row.append(tuple((c >> s & 15) * 17 for s in (8, 4, 0, 12)))
                    else:
                        r, g, b = c >> 10 & 31, c >> 5 & 31, c & 31
                        row.append((r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255 if (c >> 15 or fmt == 0xA00) else 0))
                rows.append(row)
        elif fmt == 0x400:
            rows = [[(px[y * w + x],) * 3 + (255,) for x in range(w)] for y in range(h)]
        else:
            raise ValueError("%s: raster format 0x%x not handled" % (name, raster))
        out.append({"name": name, "width": w, "height": h, "rows": rows})
    return out


def halve(rows):
    h, w = len(rows), len(rows[0])
    if w == 1 and h == 1:
        return rows
    sx, sy = (2 if w > 1 else 1), (2 if h > 1 else 1)
    out = []
    for y in range(0, h, sy):
        row = []
        for x in range(0, w, sx):
            px = [rows[y + j][x + i] for j in range(sy) for i in range(sx)]
            row.append(tuple((sum(p[k] for p in px) + len(px) // 2) // len(px) for k in range(4)))
        out.append(row)
    return out


def quantise(rows, colours):
    """Median cut on RGBA: (palette, index rows)."""
    counts = {}
    for row in rows:
        for p in row:
            counts[p] = counts.get(p, 0) + 1
    if len(counts) <= colours:
        palette = sorted(counts)
        where = {p: i for i, p in enumerate(palette)}
        return palette, [[where[p] for p in row] for row in rows]
    boxes = [list(counts)]
    while len(boxes) < colours:
        best, best_range, best_k = None, -1, 0
        for i, b in enumerate(boxes):
            if len(b) < 2:
                continue
            for k in range(4):
                lo, hi = min(p[k] for p in b), max(p[k] for p in b)
                if hi - lo > best_range:
                    best, best_range, best_k = i, hi - lo, k
        if best is None or best_range <= 0:
            break
        b = sorted(boxes[best], key=lambda p: p[best_k])
        total, run, cut = sum(counts[p] for p in b), 0, 1
        for j, p in enumerate(b):
            run += counts[p]
            if run * 2 >= total:
                cut = min(max(j + 1, 1), len(b) - 1)
                break
        boxes[best:best + 1] = [b[:cut], b[cut:]]
    palette, where = [], {}
    for i, b in enumerate(boxes):
        n = sum(counts[p] for p in b)
        palette.append(tuple((sum(p[k] * counts[p] for p in b) + n // 2) // n for k in range(4)))
        for p in b:
            where[p] = i
    return palette, [[where[p] for p in row] for row in rows]


# ---- the VCS texture dictionary ------------------------------------------------------------------------------------
def _csm1(i):
    return (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


def write_xtx(textures):
    """textures: [{name, width, height, depth (4 / 8), palette [(r, g, b, a 0..255)], indices rows}] -> bytes."""
    data = bytearray(0x40)
    relocs = []

    def ptr(at, value):
        struct.pack_into("<I", data, at, value)
        relocs.append(at)

    struct.pack_into("<I", data, 0x20, 6)  # the RslTexList: object type 6, no parent; its list filled below
    texel_at = []
    for t in textures:
        data += b"\0" * (-len(data) % 16)
        texel_at.append(len(data))
        w, h, d = t["width"], t["height"], t["depth"]
        if "raw" in t:  # a VCS texture kept as it is: texels and palette (alpha 0..0x80, CSM1 order) unchanged
            data += t["raw"]
            continue
        if d == 8:
            data += bytes(i for row in t["indices"] for i in row)
            pal = t["palette"] + [(0, 0, 0, 0)] * (256 - len(t["palette"]))
            pal = [pal[_csm1(i)] for i in range(256)]
        else:
            data += bytes(row[x] | row[x + 1] << 4 for row in t["indices"] for x in range(0, w, 2))
            pal = t["palette"] + [(0, 0, 0, 0)] * (16 - len(t["palette"]))
        data += b"".join(struct.pack("<4B", r, g, b, (a * 128 + 127) // 255) for r, g, b, a in pal)
    data += b"\0" * (-len(data) % 16)
    tex_at = [len(data) + 0x60 * i for i in range(len(textures))]  # a texture (0x50) and its raster (0x10)
    data += b"\0" * (0x60 * len(textures))
    links = [0x28] + [a + 8 for a in tex_at] + [0x28]
    ptr(0x28, links[1] if textures else 0x28)
    ptr(0x2C, links[-2] if textures else 0x28)
    for i, (t, at) in enumerate(zip(textures, tex_at)):
        w, h, d = t["width"], t["height"], t["depth"]
        ras = at + 0x50
        ptr(at, ras)
        ptr(at + 4, 0x20)
        ptr(at + 8, links[i + 2])
        ptr(at + 12, links[i])
        data[at + 16:at + 48] = t["name"].encode("latin1")[:31].ljust(32, b"\0")
        flags = (w.bit_length() - 1) | (h.bit_length() - 1) << 6 | d << 12 | 1 << 20
        struct.pack_into("<2I", data, ras, 0, (0x450000 if d == 4 else 0x250000) | (w * d // 8))
        ptr(ras + 8, texel_at[i])
        struct.pack_into("<I", data, ras + 12, flags)
    data_size = len(data)
    relocs.sort()
    data += struct.pack("<%dI" % len(relocs), *relocs)
    struct.pack_into("<7I2H", data, 0, TEX_IDENT, 0, len(data), data_size, data_size, len(relocs), 0, 0, 0)
    return bytes(data)


def read_xtx_raw(data):
    """A VCS texture dictionary's textures as stored: [{name, width, height, depth, raw (texels + palette)}]."""
    ident = struct.unpack_from("<I", data, 0)[0]
    if ident != TEX_IDENT:
        raise ValueError("not a VCS texture dictionary (ident 0x%08x)" % ident)
    u = lambda a: struct.unpack_from("<I", data, a)[0]  # noqa: E731
    out, link, guard = [], u(0x28), 0
    while link and link != 0x28 and guard < 4096:
        tex = link - 8
        ras = u(tex)
        _unk, _upload, texels, flags = struct.unpack_from("<4I", data, ras)
        w, h, d = 1 << (flags & 63), 1 << (flags >> 6 & 63), flags >> 12 & 63
        if d not in (4, 8):
            raise ValueError("depth %d not handled" % d)
        size = w * h * d // 8 + (16 if d == 4 else 256) * 4
        out.append({"name": data[tex + 16:tex + 48].split(b"\0")[0].decode("latin1"), "width": w, "height": h,
                    "depth": d, "raw": bytes(data[texels:texels + size])})
        link = u(link)
        guard += 1
    return out


def read_xtx(data):
    """A VCS texture dictionary -> [{name, width, height, depth, rows (RGBA, alpha 0..255)}]."""
    ident, _shrink, _fsize, _dsize, _rtab, _nrel = struct.unpack_from("<6I", data, 0)
    if ident != TEX_IDENT:
        raise ValueError("not a VCS texture dictionary (ident 0x%08x)" % ident)
    u = lambda a: struct.unpack_from("<I", data, a)[0]  # noqa: E731
    out, link, guard = [], u(0x28), 0
    while link and link != 0x28 and guard < 4096:
        tex = link - 8
        ras = u(tex)
        _unk, _upload, texels, flags = struct.unpack_from("<4I", data, ras)
        w, h, d = 1 << (flags & 63), 1 << (flags >> 6 & 63), flags >> 12 & 63
        if d == 4:
            idx = [v for b in data[texels:texels + w * h // 2] for v in (b & 15, b >> 4)]
            pal = [tuple(data[texels + w * h // 2 + 4 * i:texels + w * h // 2 + 4 * i + 4]) for i in range(16)]
        elif d == 8:
            idx = list(data[texels:texels + w * h])
            raw = [tuple(data[texels + w * h + 4 * i:texels + w * h + 4 * i + 4]) for i in range(256)]
            pal = [raw[_csm1(i)] for i in range(256)]
        else:
            raise ValueError("depth %d not handled" % d)
        pal = [p[:3] + (min(255, p[3] * 255 // 128),) for p in pal]
        rows = [[pal[idx[y * w + x]] for x in range(w)] for y in range(h)]
        out.append({"name": data[tex + 16:tex + 48].split(b"\0")[0].decode("latin1"), "width": w, "height": h,
                    "depth": d, "rows": rows})
        link = u(link)
        guard += 1
    return out


def png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def convert(txd_path, out_path, max_size=128, only=None, keep=None):
    textures = []
    for t in read_pc_txd(open(txd_path, "rb").read()):
        if only is not None and t["name"].lower() not in only:
            continue
        rows = t["rows"]
        while len(rows[0]) > max_size or len(rows) > max_size:
            rows = halve(rows)
        depth = 4 if len({p for row in rows for p in row}) <= 16 else 8  # 4 bit only when nothing is lost
        palette, indices = quantise(rows, 1 << depth)
        textures.append({"name": t["name"], "width": len(rows[0]), "height": len(rows), "depth": depth,
                         "palette": palette, "indices": indices})
    if keep:  # the VCS dictionary's own textures the .txd has no name for, after them
        have = {t["name"].lower() for t in textures}
        textures += [t for t in read_xtx_raw(open(keep, "rb").read()) if t["name"].lower() not in have]
    data = write_xtx(textures)
    with open(out_path, "wb") as f:
        f.write(data)
    print("%s: %d textures (%s) -> %s (%d bytes)" % (os.path.basename(txd_path), len(textures), ", ".join(
        "%s %dx%d %d bit" % (t["name"], t["width"], t["height"], t["depth"]) for t in textures), out_path, len(data)))
    return out_path


def self_test():
    """Two textures written and read back: sizes, depths, colours (alpha within the PS2's 0..128 rounding)."""
    a = [[(x * 16, y * 16, 0, 255) for x in range(16)] for y in range(8)]  # 128 colours: 8 bit
    b = [[(255, 0, 0, 255) if (x + y) & 1 else (0, 0, 255, 128) for x in range(8)] for y in range(8)]  # 2: 4 bit
    texs = []
    for name, rows in (("grad", a), ("check", b)):
        depth = 4 if len({p for row in rows for p in row}) <= 16 else 8
        palette, indices = quantise(rows, 1 << depth)
        texs.append({"name": name, "width": len(rows[0]), "height": len(rows), "depth": depth, "palette": palette,
                     "indices": indices})
    back = read_xtx(write_xtx(texs))
    ok = [t["name"] for t in back] == ["grad", "check"] and [t["depth"] for t in back] == [8, 4]
    for src, t in zip((a, b), back):
        for y, row in enumerate(src):
            for x, p in enumerate(row):
                q = t["rows"][y][x]
                ok = ok and q[:3] == p[:3] and abs(q[3] - p[3]) <= 2
    print("txd2xtx self-test:", "ok" if ok else "FAILED")
    return ok


def main():
    args = sys.argv[1:]
    if args[:1] == ["--self-test"]:
        return 0 if self_test() else 1
    if len(args) >= 3 and args[0] == "convert":
        max_size = int(args[args.index("--max") + 1]) if "--max" in args else 128
        only = set(args[args.index("--only") + 1].lower().split(",")) if "--only" in args else None
        keep = args[args.index("--keep") + 1] if "--keep" in args else None
        convert(args[1], args[2], max_size, only, keep)
        return 0
    if len(args) >= 2 and args[0] == "info":
        texs = read_xtx(open(args[1], "rb").read())
        out = args[args.index("--png") + 1] if "--png" in args else None
        for t in texs:
            print("%-32s %4dx%-4d %d bit" % (t["name"], t["width"], t["height"], t["depth"]))
            if out:
                os.makedirs(out, exist_ok=True)
                png(os.path.join(out, t["name"] + ".png"), t["rows"])
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main())

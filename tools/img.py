#!/usr/bin/env python3
"""GTA3PS2.IMG, VCS's model archive, entry by entry (docs/MODS.md "Game files", docs/game-internals.md 9d).

    python tools/img.py table [--save DIR]          the game's directory of the archive, read from a hidden run
    python tools/img.py list [names...]             entries: index, offset / size in sectors, name when known
    python tools/img.py extract [OUT] [names...]    entries out of your disc image (default OUT: build/img/GTA3PS2):
                                                    models as <name>.mdl, texture dictionaries as <name>.xtx

The archive has no directory on the disc: the game keeps one in GAME.DTZ - the streaming info, one 0x18-byte record per
resource (+0x10 offset, +0x14 size, in sectors). `table` reads it from the running game into build/img/streaming.json;
the model infos give each model's key (~CRC32 of its upper-case name), and a vehicle's text key names it when its key
matches (`admiral`). Names: a known name, `#<index>`, or wildcards over the known names (`a*`).

Extracted entries are the game's data: they stay on this PC (build/ or a mod folder of yours), never in git. A file
named like an entry in a mod folder (`modloader/<mod>/admiral.mdl`, `#1234.bin`) replaces that entry in the game.
"""
import argparse
import fnmatch
import json
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import toolchain as tc  # noqa: E402

TABLE = os.path.join(tc.ROOT, "build", "img", "streaming.json")
RESOURCES = 8482


def key_of(name):
    return zlib.crc32(name.upper().encode("latin1")) ^ 0xFFFFFFFF


def iso_path(given):
    if given:
        return given
    for ini in (os.path.join(tc.BUILD, "vcs.ini"), os.path.join(tc.ROOT, "build", "vcs.ini")):
        if os.path.isfile(ini):
            for line in open(ini, encoding="utf-8", errors="replace"):
                if line.startswith("iso="):
                    return line[4:].strip().strip('"')
    raise SystemExit("no disc image: --iso PATH (or iso= in vcs.ini)")


def table(save):
    import rig
    g = rig.Game.start(save=save, name="img_table", seconds=300)
    try:
        g.wait_ingame(timeout=180) if save else time_wait(g)
        count = g.u32(0x486E8C)
        files = b"".join(g.peek(0x5038A8 + i, 4096) for i in range(0, count * 16, 4096))[:count * 16]
        img = None
        for i in range(count):
            name_ptr, _, lsn, size = struct.unpack_from("<4I", files, i * 16)
            if b"GTA3PS2.IMG" in g.peek(name_ptr, 40):
                img = {"lsn": lsn, "bytes": size}
        base = g.u32(g.u32(0x489FF8) + 200)
        raw = b"".join(g.peek(base + i, 4096) for i in range(0, RESOURCES * 0x18, 4096))
        infos = b"".join(g.peek(g.u32(0x486518) + i, 4096) for i in range(0, RESOURCES * 4, 4096))
        entries = []
        for i in range(RESOURCES):
            offset, sectors = struct.unpack_from("<2I", raw, i * 0x18 + 0x10)
            if offset == 0xFFFFFFFF or not sectors:
                continue
            entry = {"index": i, "offset": offset, "sectors": sectors}
            info = struct.unpack_from("<I", infos, i * 4)[0]
            if 0x100000 <= info < 0x2000000:
                key = g.u32(info + 8)
                entry["key"] = "%08x" % key
                if 171 <= i <= 280:  # vehicles: their text key is usually the model's name
                    text = g.peek(info + 0x158, 8).split(b"\0")[0].decode("latin1")
                    if text and key_of(text) == key:
                        entry["name"] = text.lower()
            entries.append(entry)
        # texture dictionaries: slot i is entry *(streaming owner + 8) + i, named in the TXD store's pool
        # (*(0x489ECC) = {items, flags, size}, 0x1C an item {list, refs, name[20]}; game-internals.md 9d)
        txd_base, pool = g.u32(g.u32(0x489FF8) + 8), g.u32(0x489ECC)
        items, size = g.u32(pool), g.u32(pool + 8)
        raw_items = b"".join(g.peek(items + i, 4096) for i in range(0, size * 0x1C, 4096))
        by_index = {e["index"]: e for e in entries}
        for slot in range(min(size, RESOURCES)):
            name = raw_items[slot * 0x1C + 8:slot * 0x1C + 28].split(b"\0")[0].decode("latin1")
            e = by_index.get(txd_base + slot)
            if e is not None and name:
                e["name"], e["kind"] = name.lower(), "txd"
    finally:
        g.quit()
    os.makedirs(os.path.dirname(TABLE), exist_ok=True)
    json.dump({"img": img, "entries": entries}, open(TABLE, "w"), indent=0)
    print("GTA3PS2.IMG at LSN %d, %d entries (%d named) -> %s" % (img["lsn"], len(entries),
                                                               sum("name" in e for e in entries), TABLE))


def time_wait(g):
    import time
    time.sleep(20)


def load_table():
    if not os.path.isfile(TABLE):
        raise SystemExit("no table yet: python tools/img.py table")
    return json.load(open(TABLE))


def pick(entries, names):
    if not names:
        return entries
    out = []
    for e in entries:
        label = e.get("name", "")
        if any(n == "#%d" % e["index"] or (label and fnmatch.fnmatch(label, n.lower())) for n in names):
            out.append(e)
    return out


def file_name(e):
    if "name" not in e:
        return "#%d.bin" % e["index"]
    return e["name"] + (".xtx" if e.get("kind") == "txd" else ".mdl")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("table")
    t.add_argument("--save", help="a card folder to start from (as rig.py start --save); default: an empty card")
    ls = sub.add_parser("list")
    ls.add_argument("names", nargs="*")
    ex = sub.add_parser("extract")
    ex.add_argument("out", nargs="?", default=os.path.join(tc.ROOT, "build", "img", "GTA3PS2"))
    ex.add_argument("names", nargs="*")
    ex.add_argument("--iso")
    a = ap.parse_args()
    if a.cmd == "table":
        return table(a.save)
    t = load_table()
    entries = pick(t["entries"], a.names)
    if a.cmd == "list":
        for e in entries:
            print("%5d  offset %6d  %4d sectors  %s" % (e["index"], e["offset"], e["sectors"], file_name(e)))
        print("%d entries" % len(entries))
        return
    os.makedirs(a.out, exist_ok=True)
    with open(iso_path(a.iso), "rb") as iso:
        for e in entries:
            iso.seek((t["img"]["lsn"] + e["offset"]) * 2048)
            data = iso.read(e["sectors"] * 2048)
            open(os.path.join(a.out, file_name(e)), "wb").write(data)
    print("%d entries -> %s (the game's data: keep it out of git)" % (len(entries), a.out))


if __name__ == "__main__":
    sys.exit(main())

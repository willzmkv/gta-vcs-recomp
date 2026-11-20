#!/usr/bin/env python3
"""The game's text files (AMERICAN.GXT, ENGLISH.GXT, ...) to an editable table and back - for translations.

    python tools/gxt.py export GXT OUT.tsv [--ref REF.GXT]   every text as a row: table, key, text (+ the reference's)
    python tools/gxt.py import IN.tsv BASE.GXT OUT.GXT       BASE's tables and keys with IN's texts (missing: BASE's)
    python tools/gxt.py info GXT                              tables, keys, characters outside Latin-1
    python tools/gxt.py --self-test                           a made-up file through export / import, byte for byte

Format (read from the disc's files, docs/game-internals.md 6n): "TABL" + size + {name[8], offset} per table, MAIN
first; at each offset a table: (not MAIN) its name[8], then "TKEY" + size + {u32 offset into TDAT, key[8]} sorted by
key (the game looks keys up by binary search), then "TDAT" + size + the texts, UTF-16LE, each ended by a 0. Tables
follow each other, every one 4-byte aligned. A file written here holds the same texts as the one read (checked on all
six of the disc's) but not the same bytes: the disc's share one text between keys and pad names with spaces.

The rows are UTF-8, tab separated; a text's tabs and line breaks never occur (the game breaks lines with ~n~). The
game's files and exports stay out of git (build/, or a mod folder): text is game data.
"""
import argparse
import os
import struct
import sys


def read(path):
    """[(table, [(key, text)])] in the file's order."""
    d = open(path, "rb").read()
    if d[:4] != b"TABL":
        raise ValueError("%s: not a GXT (no TABL)" % path)
    n = struct.unpack_from("<I", d, 4)[0]
    tables = []
    for i in range(0, n, 12):
        name = d[8 + i:16 + i].split(b"\0")[0].decode("ascii")
        off = struct.unpack_from("<I", d, 16 + i)[0]
        k = off if name == "MAIN" else off + 8
        if d[k:k + 4] != b"TKEY":
            raise ValueError("%s: table %s has no TKEY" % (path, name))
        keys_size = struct.unpack_from("<I", d, k + 4)[0]
        t = k + 8 + keys_size
        if d[t:t + 4] != b"TDAT":
            raise ValueError("%s: table %s has no TDAT" % (path, name))
        data = t + 8
        entries = []
        for e in range(k + 8, k + 8 + keys_size, 12):
            at, key = struct.unpack_from("<I8s", d, e)
            s = data + at
            end = s
            while d[end:end + 2] != b"\0\0":
                end += 2
            entries.append((key.split(b"\0")[0].decode("ascii"), d[s:end].decode("utf-16-le")))
        tables.append((name, entries))
    return tables


def write(tables):
    """The file's bytes for [(table, [(key, text)])] (keys kept in the order given: sorted, as read)."""
    def pad(b):
        return b + b"\0" * (-len(b) % 4)
    blobs = []
    for name, entries in tables:
        data = b""
        keys = b""
        for key, text in entries:
            keys += struct.pack("<I8s", len(data), key.encode("ascii"))
            data += text.encode("utf-16-le") + b"\0\0"
        blob = (b"" if name == "MAIN" else name.encode("ascii").ljust(8, b"\0")) + b"TKEY" + struct.pack("<I", len(keys)) + keys
        blob += b"TDAT" + struct.pack("<I", len(pad(data))) + pad(data)
        blobs.append((name, pad(blob)))
    head = b"TABL" + struct.pack("<I", 12 * len(blobs))
    at = len(head) + 12 * len(blobs)
    index, body = b"", b""
    for name, blob in blobs:
        index += name.encode("ascii").ljust(8, b"\0") + struct.pack("<I", at + len(body))
        body += blob
    return head + index + body


def export(gxt, out, ref=None):
    reference = {}
    if ref:
        for name, entries in read(ref):
            for key, text in entries:
                reference[(name, key)] = text
    rows = 0
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("table\tkey\ttext" + ("\treference" if ref else "") + "\n")
        for name, entries in read(gxt):
            for key, text in entries:
                if "\t" in text or "\n" in text:
                    raise ValueError("%s %s: a tab or line break in the text" % (name, key))
                f.write("%s\t%s\t%s%s\n" % (name, key, text, "\t" + reference.get((name, key), "") if ref else ""))
                rows += 1
    return rows


def import_(tsv, base, out):
    texts = {}
    with open(tsv, encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split("\t")
        if header[:3] != ["table", "key", "text"]:
            raise ValueError("%s: not an export (header %s)" % (tsv, header[:3]))
        for line in f:
            cells = line.rstrip("\n").split("\t")
            if len(cells) >= 3:
                texts[(cells[0], cells[1])] = cells[2]
    tables, changed = [], 0
    for name, entries in read(base):
        new = []
        for key, text in entries:
            t = texts.get((name, key), text)
            changed += t != text
            new.append((key, t))
        tables.append((name, new))
    open(out, "wb").write(write(tables))
    return changed


def info(gxt):
    tables = read(gxt)
    keys = sum(len(e) for _, e in tables)
    odd = sorted({c for _, e in tables for _, t in e for c in t if ord(c) > 0xFF})
    print("%s: %d tables, %d texts; characters outside Latin-1: %s" % (gxt, len(tables), keys,
                                                                       " ".join("U+%04X" % ord(c) for c in odd) or "none"))


def self_test():
    import tempfile
    tables = [("MAIN", [("A", "Pressione ~k~ ~PDFW~ para atirar."), ("B_KEY", "botão"), ("Z", "")]),
              ("JER_A1", [("JER_A1", "Soldado"), ("JER_A1A", "Vá para o ~y~quartel~w~.")])]
    with tempfile.TemporaryDirectory() as tmp:
        a, tsv, b = (os.path.join(tmp, n) for n in ("a.gxt", "a.tsv", "b.gxt"))
        open(a, "wb").write(write(tables))
        assert read(a) == tables
        export(a, tsv)
        import_(tsv, a, b)
        assert open(a, "rb").read() == open(b, "rb").read(), "export / import changed the bytes"
        with open(tsv, encoding="utf-8") as f:
            rows = f.read().replace("Soldado", "Soldado (revisado)")
        with open(tsv, "w", encoding="utf-8", newline="\n") as f:
            f.write(rows)
        assert import_(tsv, a, b) == 1 and dict(read(b)[1][1])["JER_A1"] == "Soldado (revisado)"
    print("[gxt] self-test ok")


def main():
    if "--self-test" in sys.argv:
        self_test()
        return 0
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export"); e.add_argument("gxt"); e.add_argument("out"); e.add_argument("--ref")
    i = sub.add_parser("import"); i.add_argument("tsv"); i.add_argument("base"); i.add_argument("out")
    n = sub.add_parser("info"); n.add_argument("gxt")
    a = ap.parse_args()
    if a.cmd == "export":
        print("[gxt] %d texts -> %s" % (export(a.gxt, a.out, a.ref), a.out))
    elif a.cmd == "import":
        print("[gxt] %d texts changed -> %s" % (import_(a.tsv, a.base, a.out), a.out))
    else:
        info(a.gxt)
    return 0


if __name__ == "__main__":
    sys.exit(main())

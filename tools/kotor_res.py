"""Minimal KEY/BIF reader for KOTOR 2 -- list and extract game resources.

The base game assets live in data\\*.bif, indexed by chitin.key. Override files
beat BIF entries, so knowing exactly what ships in the BIFs tells us which
script names are safe to wrap and which are already claimed by a mod.

    python tools/kotor_res.py list ncs [substring]
    python tools/kotor_res.py extract <resref> <type> <outfile>

Resource types: nss=2009, ncs=2010, are=2012, git=2023, utc=2027
"""

import os
import struct
import sys

GAME = r"G:\SteamLibrary\steamapps\common\Knights of the Old Republic II"
KEY = os.path.join(GAME, "chitin.key")

TYPES = {"nss": 2009, "ncs": 2010, "are": 2012, "git": 2023, "utc": 2027, "ifo": 2014,
         "dlg": 2029, "2da": 2017,
         # added 2026-09-02 for the movement/collision work: models, walkmeshes, layouts, GUI
         "mdl": 2002, "mdx": 3008, "wok": 2016, "pwk": 3009, "dwk": 3010, "lyt": 3000,
         "vis": 3001, "txi": 2022, "tpc": 3007, "gui": 2047, "utp": 2044, "utd": 2042,
         "tlk": 2018}


def read_key(path=KEY):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"KEY ":
        raise SystemExit("not a KEY file: %s" % path)

    bif_count, key_count = struct.unpack_from("<II", data, 8)
    file_off, key_off = struct.unpack_from("<II", data, 16)

    bifs = []
    for i in range(bif_count):
        o = file_off + i * 12
        _size, name_off, name_len, _drives = struct.unpack_from("<IIHH", data, o)
        name = data[name_off:name_off + name_len].split(b"\0")[0].decode("ascii", "replace")
        bifs.append(name.replace("\\", os.sep))

    entries = []
    for i in range(key_count):
        o = key_off + i * 22
        resref = data[o:o + 16].split(b"\0")[0].decode("ascii", "replace").lower()
        restype, resid = struct.unpack_from("<IH", data, o + 16)[0:1] + (0,)
        restype = struct.unpack_from("<H", data, o + 16)[0]
        resid = struct.unpack_from("<I", data, o + 18)[0]
        entries.append((resref, restype, resid >> 20, resid & 0xFFFFF))
    return bifs, entries


def extract(bifs, entries, resref, restype):
    resref = resref.lower()
    for name, rtype, bif_idx, res_idx in entries:
        if name != resref or rtype != restype:
            continue
        bif_path = os.path.join(GAME, bifs[bif_idx])
        with open(bif_path, "rb") as fh:
            head = fh.read(20)
            if head[:4] != b"BIFF":
                raise SystemExit("not a BIF: %s" % bif_path)
            var_count = struct.unpack_from("<I", head, 8)[0]
            var_off = struct.unpack_from("<I", head, 16)[0]
            fh.seek(var_off + res_idx * 16)
            _rid, offset, size, _rt = struct.unpack("<IIII", fh.read(16))
            fh.seek(offset)
            return fh.read(size)
    return None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    bifs, entries = read_key()
    cmd = sys.argv[1]

    if cmd == "list":
        want = TYPES.get(sys.argv[2].lower(), 2010) if len(sys.argv) > 2 else 2010
        sub = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        hits = [e for e in entries if e[1] == want and sub in e[0]]
        print("BIF archives: %d | total key entries: %d" % (len(bifs), len(entries)))
        print("matching resources: %d\n" % len(hits))
        for name, _rt, bif_idx, _ri in sorted(hits)[:400]:
            print("  %-18s  %s" % (name, os.path.basename(bifs[bif_idx])))
        return 0

    if cmd == "extract":
        resref, tname, out = sys.argv[2], sys.argv[3], sys.argv[4]
        blob = extract(bifs, entries, resref, TYPES.get(tname.lower(), 2010))
        if blob is None:
            raise SystemExit("not found: %s.%s" % (resref, tname))
        with open(out, "wb") as fh:
            fh.write(blob)
        print("extracted %s.%s -> %s (%d bytes)" % (resref, tname, out, len(blob)))
        return 0

    raise SystemExit(__doc__)


if __name__ == "__main__":
    sys.exit(main())

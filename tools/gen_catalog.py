"""Build the object catalogue the in-game console browses.

    python tools/gen_catalog.py                       # -> data/k2se_catalog.csv
    python tools/gen_catalog.py --summary
    python tools/gen_catalog.py --grep lantern

F10 used to append a position to a text file. To let it browse and place real
objects instead, the console needs to know what objects exist -- and that is a
question about the game's data, not about its code, so it is answered here,
once, offline. The DLL loads a flat CSV and never parses an archive.

What goes in:

  placeables.2da     every placeable model the engine knows, with its label
  genericdoors.2da   the door models
  *.utp / *.utd      the actual blueprints, from the BIFs and from all 246
                     module archives, since most props only exist inside the
                     module that uses them
  dialog.tlk         so a row reads "Footlocker" and not "strref 12345"

Formats are read directly rather than through a library, matching kotor_res.py
and twoda.py beside it. RIM, ERF and GFF layouts follow reone
(ref/reone/src/libs/resource/format/), which is the oracle for every offset.
Only the handful of GFF fields the catalogue needs are decoded; this is not a
general GFF reader.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))), "k2-directional-movement", "tools"))

import kotor_res  # noqa: E402
from twoda import TwoDA  # noqa: E402

RES_2DA = 2017
RES_UTP = 2044
RES_UTD = 2042
RES_UTC = 2027

CSV_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "data", "k2se_catalog.csv")

# GFF field types that carry their value inline in the field entry itself.
GFF_INLINE = {0: "B", 2: "H", 4: "I", 5: "i"}
GFF_CEXOSTRING = 10
GFF_RESREF = 11
GFF_CEXOLOCSTRING = 12


# --- archives ----------------------------------------------------------------

def read_rim(path):
    """[(resref, restype, offset, size)] from a .rim."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"RIM V1.0":
        return []
    count, offset = struct.unpack_from("<II", data, 0x0C)
    out = []
    for i in range(count):
        o = offset + i * 32
        if o + 32 > len(data):
            break
        resref = data[o:o + 16].split(b"\0")[0].decode("ascii", "replace").lower()
        restype = struct.unpack_from("<H", data, o + 16)[0]
        res_off, res_size = struct.unpack_from("<II", data, o + 24)
        out.append((resref, restype, res_off, res_size))
    return out


def read_erf(path):
    """[(resref, restype, offset, size)] from a .mod/.erf/.sav."""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:3] not in (b"MOD", b"ERF", b"SAV"):
        return []
    count = struct.unpack_from("<I", data, 0x10)[0]
    off_keys, off_res = struct.unpack_from("<II", data, 0x18)
    out = []
    for i in range(count):
        k = off_keys + i * 24
        r = off_res + i * 8
        if k + 24 > len(data) or r + 8 > len(data):
            break
        resref = data[k:k + 16].split(b"\0")[0].decode("ascii", "replace").lower()
        restype = struct.unpack_from("<H", data, k + 20)[0]
        res_off, res_size = struct.unpack_from("<II", data, r)
        out.append((resref, restype, res_off, res_size))
    return out


def archive_entries(path):
    lower = path.lower()
    if lower.endswith(".rim"):
        return read_rim(path)
    if lower.endswith((".mod", ".erf", ".sav")):
        return read_erf(path)
    return []


# --- GFF, only as far as the catalogue needs ---------------------------------

def gff_top_fields(blob):
    """Top-level field name -> value, for the simple field types only.

    Lists and nested structs are skipped: a blueprint's interesting facts
    (Appearance, Tag, LocName) all live on the root struct.
    """
    if len(blob) < 56 or blob[4:8] not in (b"V3.2", b"V3.3"):
        return {}
    (struct_off, _struct_count, field_off, _field_count, label_off, _label_count,
     data_off, _data_count, indices_off, _indices_count) = struct.unpack_from(
        "<10I", blob, 8)

    # Root struct: type, dataOrOffset, fieldCount.
    _type, root_data, root_count = struct.unpack_from("<3I", blob, struct_off)
    if root_count == 1:
        field_indices = [root_data]
    else:
        field_indices = list(struct.unpack_from(
            "<%dI" % root_count, blob, indices_off + root_data))

    out = {}
    for index in field_indices:
        fo = field_off + index * 12
        if fo + 12 > len(blob):
            continue
        ftype, label_index, value = struct.unpack_from("<3I", blob, fo)
        lo = label_off + label_index * 16
        if lo + 16 > len(blob):
            continue
        label = blob[lo:lo + 16].split(b"\0")[0].decode("ascii", "replace")

        if ftype in GFF_INLINE:
            out[label] = struct.unpack_from("<" + GFF_INLINE[ftype], blob, fo + 8)[0]
        elif ftype == GFF_CEXOSTRING:
            at = data_off + value
            length = struct.unpack_from("<I", blob, at)[0]
            out[label] = blob[at + 4:at + 4 + length].decode("ascii", "replace")
        elif ftype == GFF_RESREF:
            at = data_off + value
            length = blob[at]
            out[label] = blob[at + 1:at + 1 + length].decode("ascii", "replace")
        elif ftype == GFF_CEXOLOCSTRING:
            at = data_off + value
            _total, strref = struct.unpack_from("<II", blob, at)
            out[label] = ("strref", strref if strref != 0xFFFFFFFF else -1)
    return out


# --- dialog.tlk, so the catalogue reads in words -----------------------------

class TalkTable(object):
    def __init__(self, path):
        self.entries = []
        if not os.path.isfile(path):
            return
        with open(path, "rb") as fh:
            data = fh.read()
        if data[:8] != b"TLK V3.0":
            return
        count, string_offset = struct.unpack_from("<II", data, 12)
        for i in range(count):
            o = 20 + i * 40
            if o + 40 > len(data):
                break
            off, size = struct.unpack_from("<II", data, o + 28)
            at = string_offset + off
            self.entries.append(data[at:at + size].decode("windows-1252", "replace"))

    def get(self, strref):
        if strref is None or strref < 0 or strref >= len(self.entries):
            return ""
        return self.entries[strref].replace("\n", " ").replace("\r", " ").strip()


# --- the catalogue -----------------------------------------------------------

class Row(object):
    __slots__ = ("kind", "resref", "name", "tag", "appearance", "model", "source")

    def __init__(self, kind, resref, name, tag, appearance, model, source):
        self.kind = kind
        self.resref = resref
        self.name = name
        self.tag = tag
        self.appearance = appearance
        self.model = model
        self.source = source


def clean(text):
    """CSV-safe: the DLL's parser splits on commas and does not do quoting."""
    return (text or "").replace(",", ";").replace("\n", " ").replace("\r", " ").strip()


def build(game):
    kotor_res.GAME = game
    bifs, entries = kotor_res.read_key(os.path.join(game, "chitin.key"))
    tlk = TalkTable(os.path.join(game, "dialog.tlk"))

    def table(name):
        blob = kotor_res.extract(bifs, entries, name, RES_2DA)
        return TwoDA.parse(blob) if blob else None

    # Model tables first: they are what "every 3D object in the game" means.
    models = {}      # ("placeable", appearance row) -> (label, model resref)
    placeables = table("placeables")
    if placeables:
        for row in range(placeables.rows):
            models[("placeable", row)] = (placeables.get(row, "label"),
                                          placeables.get(row, "modelname"))
    doors = table("genericdoors")
    if doors:
        for row in range(doors.rows):
            models[("door", row)] = (doors.get(row, "label"), doors.get(row, "modelname"))

    rows = []
    seen = set()

    def add(kind, resref, blob, source):
        key = (kind, resref)
        if key in seen:
            return
        seen.add(key)
        fields = gff_top_fields(blob)
        appearance = fields.get("Appearance", fields.get("GenericType", -1))
        if not isinstance(appearance, int):
            appearance = -1
        name = ""
        loc = fields.get("LocName")
        if isinstance(loc, tuple) and loc[0] == "strref":
            name = tlk.get(loc[1])
        label, model = models.get((kind, appearance), ("", ""))
        rows.append(Row(kind, resref, name or label, fields.get("Tag", ""),
                        appearance, model, source))

    # Blueprints shipped in the BIFs.
    wanted = {RES_UTP: "placeable", RES_UTD: "door"}
    for resref, restype, _bif, _idx in entries:
        if restype in wanted:
            blob = kotor_res.extract(bifs, entries, resref, restype)
            if blob:
                add(wanted[restype], resref, blob, "bif")

    # Blueprints that only exist inside a module.
    modules_dir = os.path.join(game, "modules")
    if os.path.isdir(modules_dir):
        for filename in sorted(os.listdir(modules_dir)):
            path = os.path.join(modules_dir, filename)
            found = archive_entries(path)
            if not found:
                continue
            blob_cache = None
            for resref, restype, offset, size in found:
                if restype not in wanted or (wanted[restype], resref) in seen:
                    continue
                if blob_cache is None:
                    with open(path, "rb") as fh:
                        blob_cache = fh.read()
                add(wanted[restype], resref, blob_cache[offset:offset + size],
                    os.path.splitext(filename)[0])

    # Every model row that no blueprint referenced still belongs in the list:
    # Renan asked to see all of them, and an unused model is exactly the kind of
    # thing worth finding.
    used = {(r.kind, r.appearance) for r in rows}
    for (kind, appearance), (label, model) in sorted(models.items()):
        if (kind, appearance) in used or not model or model.lower() == "****":
            continue
        rows.append(Row(kind + "-model", "", label, "", appearance, model, "2da"))

    rows.sort(key=lambda r: (r.kind, r.name.lower(), r.resref))
    return rows


def write_csv(path, rows):
    directory = os.path.dirname(os.path.abspath(path))
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)
    with open(path, "w", encoding="windows-1252", errors="replace", newline="\r\n") as fh:
        fh.write("kind,resref,name,tag,appearance,model,source\n")
        for r in rows:
            fh.write("%s,%s,%s,%s,%d,%s,%s\n" % (
                r.kind, clean(r.resref), clean(r.name), clean(r.tag),
                r.appearance, clean(r.model), clean(r.source)))
    print("wrote %s (%d rows)" % (path, len(rows)))


def summarise(rows):
    kinds = {}
    for r in rows:
        kinds[r.kind] = kinds.get(r.kind, 0) + 1
    print("catalogue: %d rows" % len(rows))
    for kind in sorted(kinds):
        print("  %-18s %5d" % (kind, kinds[kind]))
    named = sum(1 for r in rows if r.name)
    modelled = sum(1 for r in rows if r.model)
    print("  with a readable name: %d" % named)
    print("  with a model resref : %d" % modelled)
    print("  from modules        : %d" % sum(1 for r in rows if r.source
                                             not in ("bif", "2da")))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", default=kotor_res.GAME)
    ap.add_argument("--out", default=CSV_PATH)
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--grep", default=None, help="print matching rows instead of writing")
    args = ap.parse_args()

    rows = build(args.game)

    if args.grep:
        needle = args.grep.lower()
        hits = [r for r in rows if needle in r.name.lower() or needle in r.resref.lower()
                or needle in r.model.lower() or needle in r.tag.lower()]
        print("%d row(s) matching %r:" % (len(hits), args.grep))
        for r in hits[:80]:
            print("  %-16s %-18s %-28.28s model %-16s %s"
                  % (r.kind, r.resref, r.name, r.model, r.source))
        return 0

    write_csv(args.out, rows)
    summarise(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())

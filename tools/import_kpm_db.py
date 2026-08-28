"""Import Kotor-Patch-Manager's KOTOR 2 address database into K2SE.

    python tools/import_kpm_db.py [path-to-kotor2_steam_aspyr.db] [path-to-exe]

Source: https://github.com/LaneDibello/Kotor-Patch-Manager (MIT), file
`AddressDatabases/kotor2_steam_aspyr.db`.

Only DATA is taken -- addresses and struct offsets, which are facts about a
binary we both target. No source code from that project is used here.

Every function address is checked against the real executable before it is
admitted: it must live in .text and start with the MSVC frame prologue. That
matters more than usual here, because the database's own description says the
Steam rows were "generated from GOG Aspyr mappings" -- auto-ported rather than
independently reversed. The sweep is what turns a claim into a verified value.

Rows whose address fails the check are written with verified_by=unverified and a
note, never silently dropped: knowing an import is bad is worth recording.
"""

import os
import sqlite3
import sys

import addressdb
import peimage

DEFAULT_DB = os.path.join(os.path.expanduser("~"), "Desktop", "Patch-Manager",
                          "AddressDatabases", "kotor2_steam_aspyr.db")

PROVENANCE = "kpm-db"

# Globals hold a pointer that is only populated once the game is running, so
# there is nothing to verify statically beyond "is this a writable data address".
# They are marked unverified unless K2SE has separately confirmed them.
GLOBAL_SECTIONS = (".data", ".rdata")

# Addresses K2SE derived independently by disassembly, before this import existed.
# The import must agree with every one of them; a mismatch means one of the two
# derivations is wrong and the run aborts rather than quietly picking a winner.
CROSSCHECK = {
    ("function", "CVirtualMachine", "StackPopInteger"): 0x006FD9A0,
    ("function", "CVirtualMachine", "StackPushInteger"): 0x006FD9C0,
    ("function", "CVirtualMachine", "StackPopFloat"): 0x006FD9E0,
    ("function", "CVirtualMachine", "StackPushFloat"): 0x006FDA00,
    ("function", "CVirtualMachine", "StackPopVector"): 0x006FDA20,
    ("function", "CVirtualMachine", "StackPushVector"): 0x006FDA40,
    ("function", "CVirtualMachine", "StackPopObject"): 0x006FDAF0,
    ("function", "CVirtualMachine", "StackPushObject"): 0x006FDB10,
    ("function", "Other", "AurPostString"): 0x00474C00,
    ("global", "", "VIRTUAL_MACHINE_PTR"): 0x00A1B4A8,
}

# K2SE has already exercised these in a live session (DESIGN.md Q5/Q6, and the
# 2026-08-27 in-game battery), so the import must not downgrade them to
# "prologue".
RUNTIME_PROVEN = {
    ("function", "CVirtualMachine", "StackPopInteger"),
    ("function", "CVirtualMachine", "StackPushInteger"),
    ("function", "CVirtualMachine", "StackPopFloat"),
    ("function", "CVirtualMachine", "StackPushFloat"),
    ("function", "CVirtualMachine", "StackPopObject"),
    ("function", "CVirtualMachine", "StackPushObject"),
    ("function", "Other", "AurPostString"),
    ("global", "", "VIRTUAL_MACHINE_PTR"),
}


def main():
    db_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DB
    exe_path = sys.argv[2] if len(sys.argv) > 2 else None

    if not os.path.exists(db_path):
        raise SystemExit("address database not found: %s" % db_path)
    img = peimage.open_default(exe_path)

    print("exe : %s" % img.path)
    print("db  : %s" % db_path)

    db = sqlite3.connect(db_path)
    cur = db.cursor()

    sha, game, ver, plat = cur.execute(
        "SELECT sha256_hash, game_name, version_string, platform FROM game_version"
    ).fetchone()
    print("db targets: %s %s (%s)" % (game, ver, plat))
    print("db sha256 : %s" % sha)
    if sha.upper() != peimage.PRISTINE_SHA256:
        print("\nWARNING: this database is keyed to a different binary than K2SE targets.")
        print("         Import refused -- the addresses would not apply.")
        return 1
    print("            matches the build K2SE targets\n")

    rows = []
    bad = 0

    # --- functions ----------------------------------------------------------
    for cls, name, addr in cur.execute(
            "SELECT class_name, function_name, address FROM functions"):
        ok = img.has_prologue(addr)
        k = ("function", cls, name)
        if k in RUNTIME_PROVEN:
            level, note = "runtime", "exercised in a live K2SE session"
        elif ok:
            level, note = "prologue", ""
        else:
            level, note = "unverified", "no MSVC prologue at this address"
            bad += 1
        rows.append(dict(kind="function", **{"class": cls}, name=name,
                         value="0x%08X" % addr, provenance=PROVENANCE,
                         verified_by=level, notes=note))

    # --- global pointers ----------------------------------------------------
    for name, addr in cur.execute(
            "SELECT pointer_name, address FROM global_pointers"):
        sec = img.section_of(addr)
        k = ("global", "", name)
        if k in RUNTIME_PROVEN:
            level, note = "runtime", "exercised in a live K2SE session"
        elif sec in GLOBAL_SECTIONS:
            level = "unverified"
            note = "in %s; holds a runtime pointer, not statically checkable" % sec
        else:
            level, note = "unverified", "not in a data section"
            bad += 1
        rows.append(dict(kind="global", **{"class": ""}, name=name,
                         value="0x%08X" % addr, provenance=PROVENANCE,
                         verified_by=level, notes=note))

    # --- struct offsets -----------------------------------------------------
    # Nothing about a struct offset can be checked from the file alone; it is a
    # claim about an object's layout at runtime.
    for cls, member, off in cur.execute(
            "SELECT class_name, member_name, offset FROM offsets"):
        rows.append(dict(kind="offset", **{"class": cls}, name=member,
                         value="0x%04X" % off, provenance=PROVENANCE,
                         verified_by="unverified",
                         notes="struct layout; confirm against a consuming handler"))

    # --- cross-check against K2SE's own hand-derived values ------------------
    print("cross-check against K2SE's independently derived addresses:")
    index = {addressdb.key(r): r for r in rows}
    disagreements = 0
    for k, expected in sorted(CROSSCHECK.items()):
        row = index.get(k)
        if row is None:
            print("  %-40s MISSING from the database" % ("%s::%s" % (k[1], k[2])))
            disagreements += 1
            continue
        got = int(row["value"], 16)
        agree = got == expected
        print("  %-40s 0x%08X  %s" % ("%s::%s" % (k[1] or "-", k[2]), got,
                                      "agrees" if agree else
                                      "DISAGREES with K2SE's 0x%08X" % expected))
        if not agree:
            disagreements += 1

    if disagreements:
        print("\n%d disagreement(s) with K2SE's own derivations. Import ABORTED --"
              % disagreements)
        print("one of the two sources is wrong and that must be resolved by hand.")
        return 1
    print("  -> all %d agree\n" % len(CROSSCHECK))

    existing = addressdb.load()
    merged, added, updated, kept = addressdb.merge(existing, rows)
    total = addressdb.save(merged)

    verified = sum(1 for r in merged if r.get("verified_by") not in ("", "unverified"))
    print("imported: %d functions, %d globals, %d offsets"
          % (sum(1 for r in rows if r["kind"] == "function"),
             sum(1 for r in rows if r["kind"] == "global"),
             sum(1 for r in rows if r["kind"] == "offset")))
    print("merge   : %d added, %d updated, %d kept (existing was stronger)"
          % (added, updated, kept))
    print("table   : %d rows, %d verified on this binary" % (total, verified))
    if bad:
        print("\n%d row(s) FAILED verification and are marked unverified." % bad)
    print("\nwrote %s" % addressdb.CSV_PATH)
    print("next: python tools/gen_offsets.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())

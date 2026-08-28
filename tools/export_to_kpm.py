"""Export K2SE's own findings in Kotor-Patch-Manager's import format.

    python tools/export_to_kpm.py [outdir]     (default: out/kpm-contribution)

Writes CSVs their SqliteTools accepts:

    functions.csv   class_name,function_name,address,calling_convention,param_size_bytes,notes
    globals.csv     pointer_name,address,notes
    offsets.csv     class_name,member_name,offset,notes
    classes.csv     class_name,size,notes
    vtables.csv     class_name,vtable_address

Only rows with provenance=k2se-ghidra are exported -- exporting their own data
back at them would be noise, and worse, could overwrite a value they have since
corrected.

Why this is worth doing: their KOTOR 2 schema has no home for the
CSWVirtualMachineCommands story at all. There is no vtable row, no
ExecuteCommand, no InitializeCommands, no routine-table constants, and the
`classes` table is empty for all three KOTOR 2 builds -- which leaves their own
CExoString wrapper unable to size its allocation on KOTOR 2. sizeof == 8 is one
row and fixes that.

Two format quirks of their importers, worth getting right rather than having a
row silently skipped on their end:
  * offsets.csv and classes.csv parse DECIMAL only. A 0x-prefixed value is
    skipped with a warning, so those two files are written in decimal.
  * functions.csv, globals.csv and vtables.csv accept hex.
Their `classes` table is at schema v3 and `classes.vtable` at v5, so
vtables.csv needs a database migrated to v5 first.
"""

import csv
import os
import sys

import addressdb

OURS = "k2se-ghidra"


def write(path, header, rows):
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh, lineterminator="\n")
        w.writerow(header)
        w.writerows(rows)
    print("  %-14s %d row(s)" % (os.path.basename(path), len(rows)))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "out", "kpm-contribution")
    os.makedirs(outdir, exist_ok=True)

    rows = [r for r in addressdb.load() if r.get("provenance") == OURS]
    if not rows:
        raise SystemExit("no k2se-ghidra rows found; run tools/seed_k2se_findings.py")

    def sel(kind):
        return sorted([r for r in rows if r["kind"] == kind],
                      key=lambda r: (r["class"], r["name"]))

    print("exporting %d K2SE-derived row(s) to %s" % (len(rows), outdir))

    write(os.path.join(outdir, "functions.csv"),
          ["class_name", "function_name", "address", "calling_convention",
           "param_size_bytes", "notes"],
          [[r["class"], r["name"], r["value"], "", "", r["notes"]] for r in sel("function")])

    # Their globals table has no class column; K2SE's global rows carry an empty
    # class already.
    write(os.path.join(outdir, "globals.csv"),
          ["pointer_name", "address", "notes"],
          [[r["name"], r["value"], r["notes"]] for r in sel("global")])

    # Decimal: their offset importer rejects hex.
    write(os.path.join(outdir, "offsets.csv"),
          ["class_name", "member_name", "offset", "notes"],
          [[r["class"], r["name"], str(r["value_int"]), r["notes"]] for r in sel("offset")])

    write(os.path.join(outdir, "classes.csv"),
          ["class_name", "size", "notes"],
          [[r["class"], str(r["value_int"]), r["notes"]] for r in sel("class")])

    write(os.path.join(outdir, "vtables.csv"),
          ["class_name", "vtable_address"],
          [[r["class"], r["value"]] for r in sel("vtable")])

    # Constants have no table in their schema. Emit them as a note file rather
    # than dropping them: they are the most K2-specific thing K2SE knows.
    consts = sel("constant")
    if consts:
        path = os.path.join(outdir, "NOTES-constants.md")
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# Constants with no home in the current schema\n\n")
            fh.write("These are values read out of instruction immediates in\n")
            fh.write("swkotor2.exe 1.0.2.0 Steam/Aspyr. There is no table for them,\n")
            fh.write("so they are recorded here.\n\n")
            for r in consts:
                fh.write("- `%s::%s` = `%s` (%d) -- %s\n"
                         % (r["class"], r["name"], r["value"], r["value_int"], r["notes"]))
        print("  %-14s %d constant(s)" % (os.path.basename(path), len(consts)))

    print("\nTheir import commands (a database migrated to v5 is required for vtables):")
    db = "AddressDatabases/kotor2_steam_aspyr.db"
    for cmd, f in (("import-ghidra", "functions.csv"), ("import-globals", "globals.csv"),
                   ("import-offsets", "offsets.csv"), ("import-classes", "classes.csv"),
                   ("import-vtables", "vtables.csv")):
        print("  dotnet run --project tools/SqliteTools -- %s --csv %s --database %s"
              % (cmd, f, db))
    return 0


if __name__ == "__main__":
    sys.exit(main())

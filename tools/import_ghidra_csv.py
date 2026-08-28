"""Merge a Ghidra export into K2SE's address table.

    python tools/import_ghidra_csv.py functions.csv [path-to-exe]

Produce the CSV with tools/ghidra/ExportK2SE.java. Columns:

    class_name,function_name,address,calling_convention,param_size_bytes,notes

Every address is re-verified against the executable before it is admitted, on
exactly the same terms as the third-party import: a name in a Ghidra project is
a label someone typed, and a label is not evidence. Rows that fail are reported
and skipped rather than written as unverified -- unlike the database import,
where recording a known-bad third-party row has diagnostic value, a bad row here
means our own project is wrong and should be fixed at the source.

Uses Python's csv module, which handles quoting properly. The exporter also
flattens newlines, so a multi-line plate comment cannot split a record.
"""

import csv
import os
import sys

import addressdb
import peimage

PROVENANCE = "k2se-ghidra"
REQUIRED = ("class_name", "function_name", "address")


def parse_address(text):
    t = (text or "").strip()
    if not t:
        return None
    try:
        return int(t, 16) if t.lower().startswith("0x") else int(t, 10)
    except ValueError:
        return None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    csv_path = sys.argv[1]
    if not os.path.exists(csv_path):
        raise SystemExit("no such file: %s" % csv_path)

    img = peimage.open_default(sys.argv[2] if len(sys.argv) > 2 else None)
    print("exe : %s" % img.path)
    print("csv : %s" % csv_path)

    with open(csv_path, "r", newline="", encoding="utf-8-sig") as fh:
        reader = csv.DictReader(fh)
        missing = [c for c in REQUIRED if c not in (reader.fieldnames or [])]
        if missing:
            raise SystemExit("CSV is missing required column(s): %s\n  found: %s"
                             % (", ".join(missing), ", ".join(reader.fieldnames or [])))
        raw = list(reader)

    rows = []
    rejected = []
    for i, r in enumerate(raw, start=2):  # line 1 is the header
        cls = (r.get("class_name") or "").strip() or "Other"
        name = (r.get("function_name") or "").strip()
        va = parse_address(r.get("address"))
        if not name or va is None:
            rejected.append((i, name or "?", r.get("address"), "unparseable row"))
            continue
        if not img.has_prologue(va):
            rejected.append((i, "%s::%s" % (cls, name), "0x%08X" % va,
                             "no MSVC prologue in .text (section %s)"
                             % (img.section_of(va) or "none")))
            continue

        note = (r.get("notes") or "").strip()
        cc = (r.get("calling_convention") or "").strip()
        psz = (r.get("param_size_bytes") or "").strip()
        extra = []
        if cc:
            extra.append(cc)
        if psz:
            extra.append("ret %s" % psz)
        if extra:
            note = ("%s; %s" % ("/".join(extra), note)).strip("; ")

        rows.append({
            "kind": "function", "class": cls, "name": name,
            "value": "0x%08X" % va, "provenance": PROVENANCE,
            "verified_by": "prologue", "notes": note,
        })

    print("\nparsed %d row(s): %d verified, %d rejected" % (len(raw), len(rows), len(rejected)))
    for line, what, addr, why in rejected:
        print("  REJECT line %-5d %-44s %-12s %s" % (line, what, addr, why))

    if not rows:
        print("\nnothing to import.")
        return 1 if rejected else 0

    merged, added, updated, kept = addressdb.merge(addressdb.load(), rows)
    total = addressdb.save(merged)
    print("\nmerge : %d added, %d updated, %d kept (existing was verified more strongly)"
          % (added, updated, kept))
    print("table : %d rows" % total)
    print("\nnext: python tools/gen_offsets.py && python tools/verify_offsets.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())

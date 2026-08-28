"""Record K2SE's own discoveries in the address table.

    python tools/seed_k2se_findings.py

Everything here was derived in this project by disassembling swkotor2.exe, and
most of it has been exercised in a live game session. None of it exists in any
KOTOR 2 address database in the wild -- the whole CSWVirtualMachineCommands
story (the vtable, the dispatcher, the 877-entry routine table) is absent from
Kotor-Patch-Manager's KOTOR 2 schema, which has no rows for it at all.

Putting it in the same table as the imported data is what makes
tools/export_to_kpm.py a straight filter on provenance, so the findings can go
upstream instead of living only in this repo's header comments.

Where a row already exists from the import, the merge upgrades its verified_by
if we now know more -- e.g. the CExoString accessors arrived as `prologue` and
are now `disasm`, because their bodies have since been read.
"""

import sys

import addressdb
import peimage

P = "k2se-ghidra"

FINDINGS = [
    # kind, class, name, value, verified_by, notes
    ("vtable", "CSWVirtualMachineCommands", "vtable", 0x009940D0, "runtime",
     "found via the MSVC RTTI string .?AVCSWVirtualMachineCommands@@ at 0x00A0F4F8"),
    ("function", "CSWVirtualMachineCommands", "InitializeCommands", 0x00665F50, "runtime",
     "vtable slot 1; allocates 877*4 and fills the routine table with literal stores"),
    ("function", "CSWVirtualMachineCommands", "ExecuteCommand", 0x00668FD0, "runtime",
     "vtable slot 2; the NWScript routine dispatcher. K2SE hooks this slot"),
    ("function", "CSWVirtualMachineCommands", "InitializeSWMGCommands", 0x006F5B80, "disasm",
     "minigame installer; fills the remaining 103 routine slots"),
    ("offset", "CSWVirtualMachineCommands", "m_pCommands", 0x0C, "runtime",
     "the routine table pointer; byte read at both 0x00665F71 and 0x00668FE7"),
    ("offset", "CVirtualMachine", "m_pInternal", 0x1C, "disasm",
     "every stack accessor forwards through it (mov ecx,[ecx+1Ch] at 0x006FD9B0)"),

    # Routine handlers used for runtime self-validation of the table.
    ("function", "Other", "RoutineHandler_Math", 0x0068C4A0, "runtime",
     "shared handler for routines 67..77 (fabs..abs); K2SE's presence probe rides on it"),
    ("function", "Other", "RoutineHandler_Random", 0x0068F5D0, "runtime",
     "routine 0, Random"),
    ("function", "Other", "RoutineHandler_RebuildPartyTable", 0x0069C460, "runtime",
     "routine 876, the last vanilla routine"),
    ("function", "Other", "RoutineHandler_GetArea", 0x0067A070, "disasm",
     "routine 24; source of the verified StackPopObject call site"),
    ("function", "Other", "RoutineHandler_GetFirstPC", 0x006875E0, "disasm",
     "routine 548; source of StackPushObject and of OBJECT_INVALID = 0x7F000000"),

    # Constants, all read out of instruction immediates.
    ("constant", "CSWVirtualMachineCommands", "VanillaRoutineCount", 0x36D, "runtime",
     "877; the dispatcher's bound at 0x00668FDC and init's at 0x00665F87"),
    ("constant", "CSWVirtualMachineCommands", "RoutineTableAllocBytes", 0xDB4, "runtime",
     "877*4; the `push 0DB4h` at 0x00665F5A, unique in the whole 6.5MB image"),
    ("constant", "Other", "ObjectInvalid", 0x7F000000, "disasm",
     "the engine's own sentinel, from GetFirstPC's default value"),

    # Corrects a real gap: the KOTOR 2 `classes` table is empty upstream, which
    # leaves their own CExoString wrapper unable to size an allocation.
    ("class", "CExoString", "size", 0x08, "disasm",
     "two dwords; confirmed twice -- the default ctor writes only +0x00 and "
     "+0x04, and StackPushString's callee does push 8 / operator new"),

    # Upgrades of imported rows whose bodies K2SE has now read.
    ("function", "CVirtualMachine", "StackPopString", 0x006FDA70, "disasm",
     "copy-assigns into the caller's CExoString, which must be constructed first"),
    ("function", "CVirtualMachine", "StackPushString", 0x006FDA90, "disasm",
     "allocates its own CExoString and copies; the caller retains ownership"),
    ("function", "CVirtualMachine", "StackPopEngineStructure", 0x006FDAB0, "disasm",
     "ret 8: (int type, void** out). Type tag values still unverified"),
    ("function", "CVirtualMachine", "StackPushEngineStructure", 0x006FDAD0, "disasm",
     "ret 8: (int type, void* value). Type tag values still unverified"),
    ("function", "CExoString", "DefaultConstructor", 0x00733540, "disasm",
     "zeroes CStr and Length, nothing else"),
    ("function", "CExoString", "Destructor", 0x00733780, "disasm",
     "frees the game-owned buffer and nulls CStr"),
]

# Free functions and engine globals have no owning class upstream; "Other" is the
# sentinel both projects use.
GL_IMPORTS = [
    ("global", "", "IAT_glFogf", 0x00986394, "disasm", "OPENGL32.dll, by import name"),
    ("global", "", "IAT_glFogfv", 0x00986398, "disasm", "OPENGL32.dll, by import name"),
    ("global", "", "IAT_glFogi", 0x009863B0, "disasm", "OPENGL32.dll, by import name"),
    ("global", "", "IAT_gluPerspective", 0x00986028, "disasm", "GLU32.dll, by import name"),
]


def main():
    img = peimage.open_default(sys.argv[1] if len(sys.argv) > 1 else None)

    rows = []
    for kind, cls, name, value, level, note in FINDINGS + GL_IMPORTS:
        width = 4 if kind in ("offset", "class") else 8
        rows.append({
            "kind": kind, "class": cls, "name": name,
            "value": "0x%0*X" % (width, value),
            "provenance": P, "verified_by": level, "notes": note,
        })

    # Re-check the ones that are supposed to be function entry points.
    bad = 0
    for r in rows:
        if r["kind"] != "function":
            continue
        va = int(r["value"], 16)
        if not img.has_prologue(va):
            print("FAIL  %s::%s @%s has no MSVC prologue" % (r["class"], r["name"], r["value"]))
            bad += 1
    if bad:
        print("\n%d finding(s) failed verification; nothing written." % bad)
        return 1
    print("%d function entry point(s) re-verified against %s"
          % (sum(1 for r in rows if r["kind"] == "function"), img.path))

    merged, added, updated, kept = addressdb.merge(addressdb.load(), rows)
    total = addressdb.save(merged)
    print("merge : %d added, %d updated, %d kept" % (added, updated, kept))
    print("table : %d rows" % total)
    print("\nnext: python tools/gen_offsets.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())

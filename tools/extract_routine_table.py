"""Recover the full 877-entry NWScript routine table from swkotor2.exe.

The table is heap-allocated at runtime, so it does not exist anywhere in the
file -- but the code that FILLS it does. InitializeCommands (0x00665F50) and the
minigame installer (0x006F5B80) populate it with a long run of literal stores:

    mov dword ptr [reg + ID*4], handler        C7 8x <disp32> <imm32>
    mov dword ptr [reg + ID*4], handler        C7 4x <disp8>  <imm32>

Decoding those stores gives the complete ID -> handler map without a debugger.

Usage:
    python tools/extract_routine_table.py [path-to-swkotor2.exe]

Writes k2_routine_table.csv next to this script.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_offsets import Image, DEFAULT_CANDIDATES  # noqa: E402

INIT_COMMANDS = 0x00665F50
INIT_MINIGAME = 0x006F5B80
ROUTINE_COUNT = 877
TABLE_BYTES = ROUTINE_COUNT * 4

# ModRM byte -> register name, for mov dword [reg+disp], imm32 (opcode C7 /0).
#
# Three encodings matter, and missing any one silently loses slots:
#   mod=00  C7 0x        imm32   -> [reg]          (displacement 0, i.e. ID 0)
#   mod=01  C7 4x disp8  imm32   -> [reg+disp8]
#   mod=10  C7 8x disp32 imm32   -> [reg+disp32]
# rm=4 means a SIB byte follows and rm=5 at mod=00 is an absolute address, so
# both are excluded from every map.
NODISP_REGS = {0x00: "eax", 0x01: "ecx", 0x02: "edx", 0x03: "ebx",
               0x06: "esi", 0x07: "edi"}
DISP8_REGS = {0x40: "eax", 0x41: "ecx", 0x42: "edx", 0x43: "ebx",
              0x45: "ebp", 0x46: "esi", 0x47: "edi"}
DISP32_REGS = {0x80: "eax", 0x81: "ecx", 0x82: "edx", 0x83: "ebx",
               0x85: "ebp", 0x86: "esi", 0x87: "edi"}


def scan_stores(img, start_va, limit=0x4000):
    """Decode C7 /0 stores from start_va until the function epilogue."""
    lo, hi = img.text_range()
    start = img.off(start_va)
    if start is None:
        raise SystemExit("0x%08X is not mapped" % start_va)

    stores = []          # (order, index, handler, va)
    order = 0
    i = 0
    data = img.data
    while i < limit:
        p = start + i
        b = data[p]

        # epilogue: mov esp,ebp ; pop ebp ; ret
        if data[p:p + 3] == b"\x8b\xe5\x5d" and data[p + 3] in (0xC3, 0xC2):
            break

        if b == 0xC7:
            modrm = data[p + 1]
            if modrm in NODISP_REGS:
                disp = 0
                imm = struct.unpack_from("<I", data, p + 2)[0]
                size = 6
            elif modrm in DISP8_REGS:
                disp = struct.unpack_from("<b", data, p + 2)[0]
                imm = struct.unpack_from("<I", data, p + 3)[0]
                size = 7
            elif modrm in DISP32_REGS:
                disp = struct.unpack_from("<i", data, p + 2)[0]
                imm = struct.unpack_from("<I", data, p + 6)[0]
                size = 10
            else:
                i += 1
                continue

            if 0 <= disp < TABLE_BYTES and disp % 4 == 0 and lo <= imm < hi:
                stores.append((order, disp // 4, imm, start_va + i))
                order += 1
            i += size
            continue

        i += 1

    return stores


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    if path is None:
        for cand in DEFAULT_CANDIDATES:
            if os.path.exists(cand):
                path = cand
                break
    if not path or not os.path.exists(path):
        raise SystemExit("swkotor2.exe not found; pass the path as an argument")

    img = Image(path)
    lo, hi = img.text_range()
    print("file: %s" % path)
    print(".text: 0x%08X - 0x%08X\n" % (lo, hi))

    main_stores = scan_stores(img, INIT_COMMANDS)
    mini_stores = scan_stores(img, INIT_MINIGAME)
    print("InitializeCommands  @0x%08X : %d stores" % (INIT_COMMANDS, len(main_stores)))
    print("minigame installer  @0x%08X : %d stores" % (INIT_MINIGAME, len(mini_stores)))

    # Last write per index wins: at least one slot is written twice.
    table = {}
    dupes = []
    for _order, idx, handler, _va in main_stores + mini_stores:
        if idx in table and table[idx] != handler:
            dupes.append((idx, table[idx], handler))
        table[idx] = handler

    filled = len(table)
    print("\ndistinct slots filled : %d / %d" % (filled, ROUTINE_COUNT))
    missing = [i for i in range(ROUTINE_COUNT) if i not in table]
    print("missing slots         : %d %s" % (len(missing), missing[:12] if missing else ""))
    print("distinct handlers     : %d" % len(set(table.values())))
    if dupes:
        print("slots written twice   : %s" % ", ".join(
            "%d (0x%08X -> 0x%08X)" % d for d in dupes))

    # Every handler must start with the standard MSVC prologue push ebp; mov ebp,esp.
    bad = []
    for handler in sorted(set(table.values())):
        o = img.off(handler)
        if o is None or img.data[o:o + 3] != b"\x55\x8b\xec":
            bad.append(handler)
    print("handlers NOT starting with 55 8B EC : %d %s"
          % (len(bad), ["0x%08X" % b for b in bad[:6]]))

    print("\nspot checks against DESIGN.md:")
    checks = [
        (0, 0x0068F5D0, "Random"),
        (71, 0x0068C4A0, "acos  (shared math handler)"),
        (77, 0x0068C4A0, "abs   (shared math handler)"),
        (876, 0x0069C460, "RebuildPartyTable"),
    ]
    failures = 0
    for idx, expected, name in checks:
        got = table.get(idx)
        ok = got == expected
        failures += 0 if ok else 1
        print("  %-4s cmds[%3d] = %s  expected 0x%08X  %s"
              % ("OK" if ok else "FAIL", idx,
                 "0x%08X" % got if got else "MISSING", expected, name))

    # The decisive structural evidence: IDs 67..77 are the math group and must
    # all share one handler.
    math_group = sorted(set(table.get(i) for i in range(67, 78)))
    print("\n  IDs 67..77 (fabs..abs) distinct handlers: %d %s"
          % (len(math_group), ["0x%08X" % h for h in math_group if h]))

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "k2_routine_table.csv")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("id,handler\n")
        for i in range(ROUTINE_COUNT):
            h = table.get(i)
            fh.write("%d,%s\n" % (i, "0x%08X" % h if h else ""))
    print("\nwrote %s" % out)

    if filled == ROUTINE_COUNT and not failures and not bad:
        print("\nTABLE FULLY RECOVERED -- 877/877 slots, all spot checks pass.")
        return 0
    print("\nIncomplete or inconsistent recovery -- see above.")
    return 1


if __name__ == "__main__":
    sys.exit(main())

"""Locate the virtual call to ExecuteCommand (vtable slot 2) -- DESIGN.md Q3.

swkotor2.exe is built without optimisation, so a virtual call is not the compact
`call [reg+8]`; it is spelled out:

    mov ecx, [ebp-X]     ; this
    mov edx, [ecx]       ; vptr
    mov eax, [edx+8]     ; slot 2
    call eax

so the giveaway is `mov r32, [r32+8]` immediately followed by `call r32` on the
same register.

Usage:
    python tools/find_callers2.py
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from peimage import Image, DEFAULT_CANDIDATES  # noqa: E402

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
except ImportError:
    raise SystemExit("pip install --user capstone")

REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
ROUTINE_COUNT = 877


def main():
    path = None
    for cand in DEFAULT_CANDIDATES:
        if os.path.exists(cand):
            path = cand
            break
    img = Image(path)
    lo, hi = img.text_range()
    text_off = img.off(lo)
    data = img.data[text_off:text_off + (hi - lo)]
    print("file: %s\n.text 0x%08X-0x%08X (%d bytes)\n" % (path, lo, hi, len(data)))

    # mov reg, [base+8] : 8B <modrm mod=01, disp8=08>
    # then call reg     : FF <D0|reg>
    hits = []
    for i in range(len(data) - 6):
        if data[i] != 0x8B:
            continue
        modrm = data[i + 1]
        if (modrm & 0xC0) != 0x40:      # mod must be 01 (disp8)
            continue
        rm = modrm & 0x07
        if rm == 4:                      # SIB follows, skip
            continue
        if data[i + 2] != 0x08:          # disp8 == 8 -> vtable slot 2
            continue
        reg = (modrm >> 3) & 0x07
        if data[i + 3] == 0xFF and data[i + 4] == (0xD0 | reg):
            hits.append((lo + i, REGS[reg], REGS[rm]))

    print("=== `mov %%r,[%%b+8]` + `call %%r` sites: %d ===" % len(hits))

    md = Cs(CS_ARCH_X86, CS_MODE_32)

    # Rank: the dispatch site pushes two arguments (nParams, nCommandId) and
    # loads its object from somewhere. Show a generous window for each.
    for va, reg, base in hits:
        start = va - 56
        off = img.off(start)
        if off is None:
            continue
        window = img.data[off:off + 56 + 10]

        has_count = (struct.pack("<I", ROUTINE_COUNT) in window
                     or struct.pack("<H", ROUTINE_COUNT) in window)

        print("\n--- 0x%08X   mov %s,[%s+8]; call %s%s"
              % (va, reg, base, reg, "    <== 877 NEARBY" if has_count else ""))
        for ins in md.disasm(window, start):
            mark = "  <== VIRTUAL CALL" if ins.address == va + 3 else ""
            print("    0x%08X  %-18s %s%s"
                  % (ins.address, ins.bytes.hex(" "), "%s %s" % (ins.mnemonic, ins.op_str), mark))
            if ins.address > va + 3:
                break

    if not hits:
        print("none found -- the dispatch must use yet another form")


if __name__ == "__main__":
    main()

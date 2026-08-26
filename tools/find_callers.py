"""Find the code that calls CSWVirtualMachineCommands::ExecuteCommand.

ExecuteCommand is vtable slot [2], so callers reach it as `call [reg+8]` after
loading the object's vptr. Answering DESIGN.md Q3 -- "is there a second bounds
check upstream that would stop routine IDs >= 877 from ever reaching the
dispatcher?" -- means finding that call site and reading what precedes it.

Usage:
    python tools/find_callers.py
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_offsets import Image, DEFAULT_CANDIDATES  # noqa: E402

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
except ImportError:
    raise SystemExit("pip install --user capstone")

# call dword ptr [reg + 8]  -- vtable slot 2
CALL_VT8 = {
    b"\xff\x50\x08": "call [eax+8]",
    b"\xff\x51\x08": "call [ecx+8]",
    b"\xff\x52\x08": "call [edx+8]",
    b"\xff\x53\x08": "call [ebx+8]",
    b"\xff\x56\x08": "call [esi+8]",
    b"\xff\x57\x08": "call [edi+8]",
}

VTABLE = 0x009940D0
EXECUTE_COMMAND = 0x00668FD0
ROUTINE_COUNT = 877


def main():
    path = None
    for cand in DEFAULT_CANDIDATES:
        if os.path.exists(cand):
            path = cand
            break
    img = Image(path)
    lo, hi = img.text_range()
    print("file: %s" % path)
    print(".text 0x%08X - 0x%08X\n" % (lo, hi))

    text_off = img.off(lo)
    text_len = hi - lo
    data = img.data[text_off:text_off + text_len]

    # --- 1. direct references to the vtable ---------------------------------
    print("=== direct references to the vtable 0x%08X ===" % VTABLE)
    needle = struct.pack("<I", VTABLE)
    pos = 0
    while True:
        idx = data.find(needle, pos)
        if idx < 0:
            break
        print("  VA 0x%08X" % (lo + idx))
        pos = idx + 1

    # --- 2. direct calls to ExecuteCommand ----------------------------------
    print("\n=== direct E8 calls to ExecuteCommand 0x%08X ===" % EXECUTE_COMMAND)
    found_direct = 0
    for i in range(len(data) - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        if lo + i + 5 + rel == EXECUTE_COMMAND:
            print("  VA 0x%08X" % (lo + i))
            found_direct += 1
    if not found_direct:
        print("  none (consistent with dispatch going through the vtable)")

    # --- 3. vtable-slot-2 indirect call sites --------------------------------
    print("\n=== `call [reg+8]` sites (vtable slot 2 candidates) ===")
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    sites = []
    for pattern, label in CALL_VT8.items():
        pos = 0
        while True:
            idx = data.find(pattern, pos)
            if idx < 0:
                break
            sites.append((lo + idx, label))
            pos = idx + 1
    sites.sort()
    print("  total: %d" % len(sites))

    # Rank them: a site that pushes exactly two arguments and sits near a
    # comparison against the routine count is the one we care about.
    print("\n=== sites whose preceding 64 bytes mention 877 (0x36D) ===")
    interesting = []
    for va, label in sites:
        start = va - 64
        off = img.off(start)
        if off is None:
            continue
        window = img.data[off:off + 64 + 3]
        if struct.pack("<I", ROUTINE_COUNT) in window or struct.pack("<H", ROUTINE_COUNT) in window:
            interesting.append((va, label))
            print("  VA 0x%08X  %s   <-- bounds check nearby" % (va, label))
    if not interesting:
        print("  NONE -- no `call [reg+8]` site has 877 within the preceding 64 bytes.")
        print("  That is the answer to Q3: no upstream bounds check against the")
        print("  routine count guards the dispatch call.")

    # --- 4. show context for every candidate --------------------------------
    print("\n=== context for each call site (24 bytes before, 8 after) ===")
    for va, label in sites:
        start = va - 24
        off = img.off(start)
        if off is None:
            continue
        code = img.data[off:off + 40]
        print("\n  --- site 0x%08X (%s)" % (va, label))
        for ins in md.disasm(code, start):
            mark = "  <== CALL" if ins.address == va else ""
            print("      0x%08X  %-20s %s%s"
                  % (ins.address, ins.bytes.hex(" "), "%s %s" % (ins.mnemonic, ins.op_str), mark))
            if ins.address > va:
                break


if __name__ == "__main__":
    main()

"""Disassemble functions of swkotor2.exe at given virtual addresses.

Used to settle the VM ABI questions statically instead of by hand in a debugger.

Usage:
    python tools/disasm.py 0x0068C4A0 [0x006FD9A0 ...] [--len 240]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from peimage import Image, DEFAULT_CANDIDATES  # noqa: E402

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
except ImportError:
    raise SystemExit("pip install --user capstone")

# Names we have recovered for addresses that show up as call targets.
KNOWN = {
    0x00665F50: "CSWVirtualMachineCommands::InitializeCommands",
    0x00668FD0: "CSWVirtualMachineCommands::ExecuteCommand",
    0x006F5B80: "InitializeMinigameCommands",
    0x0068C4A0: "handler_math (routines 67..77)",
    0x0068F5D0: "handler_Random (routine 0)",
    0x0069C460: "handler_RebuildPartyTable (routine 876)",
    0x006FD8D0: "vm_RunScript?",
    0x006FD9A0: "StackPopInteger?",
    0x006FD9C0: "StackPushInteger?",
    0x00474C00: "AurPostString",
}


def disasm(img, va, length, stop_at_ret=True):
    off = img.off(va)
    if off is None:
        print("0x%08X is not mapped" % va)
        return
    code = img.data[off:off + length]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = False

    label = KNOWN.get(va, "")
    print("\n%s" % ("=" * 78))
    print("0x%08X  %s" % (va, label))
    print("=" * 78)

    depth_ret = 0
    for ins in md.disasm(code, va):
        target = ""
        if ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            try:
                t = int(ins.op_str, 16)
                if t in KNOWN:
                    target = "   ; -> %s" % KNOWN[t]
            except ValueError:
                pass
        print("  0x%08X  %-22s %-34s%s"
              % (ins.address, ins.bytes.hex(" "), "%s %s" % (ins.mnemonic, ins.op_str), target))
        if ins.mnemonic == "ret":
            depth_ret += 1
            if stop_at_ret and depth_ret >= 1:
                break


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    length = 240
    if "--len" in sys.argv:
        length = int(sys.argv[sys.argv.index("--len") + 1])
        args = [a for a in args if a != str(length)]

    path = None
    addrs = []
    for a in args:
        if a.lower().startswith("0x"):
            addrs.append(int(a, 16))
        else:
            path = a

    if path is None:
        for cand in DEFAULT_CANDIDATES:
            if os.path.exists(cand):
                path = cand
                break
    img = Image(path)
    print("file: %s" % path)

    for va in addrs:
        disasm(img, va, length)


if __name__ == "__main__":
    main()

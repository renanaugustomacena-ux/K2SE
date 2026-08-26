"""Re-verify every address K2SE depends on, straight out of the binary.

This is milestone M0: nothing in DESIGN.md should be taken on faith. Run it
before trusting a single line of hook code.

Usage:
    python tools/verify_offsets.py [path-to-swkotor2.exe]

Defaults to the pristine backup, falling back to the live exe.
"""

import os
import struct
import sys

DEFAULT_GAME = r"G:\SteamLibrary\steamapps\common\Knights of the Old Republic II"
DEFAULT_CANDIDATES = [
    os.path.join(DEFAULT_GAME, "swkotor2.exe.pre-laa-backup"),
    os.path.join(DEFAULT_GAME, "swkotor2.exe"),
]

EXPECTED_TIMESTAMP = 0x5603005D
EXPECTED_IMAGEBASE = 0x00400000


class Image(object):
    def __init__(self, path):
        with open(path, "rb") as fh:
            self.data = fh.read()
        self.path = path
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        self.pe = pe
        self.num_sections = struct.unpack_from("<H", self.data, pe + 6)[0]
        self.timestamp = struct.unpack_from("<I", self.data, pe + 8)[0]
        self.opt_size = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.characteristics = struct.unpack_from("<H", self.data, pe + 22)[0]
        self.image_base = struct.unpack_from("<I", self.data, pe + 24 + 28)[0]

        self.sections = []
        sec = pe + 24 + self.opt_size
        for i in range(self.num_sections):
            o = sec + i * 40
            name = self.data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr = struct.unpack_from("<II", self.data, o + 8)
            rawsize, raw = struct.unpack_from("<II", self.data, o + 16)
            self.sections.append((name, vaddr, vsize, raw, rawsize))

    def off(self, va):
        rva = va - self.image_base
        for _n, vaddr, vsize, raw, _rs in self.sections:
            if vaddr <= rva < vaddr + vsize:
                return raw + (rva - vaddr)
        return None

    def dword(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack_from("<I", self.data, o)[0]

    def byte(self, va):
        o = self.off(va)
        return None if o is None else self.data[o]

    def text_range(self):
        for name, vaddr, vsize, _raw, _rs in self.sections:
            if name == ".text":
                return (self.image_base + vaddr, self.image_base + vaddr + vsize)
        return (0, 0)


PROBES_DWORD = [
    ("vtable[0]",                 0x009940D0, 0x00536BE0),
    ("vtable[1] InitializeCmds",  0x009940D4, 0x00665F50),
    ("vtable[2] ExecuteCommand",  0x009940D8, 0x00668FD0),
    ("vtable[3]",                 0x009940DC, 0x00669020),
    ("alloc size 877*4",          0x00665F5A, 0x0DB4),
    ("init bound 877",            0x00665F87, 0x36D),
    ("dispatch bound 877",        0x00668FDC, 0x36D),
]

PROBES_BYTE = [
    ("m_pCommands (init)",     0x00665F71, 0x0C),
    ("m_pCommands (dispatch)", 0x00668FE7, 0x0C),
    ("m_pInternal",            0x006FD9B0, 0x1C),
]

RTTI_STRING = b".?AVCSWVirtualMachineCommands@@"
RTTI_VA = 0x00A0F4F8


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
    print("file        : %s" % path)
    print("size        : %d bytes" % len(img.data))
    print("ImageBase   : 0x%08X  (expected 0x%08X)" % (img.image_base, EXPECTED_IMAGEBASE))
    print("TimeDateStamp: 0x%08X (expected 0x%08X)" % (img.timestamp, EXPECTED_TIMESTAMP))
    laa = " [4GB/LAA patched]" if img.characteristics & 0x20 else " [pristine]"
    print("Characteristics: 0x%04X%s" % (img.characteristics, laa))
    lo, hi = img.text_range()
    print(".text range : 0x%08X - 0x%08X" % (lo, hi))
    print("")

    failures = 0

    if img.timestamp != EXPECTED_TIMESTAMP:
        print("FAIL  TimeDateStamp mismatch -- this is a different build")
        failures += 1

    o = img.off(RTTI_VA)
    got = img.data[o:o + len(RTTI_STRING)] if o is not None else b""
    ok = got == RTTI_STRING
    print("%-4s  RTTI class name @0x%08X -> %s" % ("OK" if ok else "FAIL", RTTI_VA,
                                                   got.decode("ascii", "replace")))
    failures += 0 if ok else 1

    for name, va, expected in PROBES_DWORD:
        actual = img.dword(va)
        ok = actual == expected
        failures += 0 if ok else 1
        shown = "unreadable" if actual is None else "0x%08X" % actual
        print("%-4s  %-26s @0x%08X = %-12s expected 0x%08X"
              % ("OK" if ok else "FAIL", name, va, shown, expected))

    for name, va, expected in PROBES_BYTE:
        actual = img.byte(va)
        ok = actual == expected
        failures += 0 if ok else 1
        shown = "unreadable" if actual is None else "0x%02X" % actual
        print("%-4s  %-26s @0x%08X = %-12s expected 0x%02X"
              % ("OK" if ok else "FAIL", name, va, shown, expected))

    print("")
    if failures:
        print("%d PROBE(S) FAILED -- do not hook this binary." % failures)
        return 1
    print("ALL PROBES PASSED -- the addresses in src/offsets.h match this binary.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Find ASCII strings in swkotor2.exe and the code that references them (imm32 xrefs)."""
import os, struct, sys
sys.path.insert(0, r"c:\Users\Renan Macena\Documents\KOTOR2-Modding\K2SE\tools")
from peimage import Image, DEFAULT_CANDIDATES
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

img = Image([c for c in DEFAULT_CANDIDATES if os.path.exists(c)][0])
lo, hi = img.text_range()
text = img.data[img.off(lo):img.off(lo) + (hi - lo)]
md = Cs(CS_ARCH_X86, CS_MODE_32)

def va_of_offset(off):
    for name, vaddr, vsize, raw, rawsize in img.sections:
        if raw <= off < raw + rawsize:
            return img.image_base + vaddr + (off - raw)
    return None

def find_strings(s):
    needle = s.encode("ascii") + b"\x00"
    out = []
    pos = 0
    while True:
        i = img.data.find(needle, pos)
        if i < 0: break
        # require start-of-string (preceded by NUL or non-printable)
        if i == 0 or img.data[i-1] == 0 or not (32 <= img.data[i-1] < 127):
            va = va_of_offset(i)
            if va: out.append(va)
        pos = i + 1
    return out

def xrefs(va):
    needle = struct.pack("<I", va)
    hits = []
    pos = 0
    while True:
        i = text.find(needle, pos)
        if i < 0: break
        hits.append(lo + i)
        pos = i + 1
    return hits

def context(site, before=40, after=16):
    start = site - before
    off = img.off(start)
    code = img.data[off:off + before + after]
    lines = []
    for ins in md.disasm(code, start):
        mark = "  <==" if ins.address <= site < ins.address + ins.size else ""
        lines.append("      0x%08X  %-18s %s %s%s" % (ins.address, ins.bytes.hex(" "), ins.mnemonic, ins.op_str, mark))
        if ins.address > site + 12: break
    return "\n".join(lines)

for s in sys.argv[1:]:
    vas = find_strings(s)
    print("\n##### %r : %d string instance(s)" % (s, len(vas)))
    for va in vas:
        xs = xrefs(va)
        print("  string @0x%08X  xrefs: %d -> %s" % (va, len(xs), ", ".join("0x%08X" % x for x in xs[:12])))
        for x in xs[:4]:
            print(context(x))

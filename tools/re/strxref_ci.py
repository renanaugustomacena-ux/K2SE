"""Case-insensitive ASCII string search in swkotor2.exe + imm32 xrefs from .text (with disassembly context)."""
import os, re, struct, sys
sys.path.insert(0, r"c:\Users\Renan Macena\Documents\KOTOR2-Modding\K2SE\tools")
from peimage import Image, DEFAULT_CANDIDATES
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
img = Image([c for c in DEFAULT_CANDIDATES if os.path.exists(c)][0])
lo, hi = img.text_range()
text = img.data[img.off(lo):img.off(lo) + (hi - lo)]
md = Cs(CS_ARCH_X86, CS_MODE_32)
low = img.data.lower()
def va_of_offset(off):
    for name, vaddr, vsize, raw, rawsize in img.sections:
        if raw <= off < raw + rawsize:
            return img.image_base + vaddr + (off - raw)
def xrefs(va):
    needle = struct.pack("<I", va); hits=[]; pos=0
    while True:
        i = text.find(needle, pos)
        if i < 0: break
        hits.append(lo+i); pos=i+1
    return hits
def context(site, before=24, after=12):
    start=site-before; off=img.off(start); code=img.data[off:off+before+after]; out=[]
    for ins in md.disasm(code, start):
        mark = "  <==" if ins.address <= site < ins.address+ins.size else ""
        out.append("      0x%08X  %-16s %s %s%s" % (ins.address, ins.bytes.hex(" "), ins.mnemonic, ins.op_str, mark))
        if ins.address > site+8: break
    return "\n".join(out)
maxctx = int(os.environ.get("MAXCTX", "3"))
for s in sys.argv[1:]:
    needle = s.lower().encode()
    print("\n##### %r" % s)
    pos=0; n=0
    while True:
        i = low.find(needle, pos)
        if i < 0: break
        pos = i+1
        # whole-string boundaries: preceded by NUL/nonprint, followed by NUL
        end = i+len(needle)
        if end < len(img.data) and img.data[end] != 0: 
            # allow suffix strings (e.g. 'perspace' inside 'creperspace') only when exact
            continue
        if i > 0 and 32 <= img.data[i-1] < 127: continue
        va = va_of_offset(i); 
        if not va: continue
        actual = img.data[i:end].decode("ascii","replace")
        xs = xrefs(va); n+=1
        print("  %r @0x%08X  xrefs %d: %s" % (actual, va, len(xs), ", ".join("0x%08X"%x for x in xs[:10])))
        for x in xs[:maxctx]: print(context(x))
    if n == 0: print("  (none)")

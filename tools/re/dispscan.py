"""Linear sweep of .text for instructions touching [reg+DISP] (optionally 16-bit ops only)."""
import os, sys, re
sys.path.insert(0, r"c:\Users\Renan Macena\Documents\KOTOR2-Modding\K2SE\tools")
from peimage import Image, DEFAULT_CANDIDATES
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
img = Image([c for c in DEFAULT_CANDIDATES if os.path.exists(c)][0])
lo, hi = img.text_range(); code = img.data[img.off(lo):img.off(lo)+(hi-lo)]
md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
disps = [int(a,16) for a in sys.argv[1:] if a.startswith("0x")]
only16 = "--word" in sys.argv
pat = re.compile(r"\[(e[a-d]x|esi|edi|ebp) \+ (0x[0-9a-f]+)\]")
hits = 0
for ins in md.disasm(code, lo):
    m = pat.search(ins.op_str)
    if not m: continue
    if int(m.group(2),16) not in disps: continue
    if only16 and "word ptr" not in ins.op_str: continue
    if only16 and "dword" in ins.op_str: continue
    print("0x%08X  %-18s %s %s" % (ins.address, ins.bytes.hex(" "), ins.mnemonic, ins.op_str)); hits += 1
    if hits > 400: print("...capped"); break
print("hits:", hits)

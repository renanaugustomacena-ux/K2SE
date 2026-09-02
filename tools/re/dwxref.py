"""Find .text references (imm32) to arbitrary VAs (globals) in swkotor2.exe, with disassembly context."""
import os, struct, sys
sys.path.insert(0, r"c:\Users\Renan Macena\Documents\KOTOR2-Modding\K2SE\tools")
from peimage import Image, DEFAULT_CANDIDATES
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
img = Image([c for c in DEFAULT_CANDIDATES if os.path.exists(c)][0])
lo, hi = img.text_range(); text = img.data[img.off(lo):img.off(lo)+(hi-lo)]
md = Cs(CS_ARCH_X86, CS_MODE_32)
ctxn = int(os.environ.get("CTX","6")); before=int(os.environ.get("BEFORE","30")); after=int(os.environ.get("AFTER","14"))
def xrefs(va):
    needle=struct.pack("<I",va); hits=[]; pos=0
    while True:
        i=text.find(needle,pos)
        if i<0: break
        hits.append(lo+i); pos=i+1
    return hits
def context(site):
    start=site-before; off=img.off(start); code=img.data[off:off+before+after]; out=[]
    for ins in md.disasm(code,start):
        mark="  <==" if ins.address<=site<ins.address+ins.size else ""
        out.append("      0x%08X  %-16s %s %s%s"%(ins.address,ins.bytes.hex(" "),ins.mnemonic,ins.op_str,mark))
        if ins.address>site+8: break
    return "\n".join(out)
for a in sys.argv[1:]:
    va=int(a,16); xs=xrefs(va)
    print("\n##### 0x%08X : %d xrefs -> %s"%(va,len(xs),", ".join("0x%08X"%x for x in xs[:24])))
    for x in xs[:ctxn]: print(context(x)); print()

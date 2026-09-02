"""Disassemble a whole function (until the end VA) to a file and print a summary: calls, this-offsets, constants."""
import os, re, struct, sys
from collections import Counter, defaultdict
sys.path.insert(0, r"c:\Users\Renan Macena\Documents\KOTOR2-Modding\K2SE\tools")
from peimage import Image, DEFAULT_CANDIDATES
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
img = Image([c for c in DEFAULT_CANDIDATES if os.path.exists(c)][0])
md = Cs(CS_ARCH_X86, CS_MODE_32)
start = int(sys.argv[1], 16); end = int(sys.argv[2], 16); outp = sys.argv[3]
code = img.data[img.off(start):img.off(start) + (end - start)]
lines=[]; calls=Counter(); offs=defaultdict(Counter); consts=Counter(); globals_=Counter()
for ins in md.disasm(code, start):
    lines.append("0x%08X  %-20s %s %s" % (ins.address, ins.bytes.hex(" "), ins.mnemonic, ins.op_str))
    if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
        calls[int(ins.op_str,16)] += 1
    for m in re.finditer(r"\[(e[a-d]x|esi|edi|ebp|esp) ([+-]) (0x[0-9a-f]+|\d+)\]", ins.op_str):
        reg, sign, val = m.groups(); v=int(val,0)
        if reg in ("ebp","esp"): continue
        offs[reg]["%s0x%X" % (sign if sign=="-" else "+", v)] += 1
    for m in re.finditer(r"ptr \[(0x[0-9a-f]+)\]", ins.op_str):
        a=int(m.group(1),16)
        if ins.mnemonic.startswith("f"): consts[a]+=1
        else: globals_[a]+=1
open(outp,"w").write("\n".join(lines))
print("instructions: %d  written to %s" % (len(lines), outp))
print("\n== calls (target: count) ==")
for t,c in sorted(calls.items()): print("  0x%08X x%d" % (t,c))
print("\n== float constants (addr: value) ==")
for a,c in sorted(consts.items()):
    o = img.off(a)
    d = struct.unpack_from("<d", img.data, o)[0] if o else None
    f = struct.unpack_from("<f", img.data, o)[0] if o else None
    print("  0x%08X x%d  as double=%r  as float=%r" % (a,c,d,f))
print("\n== globals referenced ==")
for a,c in sorted(globals_.items()): print("  0x%08X x%d" % (a,c))
print("\n== register-relative offsets (reg: offsets) ==")
for r in offs: print("  %s: %s" % (r, ", ".join("%s x%d"%(k,v) for k,v in sorted(offs[r].items(), key=lambda kv: int(kv[0].replace('+','').replace('-','-'),16)))))

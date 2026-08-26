"""DESIGN.md Q1 and Q2 -- does nwnnsscomp cooperate with an extended header?

Q2 (do this first, it gates Q1): routine 767 in the shipped TSL nwscript.nss is
declared with types the compiler may not accept. If nwnnsscomp silently SKIPS a
malformed declaration, every routine after it shifts down by one and the whole
ID space we are building on is off by one. Test: compile a call to a routine
whose ID we know independently and check the emitted ACTION.

Q1: append a prototype past the last vanilla routine and check that a call to it
compiles to ACTION with that ID.

ACTION encoding, verified from the interpreter at 0x00700212:
    byte 0   opcode 0x05
    byte 1   type   0x00
    byte 2-3 routine id, BIG ENDIAN
    byte 4   argument count
(5 bytes total; the interpreter does pc += 5)

Usage:
    python tools/q1_q2_compiler_test.py
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
COMPILER_DIR = os.path.join(HERE, "nwnnsscomp")
COMPILER = os.path.join(COMPILER_DIR, "nwnnsscomp.exe")
HEADER = os.path.join(COMPILER_DIR, "nwscript.nss")

ACTION_RE = re.compile(rb"\x05\x00(..)(.)", re.S)


def parse_routines(header_text):
    """Every non-comment prototype in nwscript.nss, in declaration order.

    nwnnsscomp assigns routine IDs positionally, so index == routine id.
    """
    # strip block comments, keep line comments out of the way
    text = re.sub(r"/\*.*?\*/", "", header_text, flags=re.S)
    lines = []
    for raw in text.splitlines():
        line = re.sub(r"//.*$", "", raw)
        lines.append(line)
    joined = "\n".join(lines)

    protos = []
    # a prototype is `type name(args);` at top level
    for m in re.finditer(r"(?m)^\s*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^;{]*)\)\s*;",
                         joined):
        rettype, name, args = m.group(1), m.group(2), m.group(3)
        if rettype in ("return", "else", "if", "while", "for"):
            continue
        protos.append((name, rettype, args.strip()))
    return protos


def find_actions(ncs_bytes):
    """All ACTION instructions in a compiled .ncs, as (offset, id, argc)."""
    out = []
    i = 13  # skip "NCS V1.0" + size header
    data = ncs_bytes
    while i < len(data) - 4:
        if data[i] == 0x05 and data[i + 1] == 0x00:
            rid = struct.unpack_from(">H", data, i + 2)[0]
            argc = data[i + 4]
            out.append((i, rid, argc))
            i += 5
        else:
            i += 1
    return out


def compile_script(src_text, header_path, workdir, name="test"):
    """Run nwnnsscomp with the given header; return (ok, ncs_bytes, output)."""
    # nwnnsscomp resolves nwscript.nss from its own directory
    shutil.copy(header_path, os.path.join(workdir, "nwscript.nss"))
    shutil.copy(COMPILER, os.path.join(workdir, "nwnnsscomp.exe"))

    nss = os.path.join(workdir, name + ".nss")
    ncs = os.path.join(workdir, name + ".ncs")
    with open(nss, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(src_text)

    proc = subprocess.run(
        [os.path.join(workdir, "nwnnsscomp.exe"), "-c", nss, "-o", ncs],
        capture_output=True, cwd=workdir, timeout=120)
    output = (proc.stdout + proc.stderr).decode("utf-8", "replace").strip()

    if os.path.exists(ncs):
        with open(ncs, "rb") as fh:
            return True, fh.read(), output
    return False, b"", output


def main():
    if not os.path.exists(COMPILER):
        raise SystemExit("nwnnsscomp.exe not found at %s" % COMPILER)

    header = open(HEADER, encoding="utf-8", errors="replace").read()
    protos = parse_routines(header)
    print("prototypes parsed from nwscript.nss: %d" % len(protos))
    print("  first : %-28s (would be id 0)" % protos[0][0])
    print("  last  : %-28s (would be id %d)" % (protos[-1][0], len(protos) - 1))
    print("\n  ids 765..772 by position:")
    for i in range(765, min(773, len(protos))):
        print("    %3d  %s %s(%s)" % (i, protos[i][1], protos[i][0], protos[i][2][:46]))

    # ------------------------------------------------------------------ Q2
    print("\n" + "=" * 76)
    print("Q2 -- does the compiler renumber because of a malformed declaration?")
    print("=" * 76)

    # pick a routine with a known-independent id to use as the yardstick
    probe_name = None
    for want in ("GetItemComponent", "GetChemicals", "GetSpellForcePointCost"):
        for idx, (nm, _rt, _ar) in enumerate(protos):
            if nm == want:
                probe_name = (want, idx)
                break
        if probe_name:
            break

    workdir = tempfile.mkdtemp(prefix="k2se_q2_")
    src = "void main() {\n    int n = %s();\n}\n" % probe_name[0]
    ok, ncs, out = compile_script(src, HEADER, workdir, "q2")
    print("compile `%s()`  ->  %s" % (probe_name[0], "OK" if ok else "FAILED"))
    if out:
        print("compiler said: %s" % out.replace("\n", " | ")[:300])
    if ok:
        acts = find_actions(ncs)
        print("ACTIONs emitted: %s" % ["id=%d argc=%d" % (a[1], a[2]) for a in acts])
        if acts:
            emitted = acts[0][1]
            expected = probe_name[1]
            verdict = "MATCH" if emitted == expected else "MISMATCH -- renumbering!"
            print("emitted id %d, header position %d  ->  %s" % (emitted, expected, verdict))
            print("raw bytes: %s" % ncs[acts[0][0]:acts[0][0] + 5].hex(" "))

    # ------------------------------------------------------------------ Q1
    print("\n" + "=" * 76)
    print("Q1 -- does an extended header past the last vanilla routine compile?")
    print("=" * 76)

    next_id = len(protos)
    extended = header.rstrip() + "\n\n// ---- K2SE extended routines ----\n" \
                                 "// %d: K2SE_GetVersion\nint K2SE_GetVersion();\n" % next_id
    ext_path = os.path.join(tempfile.mkdtemp(prefix="k2se_hdr_"), "nwscript.nss")
    with open(ext_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(extended)
    print("extended header written, new routine would be id %d" % next_id)

    workdir = tempfile.mkdtemp(prefix="k2se_q1_")
    src = "void main() {\n    int v = K2SE_GetVersion();\n}\n"
    ok, ncs, out = compile_script(src, ext_path, workdir, "q1")
    print("compile `K2SE_GetVersion()`  ->  %s" % ("OK" if ok else "FAILED"))
    if out:
        print("compiler said: %s" % out.replace("\n", " | ")[:300])
    if ok:
        acts = find_actions(ncs)
        print("ACTIONs emitted: %s" % ["id=%d argc=%d" % (a[1], a[2]) for a in acts])
        for off, rid, argc in acts:
            if rid == next_id:
                raw = ncs[off:off + 5]
                print("\nFOUND the extended call:")
                print("  bytes    : %s" % raw.hex(" "))
                print("  opcode   : 0x%02X (ACTION)" % raw[0])
                print("  type     : 0x%02X" % raw[1])
                print("  routine  : %d (0x%04X, big endian)" % (rid, rid))
                print("  argc     : %d" % argc)
                expected = bytes([0x05, 0x00]) + struct.pack(">H", next_id) + bytes([0])
                print("  expected : %s  -> %s"
                      % (expected.hex(" "), "MATCH" if raw == expected else "DIFFERENT"))
                break
        else:
            print("no ACTION with id %d found -- the compiler resolved it elsewhere" % next_id)

    print("\nwork dirs kept for inspection:\n  %s" % workdir)


if __name__ == "__main__":
    main()

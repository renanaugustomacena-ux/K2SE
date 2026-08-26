"""DESIGN.md Q7 -- what does nwnnsscomp emit for omitted default arguments?

Two possible worlds:
  A) the compiler pushes the declared default values itself and emits the FULL
     argc -> handlers always see the declared parameter count, defaults are a
     pure compile-time affair, and K2SE routines may freely declare defaults;
  B) the compiler emits a SHORT argc -> handlers must inspect nParams and
     supply defaults at runtime, and every K2SE handler with defaults needs
     explicit argc branching.

We test three shapes:
  1. a vanilla routine with every argument defaulted, called empty-handed;
  2. the same routine with the first argument given explicitly;
  3. a routine appended to the extended header with a trailing default,
     called with and without it.

Usage:
    python tools/q7_default_args_test.py
"""

import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from q1_q2_compiler_test import (  # noqa: E402
    HEADER, COMPILER, parse_routines, find_actions, compile_script)


def show(tag, src, header_path, expect_id):
    workdir = tempfile.mkdtemp(prefix="k2se_q7_")
    ok, ncs, out = compile_script(src, header_path, workdir, "q7")
    print("\n--- %s" % tag)
    print("    source : %s" % src.replace("\n", " ").strip())
    if not ok:
        print("    COMPILE FAILED: %s" % out.replace("\n", " | ")[:240])
        return None
    acts = find_actions(ncs)
    hit = next((a for a in acts if a[1] == expect_id), None)
    if hit is None:
        print("    no ACTION with id %d (all: %s)" % (expect_id, acts))
        return None
    off, rid, argc = hit
    print("    ACTION : %s   id=%d argc=%d" % (ncs[off:off + 5].hex(" "), rid, argc))
    # what the compiler emitted just before the ACTION is the argument setup
    lead = ncs[max(13, off - 24):off]
    print("    24 bytes before ACTION: %s" % lead.hex(" "))
    return argc


def main():
    if not os.path.exists(COMPILER):
        raise SystemExit("nwnnsscomp.exe not found at %s" % COMPILER)

    header = open(HEADER, encoding="utf-8", errors="replace").read()
    protos = parse_routines(header)

    name, idx, args = None, None, None
    for want in ("GetIsInCombat",):
        for i, (nm, _rt, ar) in enumerate(protos):
            if nm == want:
                name, idx, args = nm, i, ar
                break
    print("vanilla probe: %s = id %d" % (name, idx))
    print("declared as : (%s)" % args)

    argc_empty = show("Q7a: %s() with ALL defaults omitted" % name,
                      "void main() {\n    int n = %s();\n}\n" % name,
                      HEADER, idx)
    argc_one = show("Q7b: %s(OBJECT_SELF) with trailing default omitted" % name,
                    "void main() {\n    int n = %s(OBJECT_SELF);\n}\n" % name,
                    HEADER, idx)

    # ------------------------------------------------ extended-header variant
    ext_id = len(protos)
    extended = header.rstrip() + (
        "\n\n// ---- K2SE Q7 probe ----\n"
        "int K2SE_Q7Probe(int nA, int nB = 7);\n")
    ext_path = os.path.join(tempfile.mkdtemp(prefix="k2se_q7hdr_"), "nwscript.nss")
    with open(ext_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(extended)

    argc_ext_full = show("Q7c: extended K2SE_Q7Probe(1, 2) -- both given",
                         "void main() {\n    int n = K2SE_Q7Probe(1, 2);\n}\n",
                         ext_path, ext_id)
    argc_ext_short = show("Q7d: extended K2SE_Q7Probe(1) -- default omitted",
                          "void main() {\n    int n = K2SE_Q7Probe(1);\n}\n",
                          ext_path, ext_id)

    print("\n" + "=" * 76)
    if argc_empty is None or argc_ext_short is None:
        print("Q7 INCONCLUSIVE -- see failures above")
        return
    total = len([a for a in args.split(",") if a.strip()])
    if argc_empty == total and argc_ext_short == 2:
        print("Q7 ANSWER: world A -- the compiler materializes omitted defaults")
        print("and always emits the full declared argc. K2SE routines may declare")
        print("defaults; handlers will always see the full parameter count.")
    elif argc_empty < total or argc_ext_short < 2:
        print("Q7 ANSWER: world B -- the compiler emits a SHORT argc (%d/%d, ext %d/2)."
              % (argc_empty, total, argc_ext_short))
        print("K2SE handlers with defaults MUST branch on nParams and supply the")
        print("missing values at runtime, exactly like the engine's own handlers.")
    else:
        print("Q7 UNEXPECTED combination: vanilla argc=%s/%d, extended argc=%s/2"
              % (argc_empty, total, argc_ext_short))


if __name__ == "__main__":
    main()

"""M4 -- get a compiled script that calls routine 877 to run inside the game.

Strategy: WRAP, don't replace. `k_def_spawn01` is the default creature spawn
script and fires constantly, so it is an easy trigger -- but replacing it
outright would break creature setup. Instead:

    override/k2se_orig_sp.ncs   <- TSLRCM's original, copied under a new name
    override/k_def_spawn01.ncs  <- our wrapper: runs the original first,
                                   then calls the new routine

so the game behaves exactly as before and we get a call site. Both files live in
the game's override folder and deleting them restores everything.

    python tools/m4_deploy_test.py           build + install
    python tools/m4_deploy_test.py --clean   remove everything we added
"""

import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from q1_q2_compiler_test import compile_script, find_actions, parse_routines  # noqa: E402

GAME = r"G:\SteamLibrary\steamapps\common\Knights of the Old Republic II"
OVERRIDE = os.path.join(GAME, "override")
TSLRCM_OVERRIDE = r"G:\SteamLibrary\steamapps\workshop\content\208580\485537937\override"

VANILLA_HEADER = os.path.join(HERE, "nwnnsscomp", "nwscript.nss")
EXTENDED_HEADER = os.path.join(ROOT, "nss", "nwscript_k2se.nss")

# Target: the DEFAULT CREATURE HEARTBEAT, not the spawn script.
#
# The first attempt wrapped k_def_spawn01 and never fired. Two reasons, both
# instructive:
#   1. spawn scripts run when a creature is CREATED. Loading a save restores
#      creatures that already exist, so OnSpawn never re-runs.
#   2. TSLRCM ships its own k_def_spawn01.ncs in the Workshop override, and
#      whether a local override beats a Workshop override is undocumented.
#
# k_def_heartbt01 avoids both: it fires every few seconds for every creature in
# the area, and it exists ONLY in scripts.bif -- no mod claims it -- so a loose
# file in override/ wins for certain (override always beats BIF).
WRAPPER_NAME = "k_def_heartbt01"
ORIGINAL_COPY = "k2se_orig_hb"      # <= 16 chars: ResRef limit

# left over from the first attempt; cleaned up too
OLD_WRAPPER = "k_def_spawn01"
OLD_ORIGINAL = "k2se_orig_sp"

TEST_SOURCE = """// K2SE M4 test -- wraps the vanilla spawn script, then calls a routine that
// does not exist in the stock engine.
void main()
{
    ExecuteScript("%s", OBJECT_SELF);

    int nVer = K2SE_GetVersion();
    AurPostString("K2SE alive - routine 877 returned " + IntToString(nVer), 5, 5, 8.0);
}
""" % ORIGINAL_COPY


def build_extended_header():
    vanilla = open(VANILLA_HEADER, encoding="utf-8", errors="replace").read()
    protos = parse_routines(vanilla)
    next_id = len(protos)
    text = (vanilla.rstrip()
            + "\n\n"
            + "// ============================================================\n"
            + "// K2SE extended routines. Appended past the last vanilla entry\n"
            + "// (%s = id %d). nwnnsscomp assigns ids positionally, so order\n" % (protos[-1][0], next_id - 1)
            + "// here IS the id -- never insert, only append.\n"
            + "// ============================================================\n"
            + "// %d: K2SE_GetVersion\n" % next_id
            + "int K2SE_GetVersion();\n")
    os.makedirs(os.path.dirname(EXTENDED_HEADER), exist_ok=True)
    with open(EXTENDED_HEADER, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    return next_id


def clean():
    removed = 0
    for name in (WRAPPER_NAME + ".ncs", ORIGINAL_COPY + ".ncs",
                 OLD_WRAPPER + ".ncs", OLD_ORIGINAL + ".ncs",
                 WRAPPER_NAME + ".nss", ORIGINAL_COPY + ".nss"):
        p = os.path.join(OVERRIDE, name)
        if os.path.exists(p):
            os.remove(p)
            print("removed %s" % p)
            removed += 1
    if not removed:
        print("nothing to remove")
    return 0


def main():
    if "--clean" in sys.argv:
        return clean()

    if not os.path.isdir(OVERRIDE):
        raise SystemExit("override folder not found: %s" % OVERRIDE)

    next_id = build_extended_header()
    print("extended header written: %s" % EXTENDED_HEADER)
    print("new routine id: %d\n" % next_id)

    # --- preserve the original, under a new name -----------------------------
    # Prefer a mod's copy if one exists; otherwise pull the stock one out of the
    # BIF archives so the wrapper reproduces vanilla behaviour exactly.
    dst = os.path.join(OVERRIDE, ORIGINAL_COPY + ".ncs")
    modded = os.path.join(TSLRCM_OVERRIDE, WRAPPER_NAME + ".ncs")
    if os.path.exists(modded):
        shutil.copy(modded, dst)
        print("original preserved (from TSLRCM): %s (%d bytes)" % (dst, os.path.getsize(dst)))
    else:
        import kotor_res
        bifs, entries = kotor_res.read_key()
        blob = kotor_res.extract(bifs, entries, WRAPPER_NAME, 2010)
        if blob is None:
            raise SystemExit("%s.ncs not found in the BIFs either" % WRAPPER_NAME)
        with open(dst, "wb") as fh:
            fh.write(blob)
        print("original preserved (from scripts.bif): %s (%d bytes)" % (dst, len(blob)))

    # --- compile the wrapper against the extended header ---------------------
    import tempfile
    workdir = tempfile.mkdtemp(prefix="k2se_m4_")
    ok, ncs, out = compile_script(TEST_SOURCE, EXTENDED_HEADER, workdir, WRAPPER_NAME)
    print("\ncompiler: %s" % (out.replace("\n", " | ")[:200] or "(silent)"))
    if not ok:
        raise SystemExit("compilation FAILED -- nothing was installed")

    acts = find_actions(ncs)
    print("ACTIONs emitted: %s" % ["id=%d argc=%d" % (a[1], a[2]) for a in acts])
    hit = [a for a in acts if a[1] == next_id]
    if not hit:
        raise SystemExit("the compiled script does not call routine %d" % next_id)
    off = hit[0][0]
    print("routine %d call bytes: %s" % (next_id, ncs[off:off + 5].hex(" ")))
    expected = bytes([0x05, 0x00]) + struct.pack(">H", next_id) + bytes([hit[0][2]])
    print("expected            : %s  -> %s"
          % (expected.hex(" "), "MATCH" if ncs[off:off + 5] == expected else "DIFFERENT"))

    out_ncs = os.path.join(OVERRIDE, WRAPPER_NAME + ".ncs")
    with open(out_ncs, "wb") as fh:
        fh.write(ncs)
    print("\ninstalled: %s  (%d bytes)" % (out_ncs, len(ncs)))

    print("\nNow load a save. The spawn script fires as soon as creatures appear.")
    print("Expect on-screen text and a '*** FIRST EXTENDED ROUTINE CALL' line in the log.")
    print("Undo with: python tools/m4_deploy_test.py --clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""Install the K2SE test build and an in-game battery that exercises every
extended routine.

    python tools/deploy_test.py            build + install
    python tools/deploy_test.py --fog      also enable the opt-in fog subsystem
    python tools/deploy_test.py --clean    remove everything this installed

Strategy: WRAP, don't replace. `k_def_heartbt01` is the default creature
heartbeat -- it fires every few seconds for every creature in the area -- so it
is a reliable trigger. Replacing it outright would break creature AI, so:

    override/k2se_orig_hb.ncs     <- the original, copied under a new name
    override/k_def_heartbt01.ncs  <- our wrapper: runs the original first,
                                     then the test battery

The game behaves exactly as before and we get a call site. Everything this
touches is an ADDED file; --clean removes all of them.

Why the heartbeat and not the spawn script: spawn runs when a creature is
CREATED, so loading a save never re-runs it. The heartbeat also exists only in
scripts.bif -- no mod claims it -- so a loose file in override/ wins for certain.

This script REFUSES to install if the compiled bytecode does not contain exactly
the routine ids and argument counts that src/routines.cpp declares. A script
compiled against a mismatched header resolves names against the VANILLA table
with no error and no checksum, and the mistake only appears at runtime as the
wrong engine routine running with your arguments on the stack.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from q1_q2_compiler_test import compile_script, parse_routines  # noqa: E402
import routine_id_test  # noqa: E402

GAME = r"G:\SteamLibrary\steamapps\common\Knights of the Old Republic II"
OVERRIDE = os.path.join(GAME, "override")
TSLRCM_OVERRIDE = r"G:\SteamLibrary\steamapps\workshop\content\208580\485537937\override"

EXTENDED_HEADER = os.path.join(ROOT, "nss", "nwscript_k2se.nss")
BUILT_DLL = os.path.join(ROOT, "out", "version.dll")
DEPLOYED_DLL = os.path.join(GAME, "version.dll")
FOG_MARKER = os.path.join(GAME, "k2se_fog.txt")

WRAPPER_NAME = "k_def_heartbt01"
ORIGINAL_COPY = "k2se_orig_hb"  # <= 16 chars: ResRef limit

# left over from earlier milestones; cleaned up too
OLD_WRAPPER = "k_def_spawn01"
OLD_ORIGINAL = "k2se_orig_sp"

# The battery. Every extended routine gets exercised, and the four that walk
# imported struct offsets (881-884) also PRINT THEIR VALUES on screen -- those
# offsets cannot be checked against the file, so the only real verification is a
# human comparing them with the character sheet.
TEST_SOURCE = """// K2SE in-game test battery. Wraps the vanilla creature heartbeat, then
// exercises the extended ABI. Results land in %%LOCALAPPDATA%%\\K2SE\\k2se.log
// via K2SE_ReportTest; the C++ side logs each test id once, then traces.
void main()
{
    ExecuteScript("%s", OBJECT_SELF);

    object oPC = GetFirstPC();

    // --- proven in the 27/08 session; re-run as a regression check -----------
    int nVer = K2SE_GetVersion();
    K2SE_ReportTest(1, 100, nVer);              // 877, extended round trip
    K2SE_ReportTest(2, 100, abs(-1234567890));  // abs() presence sentinel
    K2SE_ReportTest(3, 5, abs(-5));             // abs() still faithful
    K2SE_ReportTest(4, 7, abs(7));
    K2SE_ReportTest(5, 0, abs(0));
    K2SE_ReportTest(6, 111250333, K2SE_SelfTest(111, 2.5, 333));  // 878, pop order

    // --- 880: the string round trip -----------------------------------------
    // An identity function, so any difference means the string ABI is wrong.
    // This is the first K2SE type whose buffer the engine and K2SE share.
    string sIn = "K2SE-880-abc";
    string sOut = K2SE_EchoString(sIn);
    K2SE_ReportTest(7, TRUE, (sOut == sIn));

    // --- 881-884: the creature reads ----------------------------------------
    // Sanity bounds catch the two failure modes that matter: -1 means the
    // pointer chain broke, and a wild number means an offset is wrong.
    int nStr  = K2SE_GetAbilityScoreBase(oPC, ABILITY_STRENGTH);
    int nDex  = K2SE_GetAbilityScoreBase(oPC, ABILITY_DEXTERITY);
    int nSkil = K2SE_GetSkillRankBase(oPC, SKILL_TREAT_INJURY);
    int nFeat = K2SE_GetFeatAcquired(oPC, FEAT_ARMOUR_PROF_LIGHT);
    int nSpel = K2SE_GetSpellAcquired(oPC, FORCE_POWER_AFFLICTION);

    K2SE_ReportTest(8,  TRUE, (nStr  > 0 && nStr  < 100));
    K2SE_ReportTest(9,  TRUE, (nDex  > 0 && nDex  < 100));
    K2SE_ReportTest(10, TRUE, (nSkil >= 0 && nSkil < 100));
    K2SE_ReportTest(11, TRUE, (nFeat == 0 || nFeat == 1));
    K2SE_ReportTest(12, TRUE, (nSpel == 0 || nSpel == 1));

    // Vanilla's own answers, for comparison. These include item and effect
    // modifiers; K2SE's are the BASE values, so they agree only when nothing is
    // modifying the stat. Shown side by side so the difference is visible
    // rather than mysterious.
    int nStrTotal = GetAbilityScore(oPC, ABILITY_STRENGTH);
    int nSkilTotal = GetSkillRank(SKILL_TREAT_INJURY, oPC);

    // --- 885-888: fog -------------------------------------------------------
    // 0 when the k2se_fog.txt marker is absent, which is the default. Reported,
    // never failed: "off" is a valid state.
    int nFog = K2SE_GetFogStatus();
    K2SE_ReportTest(13, TRUE, (nFog >= 0));

    // --- on screen ----------------------------------------------------------
    AurPostString("K2SE v" + IntToString(nVer) + "  str " + IntToString(nStr) +
                  "/" + IntToString(nStrTotal) + "  dex " + IntToString(nDex) +
                  "  heal " + IntToString(nSkil) + "/" + IntToString(nSkilTotal),
                  5, 25, 5.0);
    AurPostString("K2SE echo " + sOut + "  feat " + IntToString(nFeat) +
                  "  power " + IntToString(nSpel) + "  fog " + IntToString(nFog),
                  5, 40, 5.0);
}
""" % ORIGINAL_COPY


def run(label, argv):
    print("\n--- %s ---" % label)
    proc = subprocess.run(argv, cwd=ROOT, capture_output=True)
    out = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    tail = [l for l in out.strip().splitlines() if l.strip()][-3:]
    for l in tail:
        print("  " + l)
    return proc.returncode == 0


def game_is_running():
    """True if swkotor2.exe is live.

    It matters because a running game holds version.dll mapped, so the copy
    fails with a bare PermissionError that looks like a rights problem rather
    than what it is. Checked up front, before anything has been installed, so
    the answer is 'close the game and re-run' instead of a half-deployed state.
    """
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq swkotor2.exe"],
                             capture_output=True, timeout=30)
        return b"swkotor2.exe" in out.stdout
    except Exception:
        return False  # can't tell; let the copy fail on its own terms


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
    for p in (FOG_MARKER, DEPLOYED_DLL):
        if os.path.exists(p):
            os.remove(p)
            print("removed %s" % p)
            removed += 1
    if not removed:
        print("nothing to remove")
    print("\nThe game is back to vanilla. swkotor2.exe was never modified.")
    return 0


def main():
    if "--clean" in sys.argv:
        return clean()

    if not os.path.isdir(OVERRIDE):
        raise SystemExit("override folder not found: %s" % OVERRIDE)
    if not os.path.exists(BUILT_DLL):
        raise SystemExit("no build at %s -- run .\\build_direct.ps1 first" % BUILT_DLL)

    if game_is_running():
        raise SystemExit(
            "swkotor2.exe is RUNNING, so version.dll is mapped and cannot be replaced.\n"
            "\n"
            "Close the game, then run this again. Nothing has been installed --\n"
            "this check happens before anything is written, so there is no\n"
            "half-deployed state to clean up.")

    # --- gates: never install against an unverified binary or a drifted header
    if not run("verifying addresses against the executable",
               [sys.executable, os.path.join(HERE, "verify_offsets.py")]):
        raise SystemExit("verify_offsets FAILED -- nothing installed")
    if not run("verifying routine ids / header / bytecode agree",
               [sys.executable, os.path.join(HERE, "routine_id_test.py")]):
        raise SystemExit("routine_id_test FAILED -- nothing installed")
    if not run("verifying the DLL", [sys.executable, os.path.join(HERE, "check_dll.py"),
                                     BUILT_DLL]):
        raise SystemExit("check_dll FAILED -- nothing installed")

    # --- the routine table, straight from the C++ ---------------------------
    table = routine_id_test.TABLE_RE.findall(
        open(os.path.join(ROOT, "src", "routines.cpp"), encoding="utf-8").read())
    declared = {int(i): (n, int(a)) for i, n, a in table}
    print("\nextended routines declared in src/routines.cpp: %d (%d..%d)"
          % (len(declared), min(declared), max(declared)))

    # --- the DLL ------------------------------------------------------------
    print("\n--- installing the DLL ---")
    shutil.copy(BUILT_DLL, DEPLOYED_DLL)
    print("  %s  (%d bytes)" % (DEPLOYED_DLL, os.path.getsize(DEPLOYED_DLL)))

    if "--fog" in sys.argv:
        with open(FOG_MARKER, "w", encoding="utf-8") as fh:
            fh.write("Presence of this file enables K2SE's runtime fog support.\n"
                     "Delete it to turn fog off. See nss/k2se.nss.\n")
        print("  %s  (fog subsystem ENABLED)" % FOG_MARKER)
    else:
        if os.path.exists(FOG_MARKER):
            os.remove(FOG_MARKER)
        print("  fog subsystem off (pass --fog to enable)")

    # --- preserve the original heartbeat ------------------------------------
    print("\n--- installing the test battery ---")
    dst = os.path.join(OVERRIDE, ORIGINAL_COPY + ".ncs")
    modded = os.path.join(TSLRCM_OVERRIDE, WRAPPER_NAME + ".ncs")
    if os.path.exists(modded):
        shutil.copy(modded, dst)
        print("  original preserved (from TSLRCM): %s (%d bytes)"
              % (os.path.basename(dst), os.path.getsize(dst)))
    else:
        import kotor_res
        bifs, entries = kotor_res.read_key()
        blob = kotor_res.extract(bifs, entries, WRAPPER_NAME, 2010)
        if blob is None:
            raise SystemExit("%s.ncs not found in the BIFs" % WRAPPER_NAME)
        with open(dst, "wb") as fh:
            fh.write(blob)
        print("  original preserved (from scripts.bif): %s (%d bytes)"
              % (os.path.basename(dst), len(blob)))

    # --- compile against the committed extended header ----------------------
    workdir = tempfile.mkdtemp(prefix="k2se_deploy_")
    ok, ncs, out = compile_script(TEST_SOURCE, EXTENDED_HEADER, workdir, WRAPPER_NAME)
    print("  compiler: %s" % (out.replace("\n", " | ")[:160] or "(silent)"))
    if not ok:
        raise SystemExit("compilation FAILED -- nothing was installed")

    # --- every extended ACTION must match what routines.cpp declares --------
    candidates = routine_id_test.find_actions(ncs)
    called = {}
    for _off, rid, argc in candidates:
        if rid in declared:
            called.setdefault(rid, set()).add(argc)

    problems = []
    for rid in sorted(declared):
        name, want_argc = declared[rid]
        if rid not in called:
            problems.append("routine %d (%s) is never called by the battery" % (rid, name))
        elif want_argc not in called[rid]:
            problems.append("routine %d (%s) called with argc %s, declared %d"
                            % (rid, name, sorted(called[rid]), want_argc))
        else:
            raw = bytes([0x05, 0x00]) + struct.pack(">H", rid) + bytes([want_argc])
            print("  OK  %-26s id %-4d argc %d   ACTION %s"
                  % (name, rid, want_argc, raw.hex(" ")))

    if problems:
        print("")
        for p in problems:
            print("  REFUSED: %s" % p)
        raise SystemExit("\nbytecode does not match src/routines.cpp -- nothing installed")

    out_ncs = os.path.join(OVERRIDE, WRAPPER_NAME + ".ncs")
    with open(out_ncs, "wb") as fh:
        fh.write(ncs)
    print("\n  installed: %s  (%d bytes)" % (out_ncs, len(ncs)))

    print("""
================================================================================
READY. Launch the game normally (Steam, shortcut, anything) and load a save.

The battery runs on the creature heartbeat, so it starts within a few seconds of
the save loading -- no trigger needed. You should see two lines top-left.

WHAT TO LOOK AT
  1. On screen: compare `str  N/M` and `heal N/M` with your character sheet.
     The FIRST number is K2SE's base value, the SECOND is vanilla's total. They
     differ when an item or effect is modifying the stat -- that is correct, not
     a bug. What would be wrong is -1 (the pointer chain broke) or nonsense.
  2. The log: %LOCALAPPDATA%\\K2SE\\k2se.log
     Expect `TEST  1..13` lines, each PASS. Tests 8-12 are the ones that have
     never run before; 7 is the string round trip.

Send me the log and I will read it.

UNDO EVERYTHING:  python tools/deploy_test.py --clean
================================================================================""")
    return 0


if __name__ == "__main__":
    sys.exit(main())

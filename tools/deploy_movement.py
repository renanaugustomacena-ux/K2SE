"""Install the K2SE build with the movement features (K2 Jump / Crouch / Sprint).

    python tools/deploy_movement.py --install [--enable sprint,crouch,jump,roll] [--banner]
    python tools/deploy_movement.py --remap-keys      print the in-game rebinding steps (ini edits are reverted by the game)
    python tools/deploy_movement.py --restore-keys    put the [Keymapping] backup back
    python tools/deploy_movement.py --clean           previous DLL back, ini removed
    python tools/deploy_movement.py --status          what is installed right now

Same gates as deploy_test.py: refuses while the game runs, refuses if the
addresses, routine ids or the DLL do not verify. Everything it writes into the
game folder is either an ADDED file (version.dll, k2se_movement.ini) or a
backed-up edit of swkotor2.ini's [Keymapping] section.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

GAME = r"G:\SteamLibrary\steamapps\common\Knights of the Old Republic II"
BUILT_DLL = os.path.join(ROOT, "out", "version.dll")
DEPLOYED_DLL = os.path.join(GAME, "version.dll")
INI_TEMPLATE = os.path.join(ROOT, "data", "k2se_movement.ini")
DEPLOYED_INI = os.path.join(GAME, "k2se_movement.ini")
SWKOTOR_INI = os.path.join(GAME, "swkotor2.ini")

FEATURES = ("sprint", "crouch", "jump", "roll", "directional", "fov", "camera", "spawner", "npcvariety")
SECTION_OF = {"sprint": "Sprint", "crouch": "Crouch", "jump": "Jump", "roll": "Roll",
              "directional": "Directional", "fov": "FOV", "camera": "Camera",
              "spawner": "Spawner", "npcvariety": "NpcVariety"}
SPAWN_SCRIPT = "k2se_spawn"
SPAWN_SCRIPT_SRC = os.path.join(ROOT, "nss", SPAWN_SCRIPT + ".nss")
SPAWN_DATA_SRC = os.path.join(ROOT, "data", "k2se_spawns")
SPAWN_DATA_DST = os.path.join(GAME, "k2se_spawns")
OVERRIDE = os.path.join(GAME, "override")

# Key codes are the engine's own (keymap.2da `language0` column): letters A=51..Z=76,
# digits 1..9 = 77..85, Space 87, F1..F6 = 39..44 (F9 = 47 extrapolated), arrows Left 7 Right 8 Up 9 Down 10.
REMAP = {
    "Action241": (87, 47, "Pause: Space -> F9 (Pause/Break still pauses)"),
    "Action281A": (76, 7, "ActionLeft: Z -> Left arrow"),
    "Action281B": (53, 8, "ActionRight: C -> Right arrow"),
    # Directional movement: A/D become strafe axes, so the game's own camera
    # rotation moves to the numpad (rows 46/47 of keymap.2da are disabled, the
    # keys are free). Mouse camera rotation is the next feature.
    "Action284A": (51, 14, "CameraRotateLeft: A -> Numpad4"),
    "Action284B": (54, 16, "CameraRotateRight: D -> Numpad6"),
}


def stamp():
    return time.strftime("%Y%m%d-%H%M%S")


def run(label, argv):
    print("\n--- %s ---" % label)
    proc = subprocess.run(argv, cwd=ROOT, capture_output=True)
    out = (proc.stdout + proc.stderr).decode("utf-8", "replace")
    for l in [l for l in out.strip().splitlines() if l.strip()][-3:]:
        print("  " + l)
    return proc.returncode == 0


def game_is_running():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq swkotor2.exe"], capture_output=True,
                             timeout=30)
        return b"swkotor2.exe" in out.stdout
    except Exception:
        return False


def read_ini_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def write_ini_lines(path, lines):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def set_enabled(lines, section, on):
    """Flip Enabled= inside [section]; returns the new lines."""
    out, cur = [], None
    for l in lines:
        m = re.match(r"^\s*\[(.+?)\]", l)
        if m:
            cur = m.group(1).lower()
        if cur == section.lower() and re.match(r"^\s*Enabled\s*=", l):
            l = "Enabled=%d" % (1 if on else 0)
        out.append(l)
    return out


def set_value(lines, section, key, value):
    out, cur = [], None
    for l in lines:
        m = re.match(r"^\s*\[(.+?)\]", l)
        if m:
            cur = m.group(1).lower()
        if cur == section.lower() and re.match(r"^\s*%s\s*=" % re.escape(key), l):
            l = "%s=%s" % (key, value)
        out.append(l)
    return out


def keymap_conflicts():
    """Which of our default keys the game still binds, from swkotor2.ini."""
    if not os.path.exists(SWKOTOR_INI):
        return []
    txt = open(SWKOTOR_INI, "r", encoding="utf-8", errors="replace").read()
    found = []
    for action, (old, _new, desc) in REMAP.items():
        m = re.search(r"^%s=(\d+)" % action, txt, re.M)
        if m and int(m.group(1)) == old:
            found.append((action, desc))
    return found


IN_GAME_REBIND = """\
Rebind in game (Options -> Keyboard), the only change the game keeps:
  Pause                 Space  -> F9        (Pause/Break still pauses; Space becomes Jump)
  Camera Rotate Left    A      -> Numpad 4  (A becomes strafe/turn left for WASD movement)
  Camera Rotate Right   D      -> Numpad 6  (D becomes strafe/turn right)
  Action Left / Right   Z / C  -> Left / Right arrow   (optional: C is then only Crouch)
Then quit to the menu or leave the game once, so swkotor2.ini is written with them."""


def remap_keys():
    # Session S1 (2026-09-02): the game does not honour [Keymapping] values written
    # from outside -- it rewrote the stock bindings over two separate edits (all the
    # backups it left hold the defaults). The codes were right (keymap.2da
    # `language0`: 47 F9, 14/16 numpad 4/6, 7/8 arrows), the mechanism is not ours
    # to fix, so this command now only prints the in-game steps. --restore-keys still
    # puts back a backup made by the old behaviour.
    print(IN_GAME_REBIND)
    conflicts = keymap_conflicts()
    if conflicts:
        print("\nswkotor2.ini currently still binds:")
        for action, desc in conflicts:
            print("  %s   (%s)" % (action, desc))
    else:
        print("\nswkotor2.ini shows none of the stock conflicts: the keys are free.")
    return 0


def restore_keys():
    backups = sorted(f for f in os.listdir(GAME) if f.startswith("swkotor2.ini.backup-keymap-"))
    if not backups:
        raise SystemExit("no keymap backup found in the game folder")
    if game_is_running():
        raise SystemExit("close the game first")
    src = os.path.join(GAME, backups[-1])
    shutil.copy(src, SWKOTOR_INI)
    print("restored %s -> swkotor2.ini" % backups[-1])
    return 0


def status():
    print("game folder: %s" % GAME)
    for p in (DEPLOYED_DLL, DEPLOYED_INI):
        if os.path.exists(p):
            print("  present: %-22s %8d bytes  %s" % (os.path.basename(p), os.path.getsize(p),
                                                     time.ctime(os.path.getmtime(p))))
        else:
            print("  absent : %s" % os.path.basename(p))
    for f in sorted(os.listdir(GAME)):
        if f.startswith("version.dll.backup"):
            print("  backup : %s" % f)
    conflicts = keymap_conflicts()
    if conflicts:
        print("  key conflicts in swkotor2.ini (rebind in Options -> Keyboard; see --remap-keys):")
        for action, desc in conflicts:
            print("    %s  %s" % (action, desc))
    else:
        print("  keymap: Space and C are free")
    log = os.path.join(os.environ.get("LOCALAPPDATA", ""), "K2SE", "k2se.log")
    print("  log    : %s%s" % (log, "" if os.path.exists(log) else " (not yet written)"))
    return 0


def clean():
    if game_is_running():
        raise SystemExit("close the game first")
    removed = 0
    if os.path.exists(DEPLOYED_INI):
        os.remove(DEPLOYED_INI)
        print("removed %s" % DEPLOYED_INI)
        removed += 1
    backups = sorted(f for f in os.listdir(GAME) if f.startswith("version.dll.backup-movement-"))
    if backups:
        src = os.path.join(GAME, backups[-1])
        shutil.copy(src, DEPLOYED_DLL)
        print("restored previous DLL from %s" % backups[-1])
        removed += 1
    ncs = os.path.join(OVERRIDE, SPAWN_SCRIPT + ".ncs")
    if os.path.exists(ncs):
        os.remove(ncs)
        print("removed %s" % ncs)
        removed += 1
    if not removed:
        print("nothing to remove")
    print("swkotor2.ini was not touched by --clean; use --restore-keys for the key bindings.")
    print("k2se_spawns\\ (your spawn data) was left in place.")
    return 0


def deploy_spawn_script():
    """Compile nss/k2se_spawn.nss against nss/nwscript_k2se.nss and put the .ncs in
    override/, plus the k2se_spawns folder (README + example, never overwriting a
    module file the user wrote)."""
    import tempfile
    compiler = os.path.join(HERE, "nwnnsscomp", "nwnnsscomp.exe")
    header = os.path.join(ROOT, "nss", "nwscript_k2se.nss")
    if not os.path.exists(compiler):
        raise SystemExit("nwnnsscomp.exe missing -- run: python tools/fetch_compiler.py")
    if not os.path.exists(SPAWN_SCRIPT_SRC):
        raise SystemExit("missing %s" % SPAWN_SCRIPT_SRC)
    with tempfile.TemporaryDirectory() as workdir:
        shutil.copy(header, os.path.join(workdir, "nwscript.nss"))
        shutil.copy(compiler, os.path.join(workdir, "nwnnsscomp.exe"))
        nss = os.path.join(workdir, SPAWN_SCRIPT + ".nss")
        ncs = os.path.join(workdir, SPAWN_SCRIPT + ".ncs")
        shutil.copy(SPAWN_SCRIPT_SRC, nss)
        proc = subprocess.run([os.path.join(workdir, "nwnnsscomp.exe"), "-c", nss, "-o", ncs],
                              capture_output=True, cwd=workdir, timeout=120)
        output = (proc.stdout + proc.stderr).decode("utf-8", "replace").strip()
        if not os.path.exists(ncs):
            raise SystemExit("k2se_spawn.nss did not compile:\n" + output)
        os.makedirs(OVERRIDE, exist_ok=True)
        dst = os.path.join(OVERRIDE, SPAWN_SCRIPT + ".ncs")
        shutil.copy(ncs, dst)
        print("  %s  (%d bytes, compiled against nwscript_k2se.nss)" % (dst, os.path.getsize(dst)))
    os.makedirs(SPAWN_DATA_DST, exist_ok=True)
    copied = 0
    for name in os.listdir(SPAWN_DATA_SRC):
        src = os.path.join(SPAWN_DATA_SRC, name)
        dst = os.path.join(SPAWN_DATA_DST, name)
        if os.path.isfile(src) and (not os.path.exists(dst) or name.startswith("_") or name.lower() == "readme.txt"):
            shutil.copy(src, dst)
            copied += 1
    modules = [f for f in os.listdir(SPAWN_DATA_DST) if f.lower().endswith(".ini") and not f.startswith("_")]
    print("  %s  (%d file(s) refreshed; module files present: %s)" % (SPAWN_DATA_DST, copied,
                                                                     ", ".join(modules) or "none yet -- use F10"))


def install(enable, banner):
    if not os.path.exists(BUILT_DLL):
        raise SystemExit("no build at %s -- run .\\build_direct.ps1 first" % BUILT_DLL)
    if game_is_running():
        raise SystemExit(
            "swkotor2.exe is RUNNING, so version.dll is mapped and cannot be replaced.\n"
            "Save, close the game, run this again. Nothing has been written.")

    if not run("verifying addresses against the executable",
               [sys.executable, os.path.join(HERE, "verify_offsets.py")]):
        raise SystemExit("verify_offsets FAILED -- nothing installed")
    if not run("verifying routine ids / header / bytecode agree",
               [sys.executable, os.path.join(HERE, "routine_id_test.py")]):
        raise SystemExit("routine_id_test FAILED -- nothing installed")
    if not run("verifying the DLL", [sys.executable, os.path.join(HERE, "check_dll.py"), BUILT_DLL]):
        raise SystemExit("check_dll FAILED -- nothing installed")

    print("\n--- installing ---")
    if os.path.exists(DEPLOYED_DLL):
        backup = DEPLOYED_DLL + ".backup-movement-" + stamp()
        shutil.copy(DEPLOYED_DLL, backup)
        print("  previous DLL kept as %s" % os.path.basename(backup))
    shutil.copy(BUILT_DLL, DEPLOYED_DLL)
    print("  %s  (%d bytes)" % (DEPLOYED_DLL, os.path.getsize(DEPLOYED_DLL)))

    template = read_ini_lines(INI_TEMPLATE)
    if os.path.exists(DEPLOYED_INI):
        lines = read_ini_lines(DEPLOYED_INI)
        # Sections added to the template after the first install must reach the
        # deployed file too (2026-09-02: [Directional] was silently missing).
        have = {m.group(1).lower() for m in (re.match(r"^\s*\[(.+?)\]", l) for l in lines) if m}
        cur, block, added = None, [], []
        for l in template + ["[__end__]"]:
            m = re.match(r"^\s*\[(.+?)\]", l)
            if m:
                if cur and cur.lower() not in have:
                    lines += [""] + block
                    added.append(cur)
                cur, block = m.group(1), [l]
            else:
                block.append(l)
        print("  existing k2se_movement.ini kept (Enabled= flags updated%s)"
              % (", sections added: " + ", ".join(added) if added else ""))
    else:
        lines = list(template)
        print("  k2se_movement.ini created from data/k2se_movement.ini")
    for f in FEATURES:
        lines = set_enabled(lines, SECTION_OF[f], f in enable)
    lines = set_value(lines, "Debug", "Banner", "1" if banner else "0")
    write_ini_lines(DEPLOYED_INI, lines)
    print("  features enabled: %s%s" % (", ".join(enable) or "(none)", "  + banner" if banner else ""))
    if "spawner" in enable:
        deploy_spawn_script()

    conflicts = keymap_conflicts()
    if conflicts and any(f in enable for f in ("jump", "crouch", "directional")):
        print("\n  WARNING: the game still binds keys we use:")
        for action, desc in conflicts:
            print("    %s  %s" % (action, desc))
        print("  Rebind them in game: Options -> Keyboard (python tools/deploy_movement.py --remap-keys prints the steps)")

    print("""
================================================================================
READY. Launch the game, load a save, check %LOCALAPPDATA%\\K2SE\\k2se.log for:
  config: ... loaded            movement: sprint ON/off ...   callsite: ... redirected
  movement: installed (N call sites redirected)
Then play the checklist for this session (PIANO-DAZIONE, Appendice D).
UNDO:  python tools/deploy_movement.py --clean
================================================================================""")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--enable", default="sprint", help="comma list: sprint,crouch,jump,roll (default sprint)")
    ap.add_argument("--banner", action="store_true", help="on-screen K2SE movement banner")
    ap.add_argument("--remap-keys", action="store_true")
    ap.add_argument("--restore-keys", action="store_true")
    ap.add_argument("--clean", action="store_true")
    ap.add_argument("--status", action="store_true")
    a = ap.parse_args()

    if a.remap_keys:
        return remap_keys()
    if a.restore_keys:
        return restore_keys()
    if a.clean:
        return clean()
    if a.install:
        enable = [f.strip().lower() for f in a.enable.split(",") if f.strip()]
        bad = [f for f in enable if f not in FEATURES]
        if bad:
            raise SystemExit("unknown feature(s): %s" % ", ".join(bad))
        return install(enable, a.banner)
    return status()


if __name__ == "__main__":
    sys.exit(main())

"""Measure named nodes in KOTOR 2 models -- above all, where the eyes are.

The first-person camera in k2-multi-fov used to sit at a hardcoded height of
1.65 m for every character. That number is wrong for everyone: the male PC's
eyes are at 1.6726 and the female PC's at 1.5886, an 8.4 cm spread that one
constant cannot represent. This tool replaces the constant with a measurement.

What it does:

    body model (pmbam)  -> world transform of `headhook` and `camerahook`
    head model (pmhc01) -> local transform of the two eye bones
    eye point = headhook_world  o  midpoint(eyeL, eyeR)

all in model space, metres, Z up and Y forward. appearance.2da gives the body
model (`race`), the head row (`normalhead`) and the attachment node
(`headbone`); heads.2da turns the head row into a head model resref.

    python tools/model_measure.py --report
    python tools/model_measure.py --report --appearance 134
    python tools/model_measure.py --csv data/k2se_eye_offsets.csv
    python tools/model_measure.py --nodes pmbam:camerahook,headhook

Deliberately dependency-free, like kotor_res.py and twoda.py next to it: the
MDL node tree is about eighty lines and pulling in a modelling library for a
parent-chain walk would be a poor trade. The format follows
ref/reone/src/libs/graphics/format/mdlmdxreader.cpp, which is the oracle for
every offset below. `--verify` cross-checks the parse against PyKotor when that
happens to be installed (it is not required, and is not a project dependency).

Only rest-pose transforms are read. Animation controllers (type 8 position,
type 20 orientation) move these nodes at runtime; that is the DLL's problem,
not this tool's -- see K2SE/src/fpcam.cpp.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))), "k2-directional-movement", "tools"))

import kotor_res  # noqa: E402
from twoda import TwoDA  # noqa: E402

RES_MDL = 2002
RES_2DA = 2017

# Every offset here is from mdlmdxreader.cpp. File offsets are relative to
# MDL_DATA_OFFSET, not to the start of the file -- that indirection is the one
# thing that catches people out when they write an MDL reader.
MDL_DATA_OFFSET = 12

# Geometry header, measured from MDL_DATA_OFFSET.
OFF_ROOT_NODE = 40      # after funcPtr1, funcPtr2, char[32] name
OFF_NUM_NODES = 44
# Model header, same base.
OFF_NAME_ARRAY = 184    # ArrayDefinition{offset, count, count2}

# Node header, from MDL_DATA_OFFSET + node offset.
NODE_NAME_INDEX = 4
NODE_POSITION = 16
NODE_ORIENTATION = 28
NODE_CHILD_ARRAY = 44

# The head is parented here when appearance.2da leaves `headbone` empty.
DEFAULT_HEAD_BONE = "headhook"

# The shipped heads use two naming conventions for the eye bones, in this order
# of popularity: 109 of the 125 head models say eyeLA/eyeRA and 6 say eyeL/eyeR.
# The remaining ones (Visas, Darth Revan, the masked Dark Jedi and Sith
# Assassin) have no eye nodes at all because their faces are covered -- that is
# real data, not a parse failure, and they fall back to the body's camerahook.
EYE_NODE_PAIRS = (("eyeLA", "eyeRA"), ("eyeL", "eyeR"))


# --- quaternion helpers ------------------------------------------------------
# Quaternions are (w, x, y, z), matching the MDL field order.

def q_mul(a, b):
    w1, x1, y1, z1 = a
    w2, x2, y2, z2 = b
    return (w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2,
            w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
            w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
            w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2)


def q_rotate(q, v):
    """Rotate v by q. The cross-product form, so no matrix is built."""
    w, x, y, z = q
    vx, vy, vz = v
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty),
            vy + w * ty + (z * tx - x * tz),
            vz + w * tz + (x * ty - y * tx))


def v_add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def v_sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def v_mid(a, b):
    return ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0, (a[2] + b[2]) / 2.0)


# --- the MDL node tree -------------------------------------------------------

class Node(object):
    __slots__ = ("name", "position", "orientation", "parent", "world_pos", "world_ori")

    def __init__(self, name, position, orientation, parent):
        self.name = name
        self.position = position
        self.orientation = orientation
        self.parent = parent
        self.world_pos = None
        self.world_ori = None


class Model(object):
    """The node hierarchy of one MDL, with rest-pose world transforms resolved.

    Node names are keyed lower-case: the shipped models are inconsistent about
    case (`FreeLookHook` beside `headhook`) and the engine does not care.
    """

    def __init__(self, resref, blob):
        self.resref = resref
        self.nodes = {}
        self.order = []
        self._parse(blob)

    def _parse(self, blob):
        if len(blob) < MDL_DATA_OFFSET + OFF_NAME_ARRAY + 12:
            raise ValueError("%s: too small to be an MDL (%d bytes)" % (self.resref, len(blob)))

        def u32(at):
            return struct.unpack_from("<I", blob, at)[0]

        base = MDL_DATA_OFFSET
        root_offset = u32(base + OFF_ROOT_NODE)
        name_array_offset = u32(base + OFF_NAME_ARRAY)
        name_count = u32(base + OFF_NAME_ARRAY + 4)

        names = []
        for i in range(name_count):
            at = base + u32(base + name_array_offset + 4 * i)
            end = blob.index(b"\x00", at)
            names.append(blob[at:end].decode("ascii", "replace"))

        # Iterative walk: some supermodels nest deeply enough to matter, and a
        # recursive walk would need the interpreter's limit raised. Children go
        # on the stack reversed so they come off in the order the file lists
        # them -- self.order then holds a genuine depth-first pre-order, which
        # is what makes "the first node with this name wins" a well-defined
        # rule. s_male02 ships two nodes called w_Longsword, so it matters.
        stack = [(root_offset, None)]
        while stack:
            offset, parent = stack.pop()
            at = base + offset
            name_index = struct.unpack_from("<H", blob, at + NODE_NAME_INDEX)[0]
            name = names[name_index] if name_index < len(names) else "?%d" % name_index
            position = struct.unpack_from("<3f", blob, at + NODE_POSITION)
            orientation = struct.unpack_from("<4f", blob, at + NODE_ORIENTATION)

            node = Node(name, position, orientation, parent)
            key = name.lower()
            # A duplicated name means the model reuses a label; the first one
            # wins, which is what the engine's own by-name lookup does.
            if key not in self.nodes:
                self.nodes[key] = node
            self.order.append(node)
            self._resolve(node)

            child_offset = u32(at + NODE_CHILD_ARRAY)
            child_count = u32(at + NODE_CHILD_ARRAY + 4)
            for i in reversed(range(child_count)):
                stack.append((u32(base + child_offset + 4 * i), node))

    @staticmethod
    def _resolve(node):
        """World transform = parent's, then this node's local one."""
        if node.parent is None:
            node.world_pos = tuple(node.position)
            node.world_ori = tuple(node.orientation)
            return
        node.world_pos = v_add(node.parent.world_pos,
                               q_rotate(node.parent.world_ori, node.position))
        node.world_ori = q_mul(node.parent.world_ori, node.orientation)

    def has(self, name):
        return name.lower() in self.nodes

    def world(self, name):
        """(position, orientation) in model space, or None when absent."""
        node = self.nodes.get(name.lower())
        return (node.world_pos, node.world_ori) if node else None

    def position_of(self, name):
        got = self.world(name)
        return got[0] if got else None


# --- game resources ----------------------------------------------------------

class Resources(object):
    """KEY/BIF lookup with Override taking priority, as the engine does it."""

    def __init__(self, game=None, override=None):
        self.game = game or kotor_res.GAME
        kotor_res.GAME = self.game
        self.bifs, self.entries = kotor_res.read_key(os.path.join(self.game, "chitin.key"))
        self.override_dirs = [d for d in (override or [os.path.join(self.game, "override")])
                              if os.path.isdir(d)]
        self._cache = {}

    def get(self, resref, restype, ext):
        key = (resref.lower(), restype)
        if key in self._cache:
            return self._cache[key]
        blob = None
        for directory in self.override_dirs:
            path = os.path.join(directory, "%s.%s" % (resref, ext))
            if os.path.isfile(path):
                with open(path, "rb") as fh:
                    blob = fh.read()
                break
        if blob is None:
            blob = kotor_res.extract(self.bifs, self.entries, resref, restype)
        self._cache[key] = blob
        return blob

    def model(self, resref):
        blob = self.get(resref, RES_MDL, "mdl")
        return Model(resref, blob) if blob else None

    def table(self, name):
        blob = self.get(name, RES_2DA, "2da")
        return TwoDA.parse(blob) if blob else None


# --- the measurement itself --------------------------------------------------

class Measurement(object):
    def __init__(self, body, head, head_bone):
        self.body = body
        self.head = head
        self.head_bone = head_bone
        self.eye_left = None
        self.eye_right = None
        self.eye_mid = None
        self.camerahook = None
        self.freelookhook = None
        self.headhook = None
        self.rootdummy = None
        self.eye_nodes = ""
        self.note = ""

    @property
    def eye_height(self):
        return self.eye_mid[2] if self.eye_mid else None

    @property
    def interpupillary(self):
        if not (self.eye_left and self.eye_right):
            return None
        return abs(self.eye_right[0] - self.eye_left[0])

    @property
    def delta_from_camerahook(self):
        if not (self.eye_mid and self.camerahook):
            return None
        return v_sub(self.eye_mid, self.camerahook)


def measure(res, body_resref, head_resref, head_bone=DEFAULT_HEAD_BONE):
    """Eye point of a body+head pair, in the body model's space."""
    m = Measurement(body_resref, head_resref, head_bone)
    body = res.model(body_resref)
    if body is None:
        m.note = "body model not found"
        return m

    m.headhook = body.position_of(head_bone)
    m.camerahook = body.position_of("camerahook")
    m.freelookhook = body.position_of("FreeLookHook")
    m.rootdummy = body.position_of("rootdummy")

    # Characters whose face is part of the body model -- Kreia, the Handmaiden,
    # Atris -- have no head row at all, but their bodies carry the eye bones
    # directly. Measure those in the body's own space.
    if not head_resref:
        own = next((pair for pair in EYE_NODE_PAIRS
                    if body.has(pair[0]) and body.has(pair[1])), None)
        if own is None:
            m.note = "no head model and no eye nodes in the body"
            return m
        m.eye_nodes = "/".join(own) + " (in body)"
        m.eye_left = body.position_of(own[0])
        m.eye_right = body.position_of(own[1])
        m.eye_mid = v_mid(m.eye_left, m.eye_right)
        return m

    head = res.model(head_resref)
    if head is None:
        m.note = "head model not found"
        return m
    eye_nodes = next((pair for pair in EYE_NODE_PAIRS
                      if head.has(pair[0]) and head.has(pair[1])), None)
    if eye_nodes is None:
        m.note = "head has no eye nodes (masked face) -- use camerahook"
        return m
    m.eye_nodes = "/".join(eye_nodes)

    hook = body.world(head_bone)
    if hook is None:
        m.note = "body has no %s" % head_bone
        return m
    hook_pos, hook_ori = hook

    # The head model is attached at the head bone, so its own model space is
    # the bone's space: rotate the eye by the bone's orientation, then offset.
    m.eye_left = v_add(hook_pos, q_rotate(hook_ori, head.position_of(eye_nodes[0])))
    m.eye_right = v_add(hook_pos, q_rotate(hook_ori, head.position_of(eye_nodes[1])))
    m.eye_mid = v_mid(m.eye_left, m.eye_right)
    return m


# --- appearance.2da mapping --------------------------------------------------

# modeltype B builds the body from modela..modeln, picked by the armour class
# the character has equipped; `race` is empty for those rows. Every other
# modeltype puts a single fixed model in `race`. The player characters are all
# type B, so a tool that reads only `race` measures the NPCs and misses the PC
# entirely -- which is exactly the mistake this column list exists to prevent.
ARMOUR_COLUMNS = ["model" + c for c in "abcdefghijklmn"]


class Appearance(object):
    def __init__(self, row, label, bodies, head, head_bone, model_type,
                 height, camera_height_offset):
        self.row = row
        self.label = label
        self.bodies = bodies          # list of (armour letter, model resref)
        self.head = head
        self.head_bone = head_bone
        self.model_type = model_type
        self.height = height
        self.camera_height_offset = camera_height_offset

    @property
    def body(self):
        """The default look: no armour equipped for type B, `race` otherwise."""
        return self.bodies[0][1] if self.bodies else ""


def read_appearances(res):
    appearance = res.table("appearance")
    heads = res.table("heads")
    if appearance is None or heads is None:
        raise SystemExit("appearance.2da or heads.2da not found")

    out = []
    for row in range(appearance.rows):
        model_type = appearance.get(row, "modeltype").strip()
        bodies = []
        race = appearance.get(row, "race").strip()
        if race:
            bodies.append(("race", race))
        if model_type.upper() == "B":
            for column in ARMOUR_COLUMNS:
                model = appearance.get(row, column).strip()
                if model:
                    bodies.append((column[-1], model))
        if not bodies:
            continue

        head = ""
        normal = appearance.get(row, "normalhead").strip()
        if normal.isdigit():
            index = int(normal)
            if index < heads.rows:
                head = heads.get(index, "head").strip()
        out.append(Appearance(
            row=row,
            label=appearance.get(row, "label").strip(),
            bodies=bodies,
            head=head,
            head_bone=appearance.get(row, "headbone").strip() or DEFAULT_HEAD_BONE,
            model_type=model_type,
            height=appearance.get(row, "height").strip(),
            camera_height_offset=appearance.get(row, "cameraheightoffset").strip(),
        ))
    return out


def is_player_row(app):
    """The rows a PC can actually wear: a body built from armour, plus a head."""
    return app.model_type.upper() == "B" and bool(app.head)


# --- reporting ---------------------------------------------------------------

def fmt_vec(v):
    return "(%8.4f %8.4f %8.4f)" % v if v else "%22s" % "--"


def report(res, rows, verbose=False):
    print("Eye measurements -- model space, metres, Z up, Y forward")
    print("game: %s" % res.game)
    if res.override_dirs:
        print("override: %s" % ", ".join(res.override_dirs))
    print()
    header = "%-5s %-22s %-4s %-9s %-9s %9s %9s %9s" % (
        "row", "label", "slot", "body", "head", "eye Z", "eye Y", "camhook Z")
    print(header)
    print("-" * len(header))

    seen = {}
    problems = []
    for app, slot, m in rows:
        pair = (m.body.lower(), (m.head or "").lower())
        if pair in seen and not verbose:
            continue
        seen[pair] = m
        print("%-5d %-22.22s %-4s %-9.9s %-9.9s %9s %9s %9s" % (
            app.row, app.label, slot, m.body, m.head or "-",
            "%.4f" % m.eye_height if m.eye_height is not None else "--",
            "%.4f" % m.eye_mid[1] if m.eye_mid else "--",
            "%.4f" % m.camerahook[2] if m.camerahook else "--"))
        if m.note:
            problems.append((app.row, app.label, m.note))

    print()
    print("distinct body+head pairs measured: %d" % len(seen))
    measured = [m for m in seen.values() if m.eye_height is not None]
    if measured:
        heights = sorted(m.eye_height for m in measured)
        print("eye height: min %.4f  max %.4f  spread %.4f m"
              % (heights[0], heights[-1], heights[-1] - heights[0]))
    if problems:
        print()
        print("rows without a measurement (%d):" % len(problems))
        for row, label, note in problems[:20]:
            print("  %-5d %-22.22s %s" % (row, label, note))
    return sanity_check(measured)


def sanity_check(measurements):
    """Refuse to look right when the parse is wrong.

    A human eye height is 1.4-2.0 m and the two eye nodes are symmetric about
    the model's centre line. If either fails, the parser is broken -- not the
    model -- and the numbers must not be trusted downstream.
    """
    failures = []
    for m in measurements:
        if not 1.0 <= m.eye_height <= 2.5:
            failures.append("%s+%s: eye height %.4f outside 1.0..2.5 m"
                            % (m.body, m.head, m.eye_height))
        if m.eye_left and m.eye_right:
            offset = abs(m.eye_left[0] + m.eye_right[0]) / 2.0
            if offset > 0.01:
                failures.append("%s+%s: eyes not symmetric in X (centre off by %.4f m)"
                                % (m.body, m.head, offset))
            ipd = m.interpupillary
            if not 0.03 <= ipd <= 0.12:
                failures.append("%s+%s: interpupillary distance %.4f m implausible"
                                % (m.body, m.head, ipd))
    print()
    if failures:
        print("SANITY CHECK FAILED (%d):" % len(failures))
        for f in failures[:20]:
            print("  %s" % f)
        return False
    print("sanity check passed: %d measurements, eye heights plausible, "
          "eye nodes symmetric" % len(measurements))
    return True


def write_csv(path, rows):
    directory = os.path.dirname(os.path.abspath(path))
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)
    written = set()
    with open(path, "w", encoding="ascii", newline="") as fh:
        fh.write("body,head,head_bone,eye_nodes,eye_x,eye_y,eye_z,eyel_x,eyer_x,"
                 "camerahook_x,camerahook_y,camerahook_z,"
                 "headhook_x,headhook_y,headhook_z,ipd,note\n")
        for _app, _slot, m in rows:
            key = (m.body.lower(), (m.head or "").lower())
            if key in written:
                continue
            written.add(key)

            def cell(v, i):
                return "%.5f" % v[i] if v else ""

            fh.write("%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" % (
                m.body, m.head or "", m.head_bone, m.eye_nodes,
                cell(m.eye_mid, 0), cell(m.eye_mid, 1), cell(m.eye_mid, 2),
                cell(m.eye_left, 0), cell(m.eye_right, 0),
                cell(m.camerahook, 0), cell(m.camerahook, 1), cell(m.camerahook, 2),
                cell(m.headhook, 0), cell(m.headhook, 1), cell(m.headhook, 2),
                "%.5f" % m.interpupillary if m.interpupillary else "",
                m.note))
    print("wrote %s (%d rows)" % (path, len(written)))


def dump_nodes(res, spec):
    """--nodes model:name,name -- print world transforms of named nodes."""
    resref, _, wanted = spec.partition(":")
    model = res.model(resref)
    if model is None:
        raise SystemExit("model not found: %s" % resref)
    names = [n for n in wanted.split(",") if n] or sorted(model.nodes)
    print("%s: %d nodes" % (resref, len(model.order)))
    for name in names:
        got = model.world(name)
        if got is None:
            print("  %-16s --" % name)
        else:
            pos, ori = got
            print("  %-16s pos %s  ori (%7.4f %7.4f %7.4f %7.4f)"
                  % (name, fmt_vec(pos), ori[0], ori[1], ori[2], ori[3]))


def verify_against_pykotor(res, resrefs):
    """Cross-check the parser against an independent MDL reader, when present."""
    try:
        from pykotor.resource.formats.mdl import read_mdl
    except ImportError:
        print("PyKotor not installed -- skipping cross-check "
              "(it is not a project dependency)")
        return True
    worst = 0.0
    checked = 0
    failed = False
    for resref in resrefs:
        mine = res.model(resref)
        theirs = read_mdl(res.get(resref, RES_MDL, "mdl"))
        if mine is None or theirs is None:
            continue
        # Compare tree-positionally, not by name: s_male02 has two nodes called
        # w_Longsword, and a name lookup would compare one reader's first
        # against the other's second and report a 2.4 cm "disagreement" that is
        # really just two different nodes.
        theirs_nodes = list(theirs.all_nodes())
        if len(theirs_nodes) != len(mine.order):
            print("  %s: node COUNT differs -- mine %d, PyKotor %d"
                  % (resref, len(mine.order), len(theirs_nodes)))
            failed = True
            continue
        by_name = {}
        for node in theirs_nodes:
            by_name.setdefault(node.name.lower(), []).append(node)
        seen = {}
        for node in mine.order:
            key = node.name.lower()
            index = seen.get(key, 0)
            seen[key] = index + 1
            peers = by_name.get(key)
            if not peers or index >= len(peers):
                print("  %s: node %s missing from PyKotor's tree" % (resref, node.name))
                failed = True
                continue
            other = peers[index]
            delta = max(abs(node.position[0] - other.position.x),
                        abs(node.position[1] - other.position.y),
                        abs(node.position[2] - other.position.z))
            if delta > worst:
                worst = delta
            checked += 1
    print("cross-check vs PyKotor: %d nodes, worst local-position delta %.3e m"
          % (checked, worst))
    if worst > 1e-6 or failed:
        print("CROSS-CHECK FAILED: readers disagree")
        return False
    print("cross-check passed")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--game", default=None, help="game directory")
    parser.add_argument("--report", action="store_true", help="print the eye table")
    parser.add_argument("--csv", default=None, help="write the measurements here")
    parser.add_argument("--appearance", type=int, default=None,
                        help="measure only this appearance.2da row")
    parser.add_argument("--all-rows", action="store_true",
                        help="every appearance row, not just player-style ones")
    parser.add_argument("--armour", action="store_true",
                        help="measure every armour variant (modela..modeln), "
                             "not just the default look")
    parser.add_argument("--nodes", default=None,
                        help="dump node transforms, e.g. pmbam:camerahook,headhook")
    parser.add_argument("--verify", action="store_true",
                        help="cross-check the MDL parser against PyKotor if installed")
    args = parser.parse_args()

    res = Resources(game=args.game)

    if args.nodes:
        dump_nodes(res, args.nodes)
        return 0

    appearances = read_appearances(res)
    if args.appearance is not None:
        appearances = [a for a in appearances if a.row == args.appearance]
        if not appearances:
            raise SystemExit("appearance row %d has no model" % args.appearance)
    elif not args.all_rows:
        appearances = [a for a in appearances if is_player_row(a)]

    rows = []
    for app in appearances:
        variants = app.bodies if args.armour else app.bodies[:1]
        for slot, body in variants:
            rows.append((app, slot, measure(res, body, app.head, app.head_bone)))

    ok = True
    if args.report or not (args.csv or args.verify):
        ok = report(res, rows)
    if args.csv:
        write_csv(args.csv, rows)
    if args.verify:
        ok = verify_against_pykotor(res, ["pmbam", "pmhc01", "pfbam", "s_male02"]) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

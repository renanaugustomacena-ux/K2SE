"""Generate src/eyetable_generated.h from the model measurements.

    python tools/gen_eyetable.py [--check]

--check exits non-zero when the header on disk differs from what the models
imply, so verify_offsets.py can prove the compiled table has not drifted from
the game's own data -- the same contract gen_offsets.py has for addresses.

The table is keyed by appearance.2da row because that is what the DLL can cheaply
read at runtime (CSWSCreature+0x1184, already used by npcvariety.cpp). Only the
default look is emitted: armour swaps the body model and moves the eyes by up to
2.5 cm, and that is resolved against the live model in fpcam.cpp rather than
multiplied into this table.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import model_measure  # noqa: E402

HEADER_PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "src", "eyetable_generated.h")

BANNER = """\
// =============================================================================
// GENERATED FILE -- DO NOT EDIT.
//
//   source: appearance.2da + heads.2da + the MDL node trees in models.bif
//   regen : python tools/gen_eyetable.py
//
// Where the eyes are, per appearance.2da row, in MODEL space: metres, Z up,
// Y forward, relative to the model origin. Measured from the rest pose --
// eye point = headhook(body) composed with midpoint(eyeL, eyeR)(head).
//
// Why this exists: the first-person camera used to sit at a hardcoded 1.65 m
// for everyone. The male PC's eyes are at 1.6726 and the female PC's at
// 1.5886, so that one constant was 1.9 cm low for half the cast and 6.0 cm
// high for the other half.
//
// camerahook is recorded alongside because it is the fallback for the eleven
// masked faces (Visas, Darth Revan, the Dark Jedi) whose head models have no
// eye nodes at all. Note it is NOT interchangeable with the eye point: it sits
// 3.9-5.0 cm below and behind the eyes, and on heavy armour above them.
//
// These are REST-POSE values. In game the head is animated, so fpcam.cpp asks
// the engine for the live node position and uses these as the sanity bound and
// the static fallback. See k2-multi-fov/docs/eye-measurements.md.
// =============================================================================

#pragma once
#include <cstdint>

namespace k2se {
namespace eyetable {

struct Entry {
    uint16_t row;        // appearance.2da row
    float eyeForward;    // Y, metres, + is in front of the model origin
    float eyeHeight;     // Z, metres; 0 when the face has no eye nodes
    float hookHeight;    // camerahook Z, 0 when the body has no camerahook
};

"""

TAIL = """
constexpr int kCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

// The table is emitted in ascending row order, so a binary search is valid.
inline const Entry* Find(int row) {
    int lo = 0;
    int hi = kCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int at = static_cast<int>(kEntries[mid].row);
        if (at == row) return &kEntries[mid];
        if (at < row) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return nullptr;
}

}  // namespace eyetable
}  // namespace k2se
"""


def build():
    res = model_measure.Resources()
    appearances = model_measure.read_appearances(res)

    best = {}
    for app in appearances:
        if app.row in best:
            continue
        m = model_measure.measure(res, app.body, app.head, app.head_bone)
        if m.eye_mid is None and m.camerahook is None:
            continue
        best[app.row] = m

    out = [BANNER, "constexpr Entry kEntries[] = {\n"]
    for row in sorted(best):
        m = best[row]
        out.append("    {%3d, %10.5ff, %10.5ff, %10.5ff},\n" % (
            row,
            m.eye_mid[1] if m.eye_mid else 0.0,
            m.eye_mid[2] if m.eye_mid else 0.0,
            m.camerahook[2] if m.camerahook else 0.0))
    out.append("};\n")
    out.append(TAIL)
    return "".join(out), len(best)


def main():
    text, count = build()

    if "--check" in sys.argv:
        if not os.path.exists(HEADER_PATH):
            print("FAIL  src/eyetable_generated.h is missing; run tools/gen_eyetable.py")
            return 1
        with open(HEADER_PATH, "r", encoding="utf-8", newline="") as fh:
            current = fh.read()
        if current.replace("\r\n", "\n") != text:
            print("FAIL  src/eyetable_generated.h is stale relative to the game's models")
            print("      run: python tools/gen_eyetable.py")
            return 1
        print("OK    src/eyetable_generated.h matches the measured models (%d rows)" % count)
        return 0

    with open(HEADER_PATH, "w", encoding="ascii", newline="\n") as fh:
        fh.write(text)
    print("wrote %s (%d appearance rows)" % (HEADER_PATH, count))
    return 0


if __name__ == "__main__":
    sys.exit(main())

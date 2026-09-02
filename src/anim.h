#pragma once
#include <cstdint>

// ============================================================================
// Overlay animations on a client creature, the way PlayOverlayAnimation does
// it (routine 854, handler 0x0068F440, read on 2026-09-02):
//
//     CSWCAnimBase* base = CSWCCreature::GetAnimBase(client);     // 0x007ED830
//     uint16_t row = base->vtable[0xE0/4](code);                  // code -> animations.2da row
//     base->vtable[0x44/4](row, 1);                               // play that row as overlay
//
// The mapper is a fixed switch (code 0x290D -> row 567 = diveroll), so K2SE
// skips it and plays a 2DA row directly. That is also what makes a NEW row in
// animations.2da (a custom jump animation) reachable without touching the
// engine's code table.
// ============================================================================

namespace k2se {
namespace anim {

constexpr uint16_t kRowDiveRoll = 567;   // animations.2da "diveroll", overlay=1
constexpr uint16_t kCodeDiveRoll = 0x290D;

// The creature's anim base object (CSWCCreature::GetAnimBase), or null. SEH-guarded.
void* AnimBase(void* clientCreature);

// Plays animations.2da row `row` as an overlay on the client creature.
// `flag` is the second argument the engine passes (1 in PlayOverlayAnimation).
// Returns false if the anim base could not be reached or the call faulted.
bool PlayRow(void* clientCreature, uint16_t row, int flag = 1);

// Runs the engine's own code->row mapper first (for codes that exist in it).
bool PlayCode(void* clientCreature, uint16_t code);

// Diagnostics: the mapped row for a code, or -1.
int MapCode(void* clientCreature, uint16_t code);

}  // namespace anim
}  // namespace k2se

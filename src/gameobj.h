#pragma once
#include <cstdint>

// ============================================================================
// Turning a script OBJECT id into a live game object.
//
// This is the boundary where K2SE stops being self-contained. Everything up to
// now touched addresses whose meaning was established by disassembling this
// executable. Here we also rely on STRUCT OFFSETS -- claims about an object's
// runtime layout that cannot be checked by reading the file, only by being
// right. They were imported from a third-party database whose KOTOR 2 rows were
// auto-ported from another build.
//
// So the posture is different from the rest of K2SE: assume every pointer might
// be wrong. Each hop is range-checked, every dereference runs under a structured
// exception handler, and a failure returns a sentinel the script can test
// instead of taking the game down. A creature accessor that returns "I don't
// know" is recoverable; one that reads a wild pointer is not.
// ============================================================================

namespace k2se {
namespace gameobj {

// The CServerExoApp singleton, or null if the game has not built it yet.
void* ServerExoApp();

// Script OBJECT id -> CSWSCreature*, or null if the id names something that is
// not a creature (or nothing at all). OBJECT_INVALID is rejected up front.
void* CreatureFromObjectId(uint32_t objectId);

// CSWSCreature* -> CSWSCreatureStats*, or null. Reads through kCreatureOffStats,
// the least-verified step in the chain.
void* CreatureStats(void* creature);

// nAbility is NWScript's ABILITY_* (0..5). False if the index is out of range or
// the read faults.
bool AbilityBase(void* stats, int ability, int* out);

// Engine calls. Each returns false rather than a wrong answer when the pointer
// chain could not be walked.
bool SkillRankBase(void* stats, int skill, int* out);
bool HasFeat(void* stats, int feat, int* out);
bool HasSpell(void* stats, int spell, int* out);

// True if the pointer is plausibly a heap object rather than a small integer or
// a sentinel that slipped through. Cheap and catches the common failure.
bool LooksLikePointer(const void* p);

}  // namespace gameobj
}  // namespace k2se

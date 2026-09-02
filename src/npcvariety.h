#pragma once
#include <cstdint>

// ============================================================================
// NPC variety: generic NPCs stop sharing one face.
//
// The server loads a creature's look in CSWSCreature::LoadAppearance
// (0x0057FCE0, thiscall, one argument), reading the appearance.2da row from
// `word [this+0x1184]`. It is called from four places (creature creation
// 0x00576AAF, SetAppearanceType 0x00585241, 0x0058536E, 0x0065A9DD); all four
// call sites are redirected to a pass-through that, for rows belonging to a
// "family" of interchangeable looks (same body model, different head: the
// Commoner / Czerka / Republic soldier and officer rows of the stock 2DA),
// picks another row of the same family before the original runs. The choice is
// deterministic per creature (hashed from its object id), so an NPC keeps its
// face across saves and reloads. Party members and unique NPCs use rows outside
// the families and are never touched.
// ============================================================================
namespace k2se {
namespace npcvariety {

bool Install();
void Remove();
int Status();   // bit 0 installed, bits 8.. = swaps so far

}  // namespace npcvariety
}  // namespace k2se

#pragma once
#include <cstdint>

// ============================================================================
// Multi-FOV (k2-multi-fov): situation-dependent field of view for the scene
// camera -- exploration, combat, sprint -- plus hotkeys and a script API.
//
// How the engine does it (2026-09-02 disassembly, data/k2se_addresses.csv):
//   the Aurora class `Camera` (RTTI .?AVCamera@@, vtable 0x0098C45C, 37 slots)
//   keeps the vertical FOV in degrees at +0x204 (default 45.0), near/far at
//   +0x210/+0x214, and an optional FOV animation at +0x208. Slot 2 (0x0047F320)
//   applies the projection: it is the ONLY caller of gluPerspective in the exe
//   and it also derives the culling frustum from +0x204, which is why the FOV
//   has to change in the field, not in the GL call. Slot 3 (0x00480E40) is
//   Update(dt): it advances the FOV animation, then updates the transform.
//
// The hook: one vtable-slot swap (slot 3), the same one-dword technique as the
// VM and player-controller hooks. After the engine's own Update has run, the
// hook reads +0x204, remembers any value it did not write itself as the game's
// "vanilla" FOV, and in gameplay frames writes the smoothed target FOV. Outside
// gameplay (menus, dialogue, cutscenes, minigames -- detected as "the player
// controller has not ticked recently") it puts the vanilla value back, so
// authored dialogue cameras and GUI previews are never touched.
// ============================================================================
namespace k2se {
namespace fov {

enum StatusBits : int {
    kInstalled = 1 << 0,
    kActive = 1 << 1,        // a K2SE value is currently written in the camera
    kScriptOverride = 1 << 2,
    kRefused = 1 << 3,
};

// Reads [FOV] from k2se_movement.ini (config::Load must have run) and swaps the
// camera Update slot. Returns false when disabled or refused.
bool Install();
void Remove();
int Status();

// Script API. degrees <= 0 clears the override. seconds <= 0 keeps the ini smoothing.
bool SetOverride(float degrees, float seconds);
float Current();   // the FOV K2SE last wrote, or the vanilla one when inactive

}  // namespace fov
}  // namespace k2se

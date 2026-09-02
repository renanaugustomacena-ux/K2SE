#pragma once
#include <cstdint>

// ============================================================================
// Movement features: sprint, crouch, roll, jump (K2 Jump / Crouch / Sprint).
//
// All four ride on the player controller's own frame:
//
//   CSWPlayerControlCamRelative::Update (0x00865830)
//     -> GetMaxSpeed (0x00867B40)          <- redirected at 3 call sites
//   CSWSCreature drive mover (0x005C6340)
//     -> CSWSObject::SetPosition (0x00543F50) <- redirected at 3 call sites
//
// GetMaxSpeed runs only while the player is being driven, so keys are never
// read in dialogues, menus, cutscenes or minigames. SetPosition inside the
// mover is where the engine commits the driven creature's position each frame;
// the jump replaces the Z (and later X/Y) it is about to commit.
//
// Everything is configured by k2se_movement.ini and OFF without it.
// ============================================================================

namespace k2se {
namespace movement {

// Status bits reported to scripts by K2SE_GetMovementStatus.
enum StatusBits : int {
    kInstalled = 1 << 0,
    kSprinting = 1 << 1,
    kCrouching = 1 << 2,
    kAirborne = 1 << 3,
    kRolling = 1 << 4,
    kSprintEnabled = 1 << 5,
    kCrouchEnabled = 1 << 6,
    kJumpEnabled = 1 << 7,
    kRollEnabled = 1 << 8,
};

// Reads the ini, registers keys, redirects the call sites of the enabled
// features. Returns true if at least one feature is live. Safe from DllMain
// AFTER the fingerprint passed (it writes .text).
bool Install();
void Remove();

int Status();

// Script-facing controls (routines 889+).
void SetSprintFactor(float factor);   // 1.0 disables the boost, keeps the key
bool SetCrouch(bool on);
bool RequestJump();
bool RequestRoll();

// For other modules (fov.cpp): how many times the player controller's Update
// has run -- it only runs while the player is being driven, so a changing count
// means "gameplay" as opposed to menus, dialogue, cutscenes or minigames.
uint32_t UpdateCount();
bool PlayerInCombat();

}  // namespace movement
}  // namespace k2se

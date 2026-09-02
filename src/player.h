#pragma once
#include <cstdint>

// ============================================================================
// The driven player: controller -> server creature -> client creature.
//
// Every pointer here is resolved from the engine's own lookups (never cached
// across frames, because a load screen invalidates all of them) and every read
// through a struct offset is SEH-guarded. The offsets come from the 2026-09-02
// disassembly (docs/2026-09-02-studio-movimento-collisioni.md):
//
//   CSWPlayerControlCamRelative  +0x04 player id, +0x08 camera, +0x0C enabled,
//                                +0x10 up_down, +0x14 left_right, +0x18 walking
//   CSWSCreature (server)        +0x94 position, +0xA0 orientation,
//                                +0x380 CPathfindInformation*, +0x520 combat round,
//                                +0x1114 move flags (word), +0x1120 mode flags
//                                (bit 0 = stealth), +0x1198 stats
//   CSWCCreature (client)        +0x224 CSWCCreatureAppearance*, +0x2EC flags
//                                (word, bit 0 = stealth), +0x3C8 drive speed
//   CSWCCreatureAppearance       +0x58 driveaccl, +0x5C drivemaxspeed, +0x60 stealth speed
// ============================================================================

namespace k2se {
namespace player {

struct Refs {
    void* controller;
    uint32_t playerId;
    void* serverCreature;
    void* clientCreature;
    void* appearance;   // may be null even when the creature is valid
};

// Resolves everything from the controller `this`. Returns false (and logs the
// first failure of each kind) if any hop is missing. Cheap enough per frame.
bool Resolve(void* controller, Refs* out);

// Same, starting from the driven CLIENT creature (what the controller passes to
// SetDriveSpeed every frame). `controller` may be null when it is not known yet;
// the walking/axes readers then return false.
bool ResolveFromClient(void* clientCreature, void* controller, Refs* out);

void* ServerApp();   // CAppManager+0x08
void* ClientApp();   // CAppManager+0x04
void* ServerCreatureById(uint32_t id);
void* ClientCreatureById(uint32_t id);

// --- guarded field access (all return false if the read faults) ------------
bool ControllerEnabled(void* controller, bool* out);
bool ControllerWalking(void* controller, bool* out);
bool ControllerAxes(void* controller, float* upDown, float* leftRight);

bool ServerPosition(void* srv, float out[3]);
bool ServerOrientation(void* srv, float out[3]);
bool ServerInCombat(void* srv, bool* out);          // combat round pointer non-null
bool ServerStealthMode(void* srv, bool* out);       // mode flags bit 0
bool ServerMoveFlags(void* srv, uint16_t* out);

bool ClientStealthBit(void* cli, bool* out);
bool SetClientStealthBit(void* cli, bool on);       // the ONE write this module performs
bool ClientDriveSpeed(void* cli, float* out);
bool AppearanceFloat(void* appearance, uint32_t offset, float* out);

// True if the pointer is plausibly a heap object (same test as gameobj.cpp).
bool LooksLikePointer(const void* p);

}  // namespace player
}  // namespace k2se

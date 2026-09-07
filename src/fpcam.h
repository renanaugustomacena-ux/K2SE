#pragma once
#include <cstdint>

// ============================================================================
// First-person camera anchored to the character's eyes (k2-multi-fov).
//
// The problem it solves: camera.cpp's first-person preset used a hardcoded
// height of 1.65 m for every character. Measured against the actual models
// (tools/model_measure.py), the male PC's eyes are at 1.6726 and the female
// PC's at 1.5886 -- 1.9 cm too low for one and 6.0 cm too high for the other,
// and no single constant can cover the 1.32-1.71 m range across the cast.
//
// Two sources of truth, in order of preference:
//
//   1. LIVE  -- ask the engine where the eye node is this frame. The head is
//      animated, so this is the only source that breathes. The path, verified
//      by disassembly on 2026-09-08 (data/k2se_addresses.csv):
//
//          clientCreature+0x224            (player::Refs::appearance)
//            -> +0x3C                      CSWCAnimBase
//            -> vtable[0x98](0xFF, 1)      CSWCAnimBase::GetModel  0x00861690
//            -> vtable[0x10C](name)        FindNodeByName
//
//      That is exactly what CSWCCreature::GetCameraHookNode (vtable slot +0xD0,
//      0x00775930 -> 0x0085D260) does for "CAMERAHOOK". What is NOT yet
//      established is the layout of the node object, so Probe() hunts for the
//      position field at runtime and validates it against (2).
//
//   2. REST -- src/eyetable_generated.h, the measured rest-pose eye point per
//      appearance.2da row. Always available, costs nothing, and does not move
//      with the animation. This is the fallback, and it is also the oracle that
//      tells the live path when it has resolved something implausible.
//
// Stabilisation: a raw head bone is nauseating to look through while running.
// The anchor is split into a slow mean and the oscillation around it; BobScale
// scales only the oscillation, so 0.0 is a perfectly still head, 1.0 is the
// raw bone, and the default 0.25 keeps the character feeling alive.
//
// camerahook is deliberately NOT the primary anchor. It is a child of the model
// root, so it does not move with the animation at all, and it sits 3.9-5.0 cm
// below and behind the eyes -- on heavy armour (PMBKM) above them. It is the
// right fallback for the eleven masked faces that have no eye nodes, and the
// wrong choice for everyone else.
// ============================================================================
namespace k2se {
namespace player {
struct Refs;
}

namespace fpcam {

enum Anchor : int {
    kAnchorEyes = 0,        // eyeLA/eyeRA midpoint -- the default
    kAnchorHeadHook = 1,    // the head attachment node + the measured eye offset
    kAnchorCameraHook = 2,  // BioWare's own node; static, but always present
    kAnchorStatic = 3,      // the measured rest pose, no live query at all
};

bool Install();
void Remove();
int Status();   // bit 0 installed, bit 1 measurement found, bit 2 live anchor live

// Called by movement.cpp once per player-controller frame.
void OnGameplayFrame(const player::Refs& refs, float dt);

// The eye point for the character currently being driven, in model space
// (metres, Z up, Y forward). False when nothing is known about them yet.
// camera.cpp uses this for the first-person preset's height.
bool EyeOffset(float* forward, float* height);

// Script API: the live eye point, and the bob amount.
bool GetEyePosition(float out[3]);
void SetBobScale(float scale);

}  // namespace fpcam
}  // namespace k2se

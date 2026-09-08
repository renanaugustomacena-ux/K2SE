#pragma once
#include <cstdint>

// ============================================================================
// A real first-person camera, built rather than negotiated (k2-multi-fov).
//
// Every previous attempt tried to persuade the game's chase camera to behave
// like a first-person one: set the style height to the measured eye height, set
// the distance to zero, aim the pitch. It cannot work. The chase camera orbits a
// target and looks AT it, so the camera always ends up behind the head -- which
// is exactly what it looked like -- and its yaw belongs to the game's camera
// controls, so there is no free look.
//
// So this does not use the chase camera at all. The engine's view matrix reaches
// OpenGL through glMultMatrixf, and K2SE already patches that import table. This
// module intercepts the scene's view matrix and substitutes its own, built from
//
//     the eye position measured out of the character's model, and
//     a yaw and pitch this module owns, driven by the mouse.
//
// The engine keeps computing its camera; the result is simply not used while
// first person is active. That is the point: the rule is replaced, not tuned.
//
// Identifying the scene's view matrix is done by checking, not by guessing which
// call it is. A view matrix's rotation is orthonormal, so the camera's world
// position can be recovered from it as -(R^T * t); when that lands within a few
// metres of the player, this is the scene view and nothing else. GUI and shadow
// matrices never satisfy that test.
// ============================================================================
namespace k2se {
namespace fpview {

bool Install();
void Remove();
int Status();   // bit 0 installed, bit 1 hooked, bit 2 view replaced this session

// movement.cpp, once per gameplay frame: reads the mouse and refreshes the eye
// point. `active` is true only while the first-person view is selected.
void OnGameplayFrame(bool active, const float worldEye[3], float facingRadians);

// fov.cpp, from its Camera::ApplyProjection hook -- once per rendered frame,
// for the scene camera and nothing else. This is the anchor that says which
// modelview matrix is the view: the first one after it. Guessing from the
// contents instead matched every prop near the player and replaced 39,488
// object transforms in one session.
void OnCameraApply();

// Where the camera is looking, for anything that wants to align with it
// (movement, aiming). Yaw is radians in the game's facing convention.
float Yaw();
float Pitch();
bool Replacing();

}  // namespace fpview
}  // namespace k2se

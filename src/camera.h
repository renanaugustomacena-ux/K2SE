#pragma once
#include <cstdint>

// ============================================================================
// Camera views (k2-multi-fov): cycle between three chase-camera presets --
// near, far, first person -- with one key, on top of the game's own camera
// style. The game's free-look key (CapsLock) freezes the player; these presets
// keep the normal chase camera, so movement, combat and targeting keep working.
//
// Engine facts (2026-09-02 disassembly, data/k2se_addresses.csv):
//   the game camera object (ctor 0x007DD380) loads camerastyle.2da through
//   LoadCameraStyle(int row) (0x007E1CA0): DISTANCE -> +0x16C, SPEED -> +0x170,
//   PITCH -> +0x178, HEIGHT -> +0x17C, TILTSPEED -> +0x190, style row -> +0x5C,
//   a distance copy -> +0x88, VIEWANGLE -> +0x8C and to the Aurora Camera's FOV
//   through [this+0x1C]->vtable[1]() -> Camera::SetFOV. The loader runs from the
//   ctor (via 0x007E1BD0 at 0x007DD5D7), from the area camera setup (0x007DDC52)
//   and from the pending-style path (0x007DE7D6, global 0x00A108F8 = the row a
//   script asked for). The chase camera reads +0x16C/+0x17C/+0x178 when it
//   places itself (0x007E0D25, 0x007E22EF, 0x007E2339, ...).
//
// The hook: the three `call LoadCameraStyle` sites are redirected (E8 rel32,
// verified first) to a pass-through that remembers the camera object and the
// values the game loaded (the "vanilla style"). Each gameplay frame the active
// preset's distance/height/pitch are written into the object; its FOV and,
// for first person, a larger near plane go through fov.cpp. Preset 0 = the
// game's own style (nothing written).
// ============================================================================
namespace k2se {
namespace camera {

enum View : int {
    kViewVanilla = 0,
    kViewNear = 1,
    kViewFar = 2,
    kViewFirstPerson = 3,
};

bool Install();
void Remove();
int Status();          // bit 0 installed, bits 1..2 = active view

// Called by movement.cpp once per player-controller frame (= gameplay frame).
void OnGameplayFrame();

// For fov.cpp: the active preset's FOV in degrees (false when the vanilla style
// is active) and the near-plane override (0 = none).
bool PresetFov(float* degrees);
float NearPlaneOverride();

// Script API (routines 896/897).
bool SetView(int view);
int GetView();

}  // namespace camera
}  // namespace k2se

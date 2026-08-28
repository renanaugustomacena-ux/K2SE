#pragma once
#include <cstdint>

// ============================================================================
// Runtime fog control (DESIGN.md M5).
//
// The problem M5 identifies: on the Aspyr build the ARB fragment programs never
// reference `state.fog`, so setting GL fog state produces no visible change at
// all. Writing fog parameters alone would be a silent no-op -- which is the real
// Aspyr fog bug, not a K2SE limitation.
//
// So there are two halves, and both are needed:
//
//   1. Make the shaders honour fog. Intercept glProgramStringARB and insert
//      `OPTION ARB_fog_linear;` after the `!!ARBfp1.0` header of each fragment
//      program. One token, no per-shader knowledge, and it turns the existing
//      fixed-function fog pipeline back on. Rewriting each shader body would
//      mean knowing what every one of them does.
//
//   2. Own the fog parameters. Intercept glFogf/glFogfv/glFogi through the
//      import table so that whatever the engine sets, K2SE's override wins.
//
// glProgramStringARB is an extension entry point, so it has no import slot --
// the game fetches it through wglGetProcAddress, which IS imported (slot
// 0x0098632C). Patching that one dword and handing back a wrapper is the whole
// interception, and it is the same shape as the vtable-slot swap K2SE already
// uses for the VM: one pointer, atomic, trivially reversible.
//
// OPT-IN. Everything here is disabled unless a marker file named `k2se_fog.txt`
// sits next to the game executable. This code runs on the render thread and has
// not been confirmed in a live session; the rest of K2SE has. A subsystem that
// might destabilise a working DLL does not get to be on by default -- that is
// the same posture as DESIGN.md 3.6's "at the first doubt, install nothing".
// ============================================================================

namespace k2se {
namespace glhook {

// Bit flags returned by Status(), and reported to scripts by K2SE_GetFogStatus.
enum StatusBits : int {
    kDisabled = 0,       // no marker file; nothing was touched
    kInstalled = 1 << 0, // the import-table hooks are in place
    kShaderPatched = 1 << 1,  // at least one fragment program got the fog OPTION
    kOverrideActive = 1 << 2, // a script has set fog parameters
    kRefused = 1 << 3,   // enabled, but the imports did not look as expected
};

// Reads the marker file and, if present, patches the import slots. Safe to call
// from DllMain: it is VirtualProtect plus a few dword stores, nothing more.
// Returns false if fog support is off or could not be installed.
bool Install();

// Restores every patched slot.
void Remove();

int Status();

// --- script-facing controls -------------------------------------------------
// All no-ops when fog support is not installed, so a mod calling them on a
// machine without the marker file degrades quietly instead of misbehaving.

void SetEnabled(bool enabled);
void SetRange(float start, float end);
void SetColor(float r, float g, float b);

}  // namespace glhook
}  // namespace k2se

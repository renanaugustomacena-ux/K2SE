#pragma once
#include <cstdint>

// ============================================================================
// The render extension layer (k2-graphics).
//
// What the engine actually is, before anyone promises otherwise: OpenGL 1.x
// fixed-function plus ARB_fragment_program assembly shaders. There is no
// programmable vertex path in use, no framebuffer objects, no GLSL. So the two
// things that CAN be added to it are:
//
//   1. different fragment programs -- the engine hands every one of them to
//      glProgramStringARB, which glhook.cpp already intercepts. Dump them to
//      disk, edit the text, and the next run uses yours. That is what makes
//      the renderer moddable at all, and it is the foundation everything else
//      here sits on.
//
//   2. a post-process pass -- at SwapBuffers the finished frame is still in the
//      back buffer. Copy it into a texture, draw one full-screen quad through
//      our own fragment program, and the frame is graded and sharpened before
//      it reaches the screen.
//
// Both are opt-in and default off. Neither needs an extension the engine does
// not already use: it imports glCopyTexImage2D, glGenTextures, glTexParameteri,
// glBegin/glEnd, glOrtho and glPushAttrib itself, and resolves the ARB program
// entry points through wglGetProcAddress.
//
// What this layer cannot do, so that nobody plans around it: PBR, real-time
// shadows from a depth pass, or a modern renderer. Those need a vertex pipeline
// and render targets this engine does not have, and the honest route to them is
// a different renderer, not a hook. There is also no physics engine here to
// extend -- KOTOR has a walkmesh and nothing else.
//
// The DLL links only KERNEL32 (tools/check_dll.py enforces it), so every GL
// entry point is resolved at runtime through GetProcAddress on the already
// loaded opengl32.dll, never by linking against it.
// ============================================================================
namespace k2se {
namespace render {

enum Status : int {
    kOff = 0,
    kInstalled = 1 << 0,
    kSwapHooked = 1 << 1,
    kProgramReady = 1 << 2,
    kPostActive = 1 << 3,
    kRefused = 1 << 4,
};

bool Install();
void Remove();
int Status();

// Script API: colour grading, live.
void SetGrade(float brightness, float contrast, float saturation);
void SetTint(float r, float g, float b);
void SetSharpen(float amount);
bool SetEnabled(bool on);

// glhook.cpp asks these when a fragment program goes past.
bool WantShaderDump();
bool WantShaderReplace();
// Returns a replacement program for `source`, or null. `outLen` gets its length.
// The buffer stays valid until the next call.
const char* ReplacementFor(const char* source, int len, int* outLen);
void NoteShaderSeen(const char* source, int len);

}  // namespace render
}  // namespace k2se

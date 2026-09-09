#include "fpview.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "log.h"

namespace k2se {
namespace fpview {
namespace {

constexpr uint32_t GL_MODELVIEW = 0x1700;

// Import slots, verified by name against the import directory the same way
// glhook.cpp and render.cpp do.
constexpr uint32_t kIatMatrixMode = 0x009862A8;
constexpr uint32_t kIatLoadIdentity = 0x009862A4;
constexpr uint32_t kIatMultMatrixf = 0x009862F4;
constexpr uint32_t kIatPushMatrix = 0x009862F8;
constexpr uint32_t kIatTranslatef = 0x0098634C;
constexpr uint32_t kIatRotatef = 0x00986348;

using MatrixModeFn = void(__stdcall*)(uint32_t mode);
using LoadIdentityFn = void(__stdcall*)();
using MultMatrixfFn = void(__stdcall*)(const float* m);
using PushMatrixFn = void(__stdcall*)();
using TranslatefFn = void(__stdcall*)(float, float, float);
using RotatefFn = void(__stdcall*)(float, float, float, float);

MatrixModeFn g_origMatrixMode = nullptr;
LoadIdentityFn g_origLoadIdentity = nullptr;
MultMatrixfFn g_origMultMatrixf = nullptr;
PushMatrixFn g_origPushMatrix = nullptr;
TranslatefFn g_origTranslatef = nullptr;
RotatefFn g_origRotatef = nullptr;

struct Cfg {
    bool enabled = false;
    bool mouseLook = true;
    float sensitivity = 0.15f;
    bool invertY = false;
    float pitchLimit = 85.0f;   // degrees from level
    int traceLines = 0;         // >0: dump that many matrix calls, once
    // Off until the trace has actually shown where the view is finished.
    // Every previous build shipped a guess about that moment and every one of
    // them was wrong; this one measures first.
    bool impose = false;
};
Cfg g_cfg;

struct State {
    bool installed = false;
    bool hooked = false;
    bool active = false;          // first person is the current view
    uint32_t matrixMode = 0;
    bool armed = false;           // a MODELVIEW LoadIdentity just happened
    float eye[3] = {0, 0, 0};
    bool eyeValid = false;
    float yaw = 0.0f;             // radians, game facing convention
    float pitch = 0.0f;           // radians, + is up
    bool yawInitialised = false;
    uint32_t replaced = 0;
    uint32_t candidates = 0;
    bool reported = false;
    float view[16];
};
State g_st;

// --- user32, resolved at runtime (the DLL imports KERNEL32 only) --------------
using GetCursorPosFn = int(__stdcall*)(POINT*);
using SetCursorPosFn = int(__stdcall*)(int, int);
using GetForegroundWindowFn = HWND(__stdcall*)();
using GetClientRectFn = int(__stdcall*)(HWND, RECT*);
using ClientToScreenFn = int(__stdcall*)(HWND, POINT*);
using ShowCursorFn = int(__stdcall*)(int);

GetCursorPosFn g_getCursorPos = nullptr;
SetCursorPosFn g_setCursorPos = nullptr;
GetForegroundWindowFn g_getForegroundWindow = nullptr;
GetClientRectFn g_getClientRect = nullptr;
ClientToScreenFn g_clientToScreen = nullptr;
ShowCursorFn g_showCursor = nullptr;
bool g_userReady = false;

bool ResolveUser32() {
    if (g_userReady) return true;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) user32 = LoadLibraryA("user32.dll");
    if (!user32) return false;
    g_getCursorPos = reinterpret_cast<GetCursorPosFn>(GetProcAddress(user32, "GetCursorPos"));
    g_setCursorPos = reinterpret_cast<SetCursorPosFn>(GetProcAddress(user32, "SetCursorPos"));
    g_getForegroundWindow =
        reinterpret_cast<GetForegroundWindowFn>(GetProcAddress(user32, "GetForegroundWindow"));
    g_getClientRect = reinterpret_cast<GetClientRectFn>(GetProcAddress(user32, "GetClientRect"));
    g_clientToScreen =
        reinterpret_cast<ClientToScreenFn>(GetProcAddress(user32, "ClientToScreen"));
    g_showCursor = reinterpret_cast<ShowCursorFn>(GetProcAddress(user32, "ShowCursor"));
    g_userReady = g_getCursorPos && g_setCursorPos && g_getForegroundWindow &&
                  g_getClientRect && g_clientToScreen;
    if (!g_userReady) log::Write("fpview: user32 entry points missing -- no mouse look");
    return g_userReady;
}

// The centre of the game window, in screen coordinates. The cursor is warped
// back here every frame so the mouse never reaches an edge and the deltas keep
// coming; that is what makes look-around unbounded.
bool WindowCentre(POINT* out) {
    HWND hwnd = g_getForegroundWindow();
    if (!hwnd) return false;
    RECT rc = {};
    if (!g_getClientRect(hwnd, &rc)) return false;
    if (rc.right <= 0 || rc.bottom <= 0) return false;
    out->x = rc.right / 2;
    out->y = rc.bottom / 2;
    return g_clientToScreen(hwnd, out) != 0;
}

void UpdateMouse() {
    if (!g_cfg.mouseLook || !ResolveUser32()) return;
    POINT centre = {};
    if (!WindowCentre(&centre)) return;
    POINT now = {};
    if (!g_getCursorPos(&now)) return;

    const float dx = static_cast<float>(now.x - centre.x);
    const float dy = static_cast<float>(now.y - centre.y);
    g_setCursorPos(centre.x, centre.y);

    // A jump of hundreds of pixels is the cursor being placed by something else
    // -- alt-tab, a menu opening -- not a flick of the wrist.
    if (fabsf(dx) > 400.0f || fabsf(dy) > 400.0f) return;

    const float scale = g_cfg.sensitivity * 0.01f;
    g_st.yaw -= dx * scale;
    g_st.pitch += (g_cfg.invertY ? dy : -dy) * scale;

    const float limit = g_cfg.pitchLimit * 3.14159265f / 180.0f;
    if (g_st.pitch > limit) g_st.pitch = limit;
    if (g_st.pitch < -limit) g_st.pitch = -limit;

    const float twoPi = 6.28318531f;
    while (g_st.yaw > twoPi) g_st.yaw -= twoPi;
    while (g_st.yaw < -twoPi) g_st.yaw += twoPi;
}

// --- the view matrix ----------------------------------------------------------
// Column-major, as OpenGL wants. The world is Z up; the character's facing angle
// grows the same way the engine's own orientation vector does, so yaw 0 looks
// along +Y.
void BuildView(float* m) {
    const float cp = cosf(g_st.pitch);
    const float sp = sinf(g_st.pitch);
    const float cy = cosf(g_st.yaw);
    const float sy = sinf(g_st.yaw);

    const float f[3] = {sy * cp, cy * cp, sp};          // forward
    const float r[3] = {cy, -sy, 0.0f};                 // right (level)
    const float u[3] = {r[1] * f[2] - r[2] * f[1],      // up = right x forward
                        r[2] * f[0] - r[0] * f[2],
                        r[0] * f[1] - r[1] * f[0]};

    const float* e = g_st.eye;
    m[0] = r[0];  m[4] = r[1];  m[8]  = r[2];  m[12] = -(r[0]*e[0] + r[1]*e[1] + r[2]*e[2]);
    m[1] = u[0];  m[5] = u[1];  m[9]  = u[2];  m[13] = -(u[0]*e[0] + u[1]*e[1] + u[2]*e[2]);
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] =  (f[0]*e[0] + f[1]*e[1] + f[2]*e[2]);
    m[3] = 0.0f;  m[7] = 0.0f;  m[11] = 0.0f;  m[15] = 1.0f;
}

// The engine's view is IMPOSED OVER, not intercepted.
//
// Two earlier attempts tried to spot the view matrix among the modelview
// multiplications. The trace from 2026-09-09 shows why both failed. Right after
// Camera::ApplyProjection the frame goes:
//
//     glLoadIdentity  (MODELVIEW)
//     glMultMatrixf   t=(0 0 0)                <- rotation only, no position
//     glMultMatrixf   t=(-62610 59859 9170)    <- object world matrices
//
// The first matrix carries the camera's ORIENTATION and nothing else; the
// position is applied separately, by a glTranslatef this module never hooked.
// Replacing that rotation with a full view matrix meant the engine then
// translated our matrix as well, which threw the camera into orbit.
//
// So nothing is pattern-matched any more. The engine builds its view however it
// likes, and immediately before the first object is drawn the modelview is
// simply overwritten with ours: glLoadIdentity, then glMultMatrixf(ours). The
// first MODELVIEW glPushMatrix after the anchor is that moment -- the engine
// pushes before every object, so by then its view is complete and no geometry
// has been drawn with it yet.
//
// This depends on no assumption about HOW the view is constructed, which is the
// assumption that has failed three times.
bool g_anchored = false;          // ApplyProjection has run this frame
bool g_viewImposed = false;       // ours is already on the stack this frame

// The engine builds its view in three steps, in this order, measured from the
// 2026-09-09 trace rather than assumed:
//
//   stage 1  glLoadIdentity   (MODELVIEW)
//   stage 2  glMultMatrixf    rotation only, translation (0 0 0)
//   stage 3  glTranslatef     minus the camera position
//
// Substituting our own values into steps 2 and 3 replaces the camera exactly
// where the engine sets it, instead of fighting the result afterwards.
enum ViewStage { kIdle = 0, kIdentityLoaded, kRotationSeen, kComplete };
ViewStage g_stage = kIdle;
uint32_t g_frames = 0;
int g_trace = 0;

// The rotation half of our view, with no translation -- the exact shape the
// engine's own first matrix has.
void BuildViewRotation(float* m) {
    BuildView(m);
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
}

void ImposeView() {
    if (!g_origLoadIdentity || !g_origMultMatrixf) return;
    BuildView(g_st.view);
    g_origLoadIdentity();
    g_origMultMatrixf(g_st.view);
    ++g_st.replaced;
    if (g_st.replaced == 1)
        log::Writef("fpview: view imposed at the first object of the frame -- eye "
                    "(%d %d %d) mm, yaw %d deg, pitch %d deg",
                    static_cast<int>(g_st.eye[0] * 1000.0f),
                    static_cast<int>(g_st.eye[1] * 1000.0f),
                    static_cast<int>(g_st.eye[2] * 1000.0f),
                    static_cast<int>(g_st.yaw * 57.2957795f),
                    static_cast<int>(g_st.pitch * 57.2957795f));
}

void __stdcall HookMatrixMode(uint32_t mode) {
    g_st.matrixMode = mode;
    if (g_trace > 0) {
        --g_trace;
        log::Writef("fpview trace: glMatrixMode(0x%04X)", mode);
    }
    if (g_origMatrixMode) g_origMatrixMode(mode);
}

void __stdcall HookLoadIdentity() {
    if (g_trace > 0) {
        --g_trace;
        log::Writef("fpview trace: glLoadIdentity   (mode 0x%04X)", g_st.matrixMode);
    }
    if (g_anchored && g_st.matrixMode == GL_MODELVIEW && g_stage == kIdle)
        g_stage = kIdentityLoaded;
    if (g_origLoadIdentity) g_origLoadIdentity();
}

void __stdcall HookMultMatrixf(const float* m) {
    if (g_trace > 0 && m) {
        --g_trace;
        log::Writef("fpview trace: glMultMatrixf (mode 0x%04X) t=(%d %d %d) mm",
                    g_st.matrixMode, static_cast<int>(m[12] * 1000.0f),
                    static_cast<int>(m[13] * 1000.0f), static_cast<int>(m[14] * 1000.0f));
    }
    if (g_stage == kIdentityLoaded && g_st.matrixMode == GL_MODELVIEW) {
        g_stage = kRotationSeen;
        if (g_cfg.impose && g_st.active && g_st.eyeValid) {
            float rotation[16];
            BuildViewRotation(rotation);
            if (g_origMultMatrixf) g_origMultMatrixf(rotation);
            return;
        }
    }
    if (g_origMultMatrixf) g_origMultMatrixf(m);
}

void __stdcall HookPushMatrix() {
    if (g_trace > 0) {
        --g_trace;
        log::Writef("fpview trace: glPushMatrix    (mode 0x%04X)", g_st.matrixMode);
    }
    // The view is finished by the time anything is pushed; nothing to do here
    // beyond noting it, which keeps the stage machine honest if the engine ever
    // pushes before translating.
    if (g_stage == kRotationSeen && g_st.matrixMode == GL_MODELVIEW) g_stage = kComplete;
    if (g_origPushMatrix) g_origPushMatrix();
}

void __stdcall HookTranslatef(float x, float y, float z) {
    if (g_trace > 0) {
        --g_trace;
        log::Writef("fpview trace: glTranslatef (mode 0x%04X) (%d %d %d) mm",
                    g_st.matrixMode, static_cast<int>(x * 1000.0f),
                    static_cast<int>(y * 1000.0f), static_cast<int>(z * 1000.0f));
    }
    if (g_stage == kRotationSeen && g_st.matrixMode == GL_MODELVIEW) {
        g_stage = kComplete;
        if (g_cfg.impose && g_st.active && g_st.eyeValid) {
            if (g_st.replaced == 0)
                log::Writef("fpview: view taken over -- engine camera was (%d %d %d), "
                            "ours is (%d %d %d) mm",
                            static_cast<int>(-x * 1000.0f), static_cast<int>(-y * 1000.0f),
                            static_cast<int>(-z * 1000.0f),
                            static_cast<int>(g_st.eye[0] * 1000.0f),
                            static_cast<int>(g_st.eye[1] * 1000.0f),
                            static_cast<int>(g_st.eye[2] * 1000.0f));
            ++g_st.replaced;
            if (g_origTranslatef)
                g_origTranslatef(-g_st.eye[0], -g_st.eye[1], -g_st.eye[2]);
            return;
        }
    }
    if (g_origTranslatef) g_origTranslatef(x, y, z);
}

void __stdcall HookRotatef(float a, float x, float y, float z) {
    if (g_trace > 0) {
        --g_trace;
        log::Writef("fpview trace: glRotatef (mode 0x%04X) %d deg about (%d %d %d)",
                    g_st.matrixMode, static_cast<int>(a), static_cast<int>(x),
                    static_cast<int>(y), static_cast<int>(z));
    }
    if (g_origRotatef) g_origRotatef(a, x, y, z);
}

// --- import patching ----------------------------------------------------------
bool PatchSlot(const char* what, uint32_t va, void* replacement, void** previous) {
    auto* slot = reinterpret_cast<void**>(va);
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        log::Writef("fpview: VirtualProtect failed on 0x%08X (%lu)", va, GetLastError());
        return false;
    }
    *previous = *slot;
    *slot = replacement;
    DWORD restored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &restored);
    log::Writef("fpview: %s [0x%08X] 0x%08X -> hook", what, va,
                reinterpret_cast<uint32_t>(*previous));
    return true;
}

void RestoreSlot(uint32_t va, void* original) {
    if (!original) return;
    auto* slot = reinterpret_cast<void**>(va);
    DWORD oldProtect = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        *slot = original;
        DWORD restored = 0;
        VirtualProtect(slot, sizeof(void*), oldProtect, &restored);
    }
}

void ReadConfig() {
    g_cfg.enabled = config::GetBool("FirstPerson", "OwnView", true);
    g_cfg.mouseLook = config::GetBool("FirstPerson", "MouseLook", true);
    g_cfg.sensitivity = config::GetFloat("FirstPerson", "MouseSensitivity", 0.15f);
    g_cfg.invertY = config::GetBool("FirstPerson", "InvertY", false);
    g_cfg.pitchLimit = config::GetFloat("FirstPerson", "PitchLimit", 85.0f);
    g_cfg.impose = config::GetBool("FirstPerson", "ImposeView", false);
    g_cfg.traceLines = config::GetInt("FirstPerson", "TraceMatrices", 0);
    if (g_cfg.traceLines < 0) g_cfg.traceLines = 0;
    if (g_cfg.traceLines > 200) g_cfg.traceLines = 200;
    if (g_cfg.sensitivity < 0.01f) g_cfg.sensitivity = 0.01f;
    if (g_cfg.sensitivity > 2.0f) g_cfg.sensitivity = 2.0f;
    if (g_cfg.pitchLimit < 10.0f) g_cfg.pitchLimit = 10.0f;
    if (g_cfg.pitchLimit > 89.0f) g_cfg.pitchLimit = 89.0f;
    log::Writef("fpview: impose %d trace %d | own view %s, mouse look %d, sensitivity %d, "
                "invert %d, pitch limit %d deg", g_cfg.impose ? 1 : 0, g_cfg.traceLines,
                g_cfg.enabled ? "ON" : "off", g_cfg.mouseLook ? 1 : 0,
                static_cast<int>(g_cfg.sensitivity * 100.0f), g_cfg.invertY ? 1 : 0,
                static_cast<int>(g_cfg.pitchLimit));
}

}  // namespace

bool Install() {
    if (g_st.installed) return true;
    if (!config::Present()) return false;
    ReadConfig();
    if (!g_cfg.enabled) return false;

    void* previous = nullptr;
    bool ok = PatchSlot("glMatrixMode", kIatMatrixMode,
                        reinterpret_cast<void*>(&HookMatrixMode), &previous);
    g_origMatrixMode = reinterpret_cast<MatrixModeFn>(previous);
    ok &= PatchSlot("glLoadIdentity", kIatLoadIdentity,
                    reinterpret_cast<void*>(&HookLoadIdentity), &previous);
    g_origLoadIdentity = reinterpret_cast<LoadIdentityFn>(previous);
    ok &= PatchSlot("glMultMatrixf", kIatMultMatrixf,
                    reinterpret_cast<void*>(&HookMultMatrixf), &previous);
    g_origMultMatrixf = reinterpret_cast<MultMatrixfFn>(previous);
    // glPushMatrix is the one that matters now: it marks the moment the
    // engine's view is finished and the first object is about to be drawn.
    ok &= PatchSlot("glPushMatrix", kIatPushMatrix,
                    reinterpret_cast<void*>(&HookPushMatrix), &previous);
    g_origPushMatrix = reinterpret_cast<PushMatrixFn>(previous);
    // These two are hooked only so the trace can show how the view is built.
    ok &= PatchSlot("glTranslatef", kIatTranslatef,
                    reinterpret_cast<void*>(&HookTranslatef), &previous);
    g_origTranslatef = reinterpret_cast<TranslatefFn>(previous);
    ok &= PatchSlot("glRotatef", kIatRotatef,
                    reinterpret_cast<void*>(&HookRotatef), &previous);
    g_origRotatef = reinterpret_cast<RotatefFn>(previous);

    if (!ok || !g_origMatrixMode || !g_origLoadIdentity || !g_origMultMatrixf ||
        !g_origPushMatrix || !g_origTranslatef || !g_origRotatef) {
        log::Write("fpview: REFUSED -- the GL matrix imports did not look right");
        Remove();
        return false;
    }
    g_st.hooked = true;
    g_st.installed = true;
    log::Write("fpview: installed; the first-person view is built here, not by the "
               "chase camera");
    return true;
}

void Remove() {
    if (g_st.hooked) {
        RestoreSlot(kIatMatrixMode, reinterpret_cast<void*>(g_origMatrixMode));
        RestoreSlot(kIatLoadIdentity, reinterpret_cast<void*>(g_origLoadIdentity));
        RestoreSlot(kIatMultMatrixf, reinterpret_cast<void*>(g_origMultMatrixf));
        RestoreSlot(kIatPushMatrix, reinterpret_cast<void*>(g_origPushMatrix));
        RestoreSlot(kIatTranslatef, reinterpret_cast<void*>(g_origTranslatef));
        RestoreSlot(kIatRotatef, reinterpret_cast<void*>(g_origRotatef));
        g_st.hooked = false;
    }
    if (g_st.installed)
        log::Writef("fpview: removed after %u view matrices replaced (%u candidates seen)",
                    g_st.replaced, g_st.candidates);
    g_st.installed = false;
}

int Status() {
    int s = g_st.installed ? 1 : 0;
    if (g_st.hooked) s |= 2;
    if (g_st.replaced) s |= 4;
    return s;
}

void OnGameplayFrame(bool active, const float worldEye[3], float facingRadians) {
    if (!g_st.installed) return;

    if (!active) {
        g_anchored = false;
        // Leaving first person: forget the eye point so no stale matrix can be
        // substituted, and let the yaw re-sync to the character next time.
        g_st.active = false;
        g_st.eyeValid = false;
        g_st.yawInitialised = false;
        return;
    }

    if (!g_st.yawInitialised) {
        // Start looking where the character faces, not wherever the mouse last
        // left the camera.
        g_st.yaw = facingRadians;
        g_st.pitch = 0.0f;
        g_st.yawInitialised = true;
    }

    g_st.active = true;
    ++g_frames;
    // If ApplyProjection never fires, or never leads to a modelview matrix, the
    // view is not ours and the log should say so once rather than stay silent.
    if (g_frames == 200 && g_st.replaced == 0)
        log::Writef("fpview: 200 first-person frames and not one view matrix replaced. "
                    "anchored=%d mode=0x%04X eyeValid=%d -- set [FirstPerson] "
                    "TraceMatrices=60 to see how the engine builds its modelview",
                    g_anchored ? 1 : 0, g_st.matrixMode, g_st.eyeValid ? 1 : 0);
    if (worldEye) {
        for (int i = 0; i < 3; ++i) g_st.eye[i] = worldEye[i];
        g_st.eyeValid = true;
    }
    UpdateMouse();
}

// Called from fov.cpp's Camera::ApplyProjection hook: once per rendered frame,
// scene camera only. Everything about identifying the view matrix hangs off
// this being the truthful frame boundary.
void OnCameraApply() {
    if (!g_st.installed) return;
    g_anchored = true;
    g_viewImposed = false;
    g_stage = kIdle;
    // The trace is a one-shot: enough lines to see how one frame is built,
    // then silent, because this fires every frame.
    if (g_cfg.traceLines > 0 && g_st.active && g_frames == 3) {
        g_trace = g_cfg.traceLines;
        log::Writef("fpview trace: dumping %d matrix calls from this frame", g_trace);
        g_cfg.traceLines = 0;
    }
}

float Yaw() { return g_st.yaw; }
float Pitch() { return g_st.pitch; }
bool Replacing() { return g_st.replaced > 0; }

}  // namespace fpview
}  // namespace k2se

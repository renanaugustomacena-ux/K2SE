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

using MatrixModeFn = void(__stdcall*)(uint32_t mode);
using LoadIdentityFn = void(__stdcall*)();
using MultMatrixfFn = void(__stdcall*)(const float* m);

MatrixModeFn g_origMatrixMode = nullptr;
LoadIdentityFn g_origLoadIdentity = nullptr;
MultMatrixfFn g_origMultMatrixf = nullptr;

struct Cfg {
    bool enabled = false;
    bool mouseLook = true;
    float sensitivity = 0.15f;
    bool invertY = false;
    float pitchLimit = 85.0f;   // degrees from level
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

// Recover the camera position a view matrix encodes: for an orthonormal
// rotation R and translation t, the camera sits at -(R^T * t). If that is near
// the player, this matrix is the scene's view and not a GUI or shadow one --
// a test rather than a guess about which call to intercept.
bool IsSceneView(const float* m) {
    if (!m) return false;
    const float tx = m[12];
    const float ty = m[13];
    const float tz = m[14];
    const float cx = -(m[0] * tx + m[1] * ty + m[2] * tz);
    const float cy = -(m[4] * tx + m[5] * ty + m[6] * tz);
    const float cz = -(m[8] * tx + m[9] * ty + m[10] * tz);
    if (!(cx == cx && cy == cy && cz == cz)) return false;   // NaN
    const float dx = cx - g_st.eye[0];
    const float dy = cy - g_st.eye[1];
    const float dz = cz - g_st.eye[2];
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    if (!g_st.reported && distanceSquared < 100.0f) {
        g_st.reported = true;
        log::Writef("fpview: scene view found -- engine camera at (%d %d %d) mm, "
                    "eye at (%d %d %d) mm", static_cast<int>(cx * 1000.0f),
                    static_cast<int>(cy * 1000.0f), static_cast<int>(cz * 1000.0f),
                    static_cast<int>(g_st.eye[0] * 1000.0f),
                    static_cast<int>(g_st.eye[1] * 1000.0f),
                    static_cast<int>(g_st.eye[2] * 1000.0f));
    }
    return distanceSquared < 100.0f;   // within 10 m of the eye point
}

void __stdcall HookMatrixMode(uint32_t mode) {
    g_st.matrixMode = mode;
    if (mode != GL_MODELVIEW) g_st.armed = false;
    if (g_origMatrixMode) g_origMatrixMode(mode);
}

void __stdcall HookLoadIdentity() {
    // A view matrix is always built onto a fresh identity in MODELVIEW.
    if (g_st.matrixMode == GL_MODELVIEW) g_st.armed = true;
    if (g_origLoadIdentity) g_origLoadIdentity();
}

void __stdcall HookMultMatrixf(const float* m) {
    if (g_st.active && g_st.eyeValid && g_st.armed && g_st.matrixMode == GL_MODELVIEW) {
        ++g_st.candidates;
        if (IsSceneView(m)) {
            g_st.armed = false;          // the view is set once per pass
            BuildView(g_st.view);
            ++g_st.replaced;
            if (g_st.replaced == 1)
                log::Write("fpview: the scene view matrix is now ours -- first person is "
                           "no longer the chase camera");
            if (g_origMultMatrixf) g_origMultMatrixf(g_st.view);
            return;
        }
    }
    if (g_origMultMatrixf) g_origMultMatrixf(m);
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
    if (g_cfg.sensitivity < 0.01f) g_cfg.sensitivity = 0.01f;
    if (g_cfg.sensitivity > 2.0f) g_cfg.sensitivity = 2.0f;
    if (g_cfg.pitchLimit < 10.0f) g_cfg.pitchLimit = 10.0f;
    if (g_cfg.pitchLimit > 89.0f) g_cfg.pitchLimit = 89.0f;
    log::Writef("fpview: own view %s, mouse look %d, sensitivity %d, invert %d, "
                "pitch limit %d deg", g_cfg.enabled ? "ON" : "off", g_cfg.mouseLook ? 1 : 0,
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

    if (!ok || !g_origMatrixMode || !g_origLoadIdentity || !g_origMultMatrixf) {
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
    if (worldEye) {
        for (int i = 0; i < 3; ++i) g_st.eye[i] = worldEye[i];
        g_st.eyeValid = true;
    }
    UpdateMouse();
}

float Yaw() { return g_st.yaw; }
float Pitch() { return g_st.pitch; }
bool Replacing() { return g_st.replaced > 0; }

}  // namespace fpview
}  // namespace k2se

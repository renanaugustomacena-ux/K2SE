#include "fov.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include "camera.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "movement.h"

namespace k2se {
namespace fov {
namespace {

// --- engine anchors (see fov.h and data/k2se_addresses.csv) -------------------
constexpr uint32_t kCameraVtable = 0x0098C45C;
// Slot 2: Camera::ApplyProjection(), thiscall, no arguments (the function ends with a
// plain `ret` at 0x00480429). It is the only gluPerspective caller and builds the
// culling frustum from +0x204, so a value written just before it runs is used by
// both. Session S1 (19:49) showed slot 3 Update(dt) is NEVER called for the scene
// camera (0 calls in a whole session), so the hook moved here.
constexpr uint32_t kCameraApplySlot = kCameraVtable + 2 * 4;    // 0x0098C464
constexpr uint32_t kCameraApply = 0x0047F320;
constexpr uint32_t kOffFov = 0x204;                             // float, vertical degrees
constexpr uint32_t kOffNear = 0x210;                            // float, near clip plane (0.1)
constexpr float kDefaultFov = 45.0f;                            // [0x0098C274]; camerastyle sets 55
constexpr uint32_t kAurPostString = 0x00474C00;

using ApplyFn = int(__fastcall*)(void* self, void* edx);
using AurPostStringFn = void(__cdecl*)(const char*, int, int, float);

struct Cfg {
    bool enabled = false;
    float exploration = 60.0f;
    float combat = 55.0f;
    float sprintAdd = 5.0f;
    float smoothSeconds = 0.35f;
    int keyIncrease = 0;
    int keyDecrease = 0;
    int keyReset = 0;
    float step = 2.5f;
    float minFov = 30.0f;
    float maxFov = 110.0f;
    bool banner = true;
    bool log = true;
};
Cfg g_cfg;

// One record per camera instance we have written to (the scene camera in
// practice; GUI cameras share the vtable and pass through untouched).
struct Cam {
    void* self = nullptr;
    float vanilla = kDefaultFov;   // last value seen that K2SE did not write
    float lastWritten = -1.0f;     // what K2SE wrote into +0x204, or <0 when nothing
    float nearVanilla = 0.1f;      // the engine's near plane before an override
    bool nearWritten = false;
};
constexpr int kMaxCams = 4;
Cam g_cams[kMaxCams];
int g_camCount = 0;

ApplyFn g_origApply = nullptr;
bool g_hooked = false;
bool g_installed = false;
LARGE_INTEGER g_qpcFreq = {};
LARGE_INTEGER g_lastApply = {};

float g_current = kDefaultFov;    // smoothed value being applied
float g_userOffset = 0.0f;        // hotkeys, degrees
bool g_scriptActive = false;
float g_scriptTarget = 0.0f;
float g_scriptSeconds = 0.0f;
uint32_t g_lastMovementCount = 0;
DWORD g_lastGameplayTick = 0;
uint32_t g_updates = 0;
uint32_t g_gameplayUpdates = 0;
int g_adoptLogs = 0;
int g_bannerFrames = 0;
char g_bannerText[64] = "";

bool SafeReadF32(const void* at, float* out) {
    __try {
        *out = *reinterpret_cast<const volatile float*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SafeWriteF32(void* at, float v) {
    __try {
        *reinterpret_cast<volatile float*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Cam* FindCam(void* self) {
    for (int i = 0; i < g_camCount; ++i)
        if (g_cams[i].self == self) return &g_cams[i];
    if (g_camCount < kMaxCams) {
        Cam& c = g_cams[g_camCount++];
        c.self = self;
        c.vanilla = kDefaultFov;
        c.lastWritten = -1.0f;
        log::Writef("fov: camera #%d at 0x%08X", g_camCount, reinterpret_cast<uint32_t>(self));
        return &c;
    }
    return nullptr;
}

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void ShowBanner(const char* why) {
    if (!g_cfg.banner) return;
    _snprintf(g_bannerText, sizeof(g_bannerText), "FOV %.1f  (%s)", g_current, why);
    g_bannerText[sizeof(g_bannerText) - 1] = '\0';
    g_bannerFrames = 90;
}

// The base FOV: the active camera view's preset when camera views are on,
// otherwise the ini exploration/combat pair.
float BaseFov() {
    float t = 0.0f;
    if (camera::PresetFov(&t)) return t;
    return movement::PlayerInCombat() ? g_cfg.combat : g_cfg.exploration;
}

float TargetFov() {
    if (g_scriptActive) return Clamp(g_scriptTarget, g_cfg.minFov, g_cfg.maxFov);
    float t = BaseFov();
    if (movement::Status() & movement::kSprinting) t += g_cfg.sprintAdd;
    t += g_userOffset;
    return Clamp(t, g_cfg.minFov, g_cfg.maxFov);
}

// First person needs a larger near plane so the player's own head and torso,
// which sit around the camera, are clipped away instead of filling the screen.
void ApplyNearPlane(void* self, Cam* cam, bool gameplay) {
    const float over = gameplay ? camera::NearPlaneOverride() : 0.0f;
    float cur = 0.0f;
    if (!SafeReadF32(static_cast<char*>(self) + kOffNear, &cur)) return;
    if (over > 0.0f) {
        if (!cam->nearWritten) cam->nearVanilla = cur;
        if (fabsf(cur - over) > 0.001f) SafeWriteF32(static_cast<char*>(self) + kOffNear, over);
        cam->nearWritten = true;
    } else if (cam->nearWritten) {
        SafeWriteF32(static_cast<char*>(self) + kOffNear, cam->nearVanilla);
        cam->nearWritten = false;
    }
}

void Hotkeys() {
    bool changed = false;
    if (g_cfg.keyIncrease && input::Pressed(g_cfg.keyIncrease)) {
        g_userOffset += g_cfg.step;
        changed = true;
    }
    if (g_cfg.keyDecrease && input::Pressed(g_cfg.keyDecrease)) {
        g_userOffset -= g_cfg.step;
        changed = true;
    }
    if (g_cfg.keyReset && input::Pressed(g_cfg.keyReset)) {
        g_userOffset = 0.0f;
        g_scriptActive = false;
        changed = true;
    }
    if (changed) {
        // Keep the offset inside what the clamp can honour, so repeated presses
        // do not build an invisible debt.
        const float base = BaseFov();
        g_userOffset = Clamp(base + g_userOffset, g_cfg.minFov, g_cfg.maxFov) - base;
        log::Writef("fov: hotkey -> offset %+.1f (target %.1f)", g_userOffset, TargetFov());
        ShowBanner("key");
    }
}

void OnCameraUpdate(void* self, float dt) {
    ++g_updates;
    Cam* cam = FindCam(self);
    if (!cam) return;
    float fov = 0.0f;
    if (!SafeReadF32(static_cast<char*>(self) + kOffFov, &fov)) return;
    if (dt < 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;

    // Gameplay = the player controller has ticked within the last 100 ms. Menus
    // pause the controller, dialogue and cutscenes disable it, minigames use their
    // own; all of those get the vanilla FOV back.
    const uint32_t mc = movement::UpdateCount();
    const DWORD now = GetTickCount();
    if (mc != g_lastMovementCount) {
        g_lastMovementCount = mc;
        g_lastGameplayTick = now;
    }
    const bool gameplay = g_lastGameplayTick != 0 && (now - g_lastGameplayTick) < 100;

    // Anything in the field we did not write is the game's own value: adopt it.
    const bool ours = cam->lastWritten >= 0.0f && fabsf(fov - cam->lastWritten) < 0.01f;
    if (!ours) {
        if (fabsf(fov - cam->vanilla) > 0.01f && g_adoptLogs < 24) {
            ++g_adoptLogs;
            log::Writef("fov: camera 0x%08X vanilla %.2f -> %.2f (%s)", reinterpret_cast<uint32_t>(self),
                        cam->vanilla, fov, gameplay ? "gameplay" : "not gameplay");
        }
        cam->vanilla = fov;
        cam->lastWritten = -1.0f;
    }

    ApplyNearPlane(self, cam, gameplay);
    if (!gameplay) {
        if (ours) {
            SafeWriteF32(static_cast<char*>(self) + kOffFov, cam->vanilla);
            cam->lastWritten = -1.0f;
        }
        // Smoothing restarts from the vanilla value when gameplay resumes.
        g_current = cam->vanilla;
        return;
    }

    ++g_gameplayUpdates;
    Hotkeys();
    const float target = TargetFov();
    const float smooth = g_scriptActive && g_scriptSeconds > 0.0f ? g_scriptSeconds : g_cfg.smoothSeconds;
    if (smooth > 0.01f) {
        float k = dt / smooth;
        if (k > 1.0f) k = 1.0f;
        g_current += (target - g_current) * k;
    } else {
        g_current = target;
    }
    if (fabsf(g_current - target) < 0.005f) g_current = target;
    if (SafeWriteF32(static_cast<char*>(self) + kOffFov, g_current)) cam->lastWritten = g_current;

    if (g_gameplayUpdates == 1)
        log::Writef("fov: first gameplay frame: vanilla %.2f -> target %.2f (exploration %.1f combat %.1f "
                    "sprint %+.1f)", cam->vanilla, target, g_cfg.exploration, g_cfg.combat, g_cfg.sprintAdd);
    if (g_bannerFrames > 0) {
        --g_bannerFrames;
        reinterpret_cast<AurPostStringFn>(kAurPostString)(g_bannerText, 5, 75, 0.6f);
    }
}

// Runs BEFORE the engine's ApplyProjection, so the value written into +0x204 is the
// one gluPerspective and the frustum see this frame. dt comes from QPC: the
// projection is applied once per rendered frame.
int __fastcall HookApply(void* self, void* edx) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = 1.0f / 60.0f;
    if (g_lastApply.QuadPart != 0 && g_qpcFreq.QuadPart != 0) {
        const double d = static_cast<double>(now.QuadPart - g_lastApply.QuadPart) /
                         static_cast<double>(g_qpcFreq.QuadPart);
        dt = d > 0.0 && d < 0.1 ? static_cast<float>(d) : 1.0f / 60.0f;
    }
    g_lastApply = now;
    OnCameraUpdate(self, dt);
    return g_origApply ? g_origApply(self, edx) : 0;
}

void ReadConfig() {
    g_cfg.enabled = config::GetBool("FOV", "Enabled", false);
    g_cfg.exploration = config::GetFloat("FOV", "Exploration", 60.0f);
    g_cfg.combat = config::GetFloat("FOV", "Combat", 55.0f);
    g_cfg.sprintAdd = config::GetFloat("FOV", "SprintAdd", 5.0f);
    g_cfg.smoothSeconds = config::GetFloat("FOV", "SmoothSeconds", 0.35f);
    g_cfg.keyIncrease = config::GetKey("FOV", "KeyIncrease", VK_ADD);
    g_cfg.keyDecrease = config::GetKey("FOV", "KeyDecrease", VK_SUBTRACT);
    g_cfg.keyReset = config::GetKey("FOV", "KeyReset", VK_MULTIPLY);
    g_cfg.step = config::GetFloat("FOV", "Step", 2.5f);
    g_cfg.minFov = config::GetFloat("FOV", "Min", 30.0f);
    g_cfg.maxFov = config::GetFloat("FOV", "Max", 110.0f);
    g_cfg.banner = config::GetBool("FOV", "Banner", true);
    if (g_cfg.minFov < 10.0f) g_cfg.minFov = 10.0f;
    if (g_cfg.maxFov > 150.0f) g_cfg.maxFov = 150.0f;
    if (g_cfg.maxFov < g_cfg.minFov + 1.0f) g_cfg.maxFov = g_cfg.minFov + 1.0f;
    g_cfg.exploration = Clamp(g_cfg.exploration, g_cfg.minFov, g_cfg.maxFov);
    g_cfg.combat = Clamp(g_cfg.combat, g_cfg.minFov, g_cfg.maxFov);
    log::Writef("fov: %s exploration %.1f combat %.1f sprint %+.1f smooth %.2fs keys %s/%s/%s step %.1f "
                "range %.0f..%.0f banner %d",
                g_cfg.enabled ? "ON" : "off", g_cfg.exploration, g_cfg.combat, g_cfg.sprintAdd,
                g_cfg.smoothSeconds, config::KeyName(g_cfg.keyIncrease), config::KeyName(g_cfg.keyDecrease),
                config::KeyName(g_cfg.keyReset), g_cfg.step, g_cfg.minFov, g_cfg.maxFov, g_cfg.banner ? 1 : 0);
}

bool InstallHook() {
    QueryPerformanceFrequency(&g_qpcFreq);
    auto* slot = reinterpret_cast<ApplyFn*>(kCameraApplySlot);
    ApplyFn current = nullptr;
    __try {
        current = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("fov: camera vtable unreadable -> refused");
        return false;
    }
    if (reinterpret_cast<uint32_t>(current) != kCameraApply) {
        log::Writef("fov: vtable slot 0x%08X holds 0x%08X, expected Camera::ApplyProjection 0x%08X -> refused",
                    kCameraApplySlot, reinterpret_cast<uint32_t>(current), kCameraApply);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        log::Writef("fov: VirtualProtect failed on 0x%08X (error %lu)", kCameraApplySlot, GetLastError());
        return false;
    }
    g_origApply = current;
    *slot = &HookApply;
    DWORD restored = 0;
    VirtualProtect(slot, sizeof(void*), old, &restored);
    g_hooked = true;
    log::Writef("fov: vtable hook [0x%08X] Camera::ApplyProjection 0x%08X -> 0x%08X", kCameraApplySlot,
                kCameraApply, reinterpret_cast<uint32_t>(&HookApply));
    return true;
}

}  // namespace

bool Install() {
    if (g_installed) return true;
    if (!config::Present()) return false;
    ReadConfig();
    if (!g_cfg.enabled) return false;
    if (!InstallHook()) return false;
    input::Track(g_cfg.keyIncrease);
    input::Track(g_cfg.keyDecrease);
    input::Track(g_cfg.keyReset);
    g_installed = true;
    return true;
}

void Remove() {
    if (!g_installed) return;
    // Put the game's values back before the hook goes.
    for (int i = 0; i < g_camCount; ++i) {
        if (g_cams[i].lastWritten >= 0.0f)
            SafeWriteF32(static_cast<char*>(g_cams[i].self) + kOffFov, g_cams[i].vanilla);
        if (g_cams[i].nearWritten)
            SafeWriteF32(static_cast<char*>(g_cams[i].self) + kOffNear, g_cams[i].nearVanilla);
    }
    if (g_hooked && g_origApply) {
        auto* slot = reinterpret_cast<ApplyFn*>(kCameraApplySlot);
        DWORD old = 0;
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            *slot = g_origApply;
            DWORD restored = 0;
            VirtualProtect(slot, sizeof(void*), old, &restored);
        }
        g_hooked = false;
    }
    log::Writef("fov: removed after %u projection applies (%u in gameplay)", g_updates, g_gameplayUpdates);
    g_installed = false;
}

int Status() {
    int s = 0;
    if (g_installed) s |= kInstalled;
    for (int i = 0; i < g_camCount; ++i)
        if (g_cams[i].lastWritten >= 0.0f) s |= kActive;
    if (g_scriptActive) s |= kScriptOverride;
    return s;
}

bool SetOverride(float degrees, float seconds) {
    if (!g_installed) return false;
    if (degrees <= 0.0f) {
        g_scriptActive = false;
        log::Write("fov: script override cleared");
    } else {
        g_scriptActive = true;
        g_scriptTarget = degrees;
        g_scriptSeconds = seconds;
        log::Writef("fov: script override %.1f over %.2fs", degrees, seconds);
    }
    ShowBanner("script");
    return true;
}

float Current() { return g_current; }

}  // namespace fov
}  // namespace k2se

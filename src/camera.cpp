#include "camera.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include "callsite.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "player.h"
#include "spawner.h"

namespace k2se {
namespace camera {
namespace {

// --- engine anchors (see camera.h and data/k2se_addresses.csv) ----------------
constexpr uint32_t kLoadCameraStyle = 0x007E1CA0;    // thiscall (int row), ret 4
constexpr uint32_t kLoadSites[] = {0x007E1C0B, 0x007DDC52, 0x007DE7D6};
constexpr uint32_t kOffDistance = 0x16C;
constexpr uint32_t kOffSpeed = 0x170;
constexpr uint32_t kOffPitch = 0x178;
constexpr uint32_t kOffHeight = 0x17C;
constexpr uint32_t kOffDistanceCopy = 0x88;
constexpr uint32_t kOffViewAngle = 0x8C;
constexpr uint32_t kOffStyleRow = 0x5C;
constexpr uint32_t kAurPostString = 0x00474C00;

using LoadStyleFn = int(__fastcall*)(void* self, void* edx, int row);
using AurPostStringFn = void(__cdecl*)(const char*, int, int, float);

struct Preset {
    const char* name;
    float distance;
    float height;
    float pitch;
    float fov;
    float nearPlane;   // 0 = leave the engine's
};

struct Cfg {
    bool enabled = false;
    int keyCycle = 'N';
    bool includeVanilla = false;
    int startView = kViewNear;
    bool banner = true;
    Preset presets[4] = {
        {"game", 0, 0, 0, 0, 0},
        {"near", 2.2f, 0.6f, 83.0f, 60.0f, 0.0f},
        {"far", 5.5f, 1.4f, 80.0f, 52.0f, 0.0f},
        {"first person", 0.05f, 1.65f, 90.0f, 75.0f, 0.35f},
    };
};
Cfg g_cfg;

struct Vanilla {
    float distance = 0, speed = 0, pitch = 0, height = 0, viewAngle = 0;
    int row = -1;
};

void* g_cam = nullptr;          // the game camera object (this of LoadCameraStyle)
Vanilla g_vanilla;
bool g_vanillaKnown = false;
int g_view = kViewVanilla;
bool g_written = false;         // preset values currently sit in the object
bool g_installed = false;
uint32_t g_loads = 0;
uint32_t g_frames = 0;
int g_bannerFrames = 0;
char g_bannerText[64] = "";
LoadStyleFn g_origLoad = reinterpret_cast<LoadStyleFn>(kLoadCameraStyle);

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
bool SafeReadI32(const void* at, int* out) {
    __try {
        *out = *reinterpret_cast<const volatile int*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

char* Field(uint32_t off) { return static_cast<char*>(g_cam) + off; }

bool ReadVanilla() {
    if (!player::LooksLikePointer(g_cam)) return false;
    Vanilla v;
    if (!SafeReadF32(Field(kOffDistance), &v.distance) || !SafeReadF32(Field(kOffSpeed), &v.speed) ||
        !SafeReadF32(Field(kOffPitch), &v.pitch) || !SafeReadF32(Field(kOffHeight), &v.height) ||
        !SafeReadF32(Field(kOffViewAngle), &v.viewAngle) || !SafeReadI32(Field(kOffStyleRow), &v.row))
        return false;
    g_vanilla = v;
    g_vanillaKnown = true;
    return true;
}

void WriteStyle(float distance, float height, float pitch) {
    if (!player::LooksLikePointer(g_cam)) return;
    SafeWriteF32(Field(kOffDistance), distance);
    SafeWriteF32(Field(kOffDistanceCopy), distance);
    SafeWriteF32(Field(kOffHeight), height);
    SafeWriteF32(Field(kOffPitch), pitch);
}

void RestoreVanilla(const char* why) {
    if (!g_written || !g_vanillaKnown) return;
    WriteStyle(g_vanilla.distance, g_vanilla.height, g_vanilla.pitch);
    g_written = false;
    log::Writef("camera: game style restored (%s): distance %.2f height %.2f pitch %.1f", why,
                g_vanilla.distance, g_vanilla.height, g_vanilla.pitch);
}

void ShowBanner() {
    if (!g_cfg.banner) return;
    const Preset& p = g_cfg.presets[g_view];
    if (g_view == kViewVanilla)
        _snprintf(g_bannerText, sizeof(g_bannerText), "Camera: game style");
    else
        _snprintf(g_bannerText, sizeof(g_bannerText), "Camera: %s  (dist %.1f  fov %.0f)", p.name, p.distance, p.fov);
    g_bannerText[sizeof(g_bannerText) - 1] = '\0';
    g_bannerFrames = 90;
}

void ApplyView(const char* why) {
    if (g_view == kViewVanilla) {
        RestoreVanilla(why);
    } else if (g_vanillaKnown) {
        const Preset& p = g_cfg.presets[g_view];
        WriteStyle(p.distance, p.height, p.pitch);
        g_written = true;
    }
    log::Writef("camera: view %d (%s) %s at t+%u ms", g_view, g_cfg.presets[g_view].name, why,
                log::MillisSinceInit());
    ShowBanner();
}

// The redirected LoadCameraStyle: the game (re)loads a style -- area entry,
// script SetCameraStyle, camera construction. Let it, then learn the object and
// its fresh values, and put the active preset back on top.
int __fastcall HookLoadStyle(void* self, void* edx, int row) {
    const int r = g_origLoad(self, edx, row);
    ++g_loads;
    spawner::OnAreaSetup();   // a camera style load = an area was (re)set up
    if (g_cam != self) {
        g_cam = self;
        log::Writef("camera: game camera object 0x%08X (style row %d)", reinterpret_cast<uint32_t>(self), row);
    }
    g_written = false;   // the loader overwrote whatever we had written
    if (ReadVanilla())
        log::Writef("camera: style row %d loaded: distance %.2f speed %.1f pitch %.1f height %.2f viewangle %.1f",
                    g_vanilla.row, g_vanilla.distance, g_vanilla.speed, g_vanilla.pitch, g_vanilla.height,
                    g_vanilla.viewAngle);
    if (g_view != kViewVanilla) ApplyView("re-applied after style load");
    return r;
}

void ReadPreset(int idx, const char* section) {
    Preset& p = g_cfg.presets[idx];
    p.distance = config::GetFloat(section, "Distance", p.distance);
    p.height = config::GetFloat(section, "Height", p.height);
    p.pitch = config::GetFloat(section, "Pitch", p.pitch);
    p.fov = config::GetFloat(section, "FOV", p.fov);
    p.nearPlane = config::GetFloat(section, "NearPlane", p.nearPlane);
    if (p.distance < 0.0f) p.distance = 0.0f;
    if (p.distance > 30.0f) p.distance = 30.0f;
    if (p.height < -2.0f) p.height = -2.0f;
    if (p.height > 10.0f) p.height = 10.0f;
    if (p.pitch < 10.0f) p.pitch = 10.0f;
    if (p.pitch > 170.0f) p.pitch = 170.0f;
    if (p.fov < 20.0f) p.fov = 20.0f;
    if (p.fov > 140.0f) p.fov = 140.0f;
    if (p.nearPlane < 0.0f) p.nearPlane = 0.0f;
    if (p.nearPlane > 2.0f) p.nearPlane = 2.0f;
}

void ReadConfig() {
    g_cfg.enabled = config::GetBool("Camera", "Enabled", false);
    g_cfg.keyCycle = config::GetKey("Camera", "KeyCycle", 'N');
    g_cfg.includeVanilla = config::GetBool("Camera", "IncludeGameStyle", false);
    g_cfg.startView = config::GetInt("Camera", "StartView", kViewNear);
    g_cfg.banner = config::GetBool("Camera", "Banner", true);
    if (g_cfg.startView < 0 || g_cfg.startView > 3) g_cfg.startView = kViewNear;
    ReadPreset(kViewNear, "CameraNear");
    ReadPreset(kViewFar, "CameraFar");
    ReadPreset(kViewFirstPerson, "CameraFirstPerson");
    log::Writef("camera: %s key %s start %d gameStyleInCycle %d | near d%.2f h%.2f p%.0f fov%.0f | far d%.2f "
                "h%.2f p%.0f fov%.0f | fp d%.2f h%.2f p%.0f fov%.0f near%.2f",
                g_cfg.enabled ? "ON" : "off", config::KeyName(g_cfg.keyCycle), g_cfg.startView,
                g_cfg.includeVanilla ? 1 : 0, g_cfg.presets[1].distance, g_cfg.presets[1].height,
                g_cfg.presets[1].pitch, g_cfg.presets[1].fov, g_cfg.presets[2].distance, g_cfg.presets[2].height,
                g_cfg.presets[2].pitch, g_cfg.presets[2].fov, g_cfg.presets[3].distance, g_cfg.presets[3].height,
                g_cfg.presets[3].pitch, g_cfg.presets[3].fov, g_cfg.presets[3].nearPlane);
}

}  // namespace

bool Install() {
    if (g_installed) return true;
    if (!config::Present()) return false;
    ReadConfig();
    if (!g_cfg.enabled) return false;
    int redirected = 0;
    for (int i = 0; i < 3; ++i) {
        static char names[3][40];
        _snprintf(names[i], sizeof(names[i]), "LoadCameraStyle (site %d/3)", i + 1);
        names[i][sizeof(names[i]) - 1] = '\0';
        if (callsite::Redirect(names[i], kLoadSites[i], kLoadCameraStyle,
                               reinterpret_cast<const void*>(&HookLoadStyle)))
            ++redirected;
    }
    if (redirected == 0) {
        log::Write("camera: no LoadCameraStyle site redirected -> camera views off");
        return false;
    }
    input::Track(g_cfg.keyCycle);
    g_view = g_cfg.startView;
    g_installed = true;
    log::Writef("camera: installed (%d/3 sites), start view %d (%s)", redirected, g_view, g_cfg.presets[g_view].name);
    return true;
}

void Remove() {
    if (!g_installed) return;
    RestoreVanilla("remove");
    // The call sites are restored by callsite::RestoreAll (movement::Remove).
    log::Writef("camera: removed after %u style loads, %u gameplay frames", g_loads, g_frames);
    g_installed = false;
}

int Status() {
    int s = g_installed ? 1 : 0;
    s |= (g_view & 3) << 1;
    return s;
}

void OnGameplayFrame() {
    if (!g_installed) return;
    ++g_frames;
    if (input::Pressed(g_cfg.keyCycle)) {
        int next = g_view + 1;
        if (next > kViewFirstPerson) next = g_cfg.includeVanilla ? kViewVanilla : kViewNear;
        if (next == kViewVanilla && !g_cfg.includeVanilla) next = kViewNear;
        g_view = next;
        ApplyView("key");
    }
    // Keep the preset in place: the engine's own paths (style reload, mode
    // changes) may write the fields between our frames.
    if (g_view != kViewVanilla && g_vanillaKnown) {
        const Preset& p = g_cfg.presets[g_view];
        WriteStyle(p.distance, p.height, p.pitch);
        g_written = true;
    }
    if (g_bannerFrames > 0) {
        --g_bannerFrames;
        reinterpret_cast<AurPostStringFn>(kAurPostString)(g_bannerText, 5, 95, 0.6f);
    }
}

bool PresetFov(float* degrees) {
    if (!g_installed || g_view == kViewVanilla || !degrees) return false;
    *degrees = g_cfg.presets[g_view].fov;
    return true;
}

float NearPlaneOverride() {
    if (!g_installed || g_view == kViewVanilla) return 0.0f;
    return g_cfg.presets[g_view].nearPlane;
}

bool SetView(int view) {
    if (!g_installed || view < 0 || view > 3) return false;
    g_view = view;
    ApplyView("script");
    return true;
}

int GetView() { return g_installed ? g_view : -1; }

}  // namespace camera
}  // namespace k2se

#include "fpcam.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

#include "camera.h"
#include "config.h"
#include "eyetable_generated.h"
#include "log.h"
#include "player.h"

namespace k2se {
namespace fpcam {
namespace {

// --- engine anchors (see fpcam.h and data/k2se_addresses.csv) ----------------
constexpr uint32_t kOffAppearanceAnimBase = 0x3C;    // CSWCAppearance -> CSWCAnimBase
constexpr uint32_t kVtGetModel = 0x98;               // CSWCAnimBase::GetModel(uint8, int)
constexpr uint32_t kVtFindNodeByName = 0x10C;        // CAuroraModel::FindNodeByName(const char*)
constexpr uint8_t kBodyModelSlot = 0xFF;             // 0xFF selects the body, not equipment
constexpr uint32_t kSrvOffAppearanceRow = 0x1184;    // uint16, as npcvariety.cpp uses

using GetModelFn = void*(__thiscall*)(void* self, uint32_t slot, int flag);
using FindNodeFn = void*(__thiscall*)(void* self, const char* name);

// The eye point cannot be below the knee or above a Wookiee. Anything outside
// this says the live query resolved something that is not a head, and the
// rest-pose table is the better answer.
constexpr float kMinPlausibleEye = 0.8f;
constexpr float kMaxPlausibleEye = 2.6f;

struct Cfg {
    bool enabled = false;
    int anchor = kAnchorEyes;
    float bobScale = 0.25f;
    float bobDamping = 0.85f;
    float maxJump = 0.5f;
    float forwardOffset = 0.0f;
    float heightOverride = 0.0f;
    bool probe = false;
    // The engine derives the view direction from the chase camera's own
    // geometry. With the camera five centimetres behind the character and 1.67 m
    // up, that direction points almost straight down at the feet -- which is
    // what "first person" looked like. Two independent remedies, because which
    // one is needed depends on whether the engine computes the direction before
    // or after the position it is handed, and that is not established:
    //   - the preset now uses distance 2.0 / height 0.0, so the direction the
    //     engine computes is horizontal, and only the position is overridden;
    //   - LockDirection forces the direction to the character's facing outright.
    bool lockDirection = false;
};
Cfg g_cfg;

struct State {
    bool installed = false;
    bool haveRest = false;       // the appearance row was found in the table
    bool haveLive = false;       // a live node position was read this session
    int row = -1;                // appearance.2da row of the driven character
    float restForward = 0.0f;
    float restHeight = 0.0f;
    float hookHeight = 0.0f;
    float smooth[3] = {0, 0, 0}; // the slow mean the bob oscillates around
    bool smoothValid = false;
    float eye[3] = {0, 0, 0};    // the stabilised anchor, model space
    uint32_t frames = 0;
    uint32_t jumpsRejected = 0;
    uint32_t probeRuns = 0;
    int nodePosOffset = -1;      // byte offset of the position triple in a node
};
State g_st;

// --- guarded memory access ---------------------------------------------------
bool SafeReadPtr(const void* at, void** out) {
    __try {
        *out = *reinterpret_cast<void* const volatile*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SafeReadU16(const void* at, uint16_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint16_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SafeReadF32(const void* at, float* out) {
    __try {
        *out = *reinterpret_cast<const volatile float*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SafeCallGetModel(void* animBase, void** out) {
    __try {
        uint32_t* vt = *reinterpret_cast<uint32_t**>(animBase);
        auto fn = reinterpret_cast<GetModelFn>(vt[kVtGetModel / 4]);
        *out = fn(animBase, kBodyModelSlot, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SafeCallFindNode(void* model, const char* name, void** out) {
    __try {
        uint32_t* vt = *reinterpret_cast<uint32_t**>(model);
        auto fn = reinterpret_cast<FindNodeFn>(vt[kVtFindNodeByName / 4]);
        *out = fn(model, name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* At(const void* base, uint32_t off) {
    return static_cast<const char*>(base) + off;
}

bool Finite(float v) { return v == v && v > -1.0e6f && v < 1.0e6f; }

// --- the rest-pose table ------------------------------------------------------
void LoadRestPose(void* serverCreature) {
    uint16_t row = 0;
    if (!player::LooksLikePointer(serverCreature) ||
        !SafeReadU16(At(serverCreature, kSrvOffAppearanceRow), &row))
        return;
    if (g_st.row == static_cast<int>(row)) return;   // unchanged, nothing to say

    g_st.row = static_cast<int>(row);
    g_st.haveRest = false;
    g_st.smoothValid = false;   // a different body means a different anchor
    const eyetable::Entry* e = eyetable::Find(g_st.row);
    if (!e) {
        log::Writef("fpcam: appearance row %d is not in the measured table -- "
                    "falling back to the ini height", g_st.row);
        return;
    }
    g_st.restForward = e->eyeForward;
    g_st.restHeight = e->eyeHeight;
    g_st.hookHeight = e->hookHeight;
    g_st.haveRest = (e->eyeHeight > 0.0f);
    if (g_st.haveRest) {
        log::Writef("fpcam: appearance row %d -> eyes at forward %d mm, height %d mm "
                    "(camerahook %d mm)", g_st.row, static_cast<int>(e->eyeForward * 1000.0f),
                    static_cast<int>(e->eyeHeight * 1000.0f),
                    static_cast<int>(e->hookHeight * 1000.0f));
    } else {
        log::Writef("fpcam: appearance row %d has no eye nodes (masked face) -- "
                    "using camerahook at %d mm", g_st.row,
                    static_cast<int>(e->hookHeight * 1000.0f));
    }
}

// --- the live model chain -----------------------------------------------------
// appearance -> CSWCAnimBase -> GetModel(0xFF, 1) -> the Aurora model.
void* ModelOf(void* appearance) {
    if (!player::LooksLikePointer(appearance)) return nullptr;
    void* animBase = nullptr;
    if (!SafeReadPtr(At(appearance, kOffAppearanceAnimBase), &animBase) ||
        !player::LooksLikePointer(animBase))
        return nullptr;
    void* model = nullptr;
    if (!SafeCallGetModel(animBase, &model)) {
        log::Write("fpcam: CSWCAnimBase::GetModel faulted -- live anchor disabled");
        g_cfg.anchor = kAnchorStatic;
        return nullptr;
    }
    return player::LooksLikePointer(model) ? model : nullptr;
}

// The node object's layout is not established yet, so the position field is
// found by matching it against a value we already know from the models: the
// rest-pose height of the very node we asked for. That makes the discovery
// self-verifying -- a wrong offset cannot pass -- and once it matches, the
// offset is cached and the scan never runs again.
bool FindNodePositionOffset(void* node, float expectedZ) {
    for (uint32_t off = 0; off <= 0x200; off += 4) {
        float x = 0, y = 0, z = 0;
        if (!SafeReadF32(At(node, off), &x) || !SafeReadF32(At(node, off + 4), &y) ||
            !SafeReadF32(At(node, off + 8), &z))
            break;
        if (!Finite(x) || !Finite(y) || !Finite(z)) continue;
        if (fabsf(z - expectedZ) < 0.02f && fabsf(x) < 0.2f) {
            g_st.nodePosOffset = static_cast<int>(off);
            log::Writef("fpcam: node position found at +0x%02X "
                        "(%d %d %d mm, expected z %d mm)", off,
                        static_cast<int>(x * 1000.0f), static_cast<int>(y * 1000.0f),
                        static_cast<int>(z * 1000.0f), static_cast<int>(expectedZ * 1000.0f));
            return true;
        }
    }
    return false;
}

bool NodePosition(void* node, float out[3]) {
    if (g_st.nodePosOffset < 0) return false;
    const uint32_t off = static_cast<uint32_t>(g_st.nodePosOffset);
    return SafeReadF32(At(node, off), &out[0]) && SafeReadF32(At(node, off + 4), &out[1]) &&
           SafeReadF32(At(node, off + 8), &out[2]);
}

// One-shot exploration, only with Probe=1 in the ini. It calls vtable slots
// whose signatures are known but whose object layouts are not, so it stays off
// by default and stops after a handful of runs whatever it finds.
void Probe(void* appearance) {
    if (!g_cfg.probe || g_st.probeRuns >= 3) return;
    ++g_st.probeRuns;

    void* model = ModelOf(appearance);
    log::Writef("fpcam probe %u: appearance 0x%08X -> model 0x%08X", g_st.probeRuns,
                reinterpret_cast<uint32_t>(appearance), reinterpret_cast<uint32_t>(model));
    if (!model) return;

    static const char* const kNames[] = {"camerahook", "CAMERAHOOK", "CameraHook",
                                         "headhook", "eyeLA", "eyeRA", "FreeLookHook"};
    for (int i = 0; i < static_cast<int>(sizeof(kNames) / sizeof(kNames[0])); ++i) {
        void* node = nullptr;
        if (!SafeCallFindNode(model, kNames[i], &node)) {
            log::Writef("fpcam probe: FindNodeByName(\"%s\") faulted -- stopping", kNames[i]);
            g_cfg.probe = false;
            return;
        }
        log::Writef("fpcam probe: \"%s\" -> 0x%08X", kNames[i], reinterpret_cast<uint32_t>(node));
        // Only camerahook has a rest-pose value we can check a raw offset
        // against: it is a child of the model root, so its stored transform is
        // its model-space one, unchanged by animation.
        if (node && player::LooksLikePointer(node) && g_st.nodePosOffset < 0 &&
            g_st.hookHeight > 0.0f && i <= 2)
            FindNodePositionOffset(node, g_st.hookHeight);
    }
}

// --- putting the camera at the eyes -------------------------------------------
//
// The measured eye height alone changed nothing visible, and it was never going
// to. reone's ThirdPersonCamera spells out what the style fields mean:
//
//     cameraPos  = targetPosition + distance * dir
//     cameraPos.z += height
//
// so `height` really is absolute height above the character's feet, and 1.670
// instead of 1.650 moves the camera two centimetres. Adjusting a chase camera's
// parameters cannot produce a first-person view; the camera has to be placed at
// the eye point instead.
//
// Camera (vtable 0x0098C45C) has two methods taking three floats -- slot 29
// (+0x74) and slot 30 (+0x78), both `ret 12`. One is the position, the other a
// direction. Rather than reverse two SEH-wrapped forwarding chains to find out
// which, they are told apart at runtime by their magnitude: a direction is a
// unit vector, a world position in KOTOR is metres from the area origin and
// essentially never has length near 1. The discriminator is logged the first
// time it fires, so the guess is visible and checkable rather than assumed.
//
// Only the position is replaced. The direction is left exactly as the engine
// computed it, so looking around keeps working through the game's own camera
// controls -- this changes where the camera IS, not where it points.
constexpr uint32_t kCameraVtable = 0x0098C45C;
constexpr uint32_t kSlotVecA = kCameraVtable + 29 * 4;   // +0x74
constexpr uint32_t kSlotVecB = kCameraVtable + 30 * 4;   // +0x78

using CameraVec3Fn = void(__fastcall*)(void* self, void* edx, float x, float y, float z);

CameraVec3Fn g_origVecA = nullptr;
CameraVec3Fn g_origVecB = nullptr;
bool g_cameraHooked = false;

// The eye point in world space, recomputed each gameplay frame.
float g_worldEye[3] = {0, 0, 0};
float g_worldFacing[3] = {0, 1, 0};
bool g_worldEyeValid = false;
uint32_t g_overrides = 0;
int g_positionSlot = -1;   // 29 or 30, decided at runtime by magnitude
// GUI cameras share this vtable, and moving one would wreck a menu. The scene
// camera is identified by the only thing that distinguishes it from here: its
// position tracks the player. The first instance seen within a few metres of
// the character while first person is active is locked in, and no other
// instance is ever touched.
void* g_sceneCamera = nullptr;

bool LooksLikeDirection(float x, float y, float z) {
    const float lengthSquared = x * x + y * y + z * z;
    return lengthSquared > 0.64f && lengthSquared < 1.44f;   // |v| roughly 0.8..1.2
}

void NoteSlot(int slot, float x, float y, float z) {
    if (g_positionSlot != -1) return;
    if (LooksLikeDirection(x, y, z)) return;
    // The first non-unit vector to arrive is the position, and the other slot is
    // the direction by elimination.
    g_positionSlot = slot;
    log::Writef("fpcam: camera slot %d carries the position (%d %d %d mm); "
                "slot %d is the direction", slot, static_cast<int>(x * 1000.0f),
                static_cast<int>(y * 1000.0f), static_cast<int>(z * 1000.0f),
                slot == 29 ? 30 : 29);
}

bool NearPlayer(float x, float y, float z) {
    const float dx = x - g_worldEye[0];
    const float dy = y - g_worldEye[1];
    const float dz = z - g_worldEye[2];
    return (dx * dx + dy * dy + dz * dz) < 25.0f;   // within 5 m
}

bool WantOverride(void* self, float x, float y, float z) {
    if (!g_st.installed || !g_cfg.enabled || !g_worldEyeValid) return false;
    if (camera::GetView() != camera::kViewFirstPerson) return false;
    if (g_sceneCamera == nullptr) {
        if (!NearPlayer(x, y, z)) return false;   // not the gameplay camera
        g_sceneCamera = self;
        log::Writef("fpcam: scene camera locked to 0x%08X", reinterpret_cast<uint32_t>(self));
    }
    return self == g_sceneCamera;
}


// The slot that is not the position carries the direction. Replacing it with the
// character's facing gives a level first-person view; it costs the ability to
// look up and down, which is why it is opt-in.
bool OverrideDirection(int slot, void* self, float* x, float* y, float* z) {
    if (!g_cfg.lockDirection || g_positionSlot == -1 || slot == g_positionSlot) return false;
    if (!g_st.installed || !g_cfg.enabled || !g_worldEyeValid) return false;
    if (camera::GetView() != camera::kViewFirstPerson) return false;
    if (g_sceneCamera == nullptr || self != g_sceneCamera) return false;
    *x = g_worldFacing[0];
    *y = g_worldFacing[1];
    *z = g_worldFacing[2];
    return true;
}

void __fastcall HookVecA(void* self, void* edx, float x, float y, float z) {
    NoteSlot(29, x, y, z);
    if (g_positionSlot == 29 && WantOverride(self, x, y, z)) {
        if (!g_overrides++)
            log::Writef("fpcam: first-person camera moved from (%d %d %d) to the eye point "
                        "(%d %d %d) mm", static_cast<int>(x * 1000.0f),
                        static_cast<int>(y * 1000.0f), static_cast<int>(z * 1000.0f),
                        static_cast<int>(g_worldEye[0] * 1000.0f),
                        static_cast<int>(g_worldEye[1] * 1000.0f),
                        static_cast<int>(g_worldEye[2] * 1000.0f));
        x = g_worldEye[0];
        y = g_worldEye[1];
        z = g_worldEye[2];
    }
    OverrideDirection(29, self, &x, &y, &z);
    if (g_origVecA) g_origVecA(self, edx, x, y, z);
}

void __fastcall HookVecB(void* self, void* edx, float x, float y, float z) {
    NoteSlot(30, x, y, z);
    if (g_positionSlot == 30 && WantOverride(self, x, y, z)) {
        if (!g_overrides++)
            log::Writef("fpcam: first-person camera moved from (%d %d %d) to the eye point "
                        "(%d %d %d) mm", static_cast<int>(x * 1000.0f),
                        static_cast<int>(y * 1000.0f), static_cast<int>(z * 1000.0f),
                        static_cast<int>(g_worldEye[0] * 1000.0f),
                        static_cast<int>(g_worldEye[1] * 1000.0f),
                        static_cast<int>(g_worldEye[2] * 1000.0f));
        x = g_worldEye[0];
        y = g_worldEye[1];
        z = g_worldEye[2];
    }
    OverrideDirection(30, self, &x, &y, &z);
    if (g_origVecB) g_origVecB(self, edx, x, y, z);
}

bool SwapSlot(uint32_t slotVa, void* replacement, CameraVec3Fn* original) {
    auto* slot = reinterpret_cast<CameraVec3Fn*>(slotVa);
    CameraVec3Fn current = nullptr;
    __try {
        current = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Writef("fpcam: camera vtable slot 0x%08X unreadable -> refused", slotVa);
        return false;
    }
    if (!player::LooksLikePointer(reinterpret_cast<void*>(current))) {
        log::Writef("fpcam: camera vtable slot 0x%08X holds 0x%08X, not a function -> refused",
                    slotVa, reinterpret_cast<uint32_t>(current));
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        log::Writef("fpcam: VirtualProtect failed on 0x%08X (%lu)", slotVa, GetLastError());
        return false;
    }
    *original = current;
    *slot = reinterpret_cast<CameraVec3Fn>(replacement);
    DWORD restored = 0;
    VirtualProtect(slot, sizeof(void*), old, &restored);
    log::Writef("fpcam: camera vtable [0x%08X] 0x%08X -> hook", slotVa,
                reinterpret_cast<uint32_t>(current));
    return true;
}

void RestoreSlot(uint32_t slotVa, CameraVec3Fn original) {
    if (!original) return;
    auto* slot = reinterpret_cast<CameraVec3Fn*>(slotVa);
    DWORD old = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = original;
        DWORD restored = 0;
        VirtualProtect(slot, sizeof(void*), old, &restored);
    }
}

// Model space -> world space. The measured eye point is (0, forward, height)
// relative to the model origin, and the character's facing rotates the forward
// part into the world.
void UpdateWorldEye(void* serverCreature, float forward, float height) {
    float pos[3];
    float ori[3];
    g_worldEyeValid = false;
    if (!player::ServerPosition(serverCreature, pos) ||
        !player::ServerOrientation(serverCreature, ori))
        return;
    // Orientation is a facing vector in the XY plane; normalise it so a stale or
    // zero-length one cannot throw the camera across the map.
    const float length = sqrtf(ori[0] * ori[0] + ori[1] * ori[1]);
    float fx = 0.0f;
    float fy = 1.0f;
    if (length > 0.001f) {
        fx = ori[0] / length;
        fy = ori[1] / length;
    }
    g_worldEye[0] = pos[0] + fx * forward;
    g_worldEye[1] = pos[1] + fy * forward;
    g_worldEye[2] = pos[2] + height;
    g_worldFacing[0] = fx;
    g_worldFacing[1] = fy;
    g_worldFacing[2] = 0.0f;   // level: first person should not stare at the floor
    g_worldEyeValid = true;
}

// --- stabilisation ------------------------------------------------------------
// Split the raw anchor into a slow mean and the oscillation around it, then
// scale only the oscillation. BobScale 0 = a perfectly still head, 1 = the raw
// bone. Anything that moves further than MaxJump in one frame is a teleport,
// a model swap or a bad read, and is refused rather than smoothed.
void Stabilise(const float raw[3], float out[3]) {
    if (!g_st.smoothValid) {
        for (int i = 0; i < 3; ++i) g_st.smooth[i] = raw[i];
        g_st.smoothValid = true;
    }
    float jump = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = raw[i] - g_st.smooth[i];
        jump += d * d;
    }
    if (jump > g_cfg.maxJump * g_cfg.maxJump) {
        ++g_st.jumpsRejected;
        if (g_st.jumpsRejected == 1)
            log::Writef("fpcam: anchor jumped %d mm in one frame -- refused "
                        "(further jumps counted, not logged)",
                        static_cast<int>(sqrtf(jump) * 1000.0f));
        for (int i = 0; i < 3; ++i) out[i] = g_st.smooth[i];
        return;
    }
    const float a = g_cfg.bobDamping;
    for (int i = 0; i < 3; ++i) {
        g_st.smooth[i] = a * g_st.smooth[i] + (1.0f - a) * raw[i];
        out[i] = g_st.smooth[i] + g_cfg.bobScale * (raw[i] - g_st.smooth[i]);
    }
}

void ReadConfig() {
    g_cfg.enabled = config::GetBool("FirstPerson", "Enabled", false);
    g_cfg.anchor = config::GetInt("FirstPerson", "Anchor", kAnchorEyes);
    g_cfg.bobScale = config::GetFloat("FirstPerson", "BobScale", 0.25f);
    g_cfg.bobDamping = config::GetFloat("FirstPerson", "BobDamping", 0.85f);
    g_cfg.maxJump = config::GetFloat("FirstPerson", "MaxJump", 0.5f);
    g_cfg.forwardOffset = config::GetFloat("FirstPerson", "ForwardOffset", 0.0f);
    g_cfg.heightOverride = config::GetFloat("FirstPerson", "EyeHeightOverride", 0.0f);
    g_cfg.probe = config::GetBool("FirstPerson", "Probe", false);
    g_cfg.lockDirection = config::GetBool("FirstPerson", "LockDirection", false);

    if (g_cfg.anchor < kAnchorEyes || g_cfg.anchor > kAnchorStatic) g_cfg.anchor = kAnchorEyes;
    if (g_cfg.bobScale < 0.0f) g_cfg.bobScale = 0.0f;
    if (g_cfg.bobScale > 1.0f) g_cfg.bobScale = 1.0f;
    if (g_cfg.bobDamping < 0.0f) g_cfg.bobDamping = 0.0f;
    if (g_cfg.bobDamping > 0.99f) g_cfg.bobDamping = 0.99f;
    if (g_cfg.maxJump < 0.05f) g_cfg.maxJump = 0.05f;
    if (g_cfg.maxJump > 5.0f) g_cfg.maxJump = 5.0f;
    if (g_cfg.heightOverride < 0.0f || g_cfg.heightOverride > 3.0f) g_cfg.heightOverride = 0.0f;

    log::Writef("fpcam: %s anchor %d bob %d%% damping %d%% maxjump %d mm "
                "forward %+d mm override %d mm probe %d",
                g_cfg.enabled ? "ON" : "off", g_cfg.anchor,
                static_cast<int>(g_cfg.bobScale * 100.0f),
                static_cast<int>(g_cfg.bobDamping * 100.0f),
                static_cast<int>(g_cfg.maxJump * 1000.0f),
                static_cast<int>(g_cfg.forwardOffset * 1000.0f),
                static_cast<int>(g_cfg.heightOverride * 1000.0f), g_cfg.probe ? 1 : 0);
    log::Writef("fpcam: lock direction %d", g_cfg.lockDirection ? 1 : 0);
}

}  // namespace

bool Install() {
    if (g_st.installed) return true;
    if (!config::Present()) return false;
    ReadConfig();
    if (!g_cfg.enabled) return false;
    // Two slots, because which one carries the position is decided at runtime
    // from the magnitude of what arrives. Both must be hooked to find out.
    const bool a = SwapSlot(kSlotVecA, reinterpret_cast<void*>(&HookVecA), &g_origVecA);
    const bool b = SwapSlot(kSlotVecB, reinterpret_cast<void*>(&HookVecB), &g_origVecB);
    g_cameraHooked = a && b;
    if (!g_cameraHooked) {
        // Half a hook is worse than none: restore and fall back to the style
        // height, which is what previous builds did.
        if (a) RestoreSlot(kSlotVecA, g_origVecA);
        if (b) RestoreSlot(kSlotVecB, g_origVecB);
        g_origVecA = g_origVecB = nullptr;
        log::Write("fpcam: camera vtable not hooked -- first person stays a chase camera");
    }

    g_st.installed = true;
    log::Writef("fpcam: installed; %d appearance rows measured, camera hook %s",
                eyetable::kCount, g_cameraHooked ? "on" : "OFF");
    return true;
}

void Remove() {
    if (!g_st.installed) return;
    if (g_cameraHooked) {
        RestoreSlot(kSlotVecA, g_origVecA);
        RestoreSlot(kSlotVecB, g_origVecB);
        g_cameraHooked = false;
    }
    log::Writef("fpcam: removed after %u first-person frames, %u camera overrides, "
                "%u anchor jumps refused, live anchor %s (position slot %d, node offset %d)",
                g_st.frames, g_overrides, g_st.jumpsRejected, g_st.haveLive ? "yes" : "no",
                g_positionSlot, g_st.nodePosOffset);
    g_st.installed = false;
}

int Status() {
    int s = g_st.installed ? 1 : 0;
    if (g_st.haveRest) s |= 2;
    if (g_st.haveLive) s |= 4;
    return s;
}

void OnGameplayFrame(const player::Refs& refs, float dt) {
    if (!g_st.installed || !g_cfg.enabled) return;
    (void)dt;

    // The appearance row is cheap and changes when the player swaps body, so it
    // is read every frame; LoadRestPose only does work when it actually changed.
    LoadRestPose(refs.serverCreature);

    if (camera::GetView() != camera::kViewFirstPerson) {
        // Outside first person the anchor is not used; drop the smoothing state
        // so re-entering does not lerp in from a stale position, and clear the
        // world eye point so a stale one can never reach the camera hook.
        g_st.smoothValid = false;
        g_worldEyeValid = false;
        return;
    }
    ++g_st.frames;

    Probe(refs.appearance);

    // Rest pose is the baseline. The live path refines it when it is available
    // and plausible; nothing here can make the camera worse than the measurement.
    float raw[3] = {0.0f, g_st.restForward + g_cfg.forwardOffset, g_st.restHeight};
    if (!g_st.haveRest && g_st.hookHeight > 0.0f) raw[2] = g_st.hookHeight;
    if (g_cfg.heightOverride > 0.0f) raw[2] = g_cfg.heightOverride;

    if (g_cfg.anchor != kAnchorStatic && g_st.nodePosOffset >= 0) {
        void* model = ModelOf(refs.appearance);
        if (model) {
            const char* name = (g_cfg.anchor == kAnchorCameraHook || !g_st.haveRest)
                                   ? "camerahook"
                                   : (g_cfg.anchor == kAnchorHeadHook ? "headhook" : "eyeLA");
            void* node = nullptr;
            float live[3] = {0, 0, 0};
            if (SafeCallFindNode(model, name, &node) && player::LooksLikePointer(node) &&
                NodePosition(node, live) && Finite(live[0]) && Finite(live[1]) &&
                Finite(live[2]) && live[2] > kMinPlausibleEye && live[2] < kMaxPlausibleEye) {
                for (int i = 0; i < 3; ++i) raw[i] = live[i];
                raw[1] += g_cfg.forwardOffset;
                if (!g_st.haveLive) {
                    g_st.haveLive = true;
                    log::Writef("fpcam: live anchor \"%s\" active at %d %d %d mm", name,
                                static_cast<int>(live[0] * 1000.0f),
                                static_cast<int>(live[1] * 1000.0f),
                                static_cast<int>(live[2] * 1000.0f));
                }
            }
        }
    }

    Stabilise(raw, g_st.eye);

    // The hook runs on the render side and cannot go looking for the player, so
    // the world-space eye point is handed to it here, once a frame.
    UpdateWorldEye(refs.serverCreature, g_st.eye[1], g_st.eye[2]);
}

bool EyeOffset(float* forward, float* height) {
    if (!g_st.installed || !g_cfg.enabled) return false;
    if (g_cfg.heightOverride > 0.0f) {
        if (forward) *forward = g_st.restForward + g_cfg.forwardOffset;
        if (height) *height = g_cfg.heightOverride;
        return true;
    }
    if (g_st.smoothValid) {
        if (forward) *forward = g_st.eye[1];
        if (height) *height = g_st.eye[2];
        return true;
    }
    if (g_st.haveRest) {
        if (forward) *forward = g_st.restForward + g_cfg.forwardOffset;
        if (height) *height = g_st.restHeight;
        return true;
    }
    if (g_st.hookHeight > 0.0f) {
        if (forward) *forward = g_cfg.forwardOffset;
        if (height) *height = g_st.hookHeight;
        return true;
    }
    return false;
}

bool CameraHookActive() {
    return g_st.installed && g_cfg.enabled && g_cameraHooked;
}

bool GetEyePosition(float out[3]) {
    if (!g_st.installed || !g_st.smoothValid || !out) return false;
    for (int i = 0; i < 3; ++i) out[i] = g_st.eye[i];
    return true;
}

void SetBobScale(float scale) {
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 1.0f) scale = 1.0f;
    if (scale == g_cfg.bobScale) return;
    g_cfg.bobScale = scale;
    log::Writef("fpcam: bob scale set to %d%%", static_cast<int>(scale * 100.0f));
}

}  // namespace fpcam
}  // namespace k2se

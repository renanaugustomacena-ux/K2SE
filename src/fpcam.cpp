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
    // Eye level exactly was "too high off the ground". The camera sits this far
    // below the measured eye point; the measurement stays the anchor, it is just
    // not treated as sacred.
    float heightDrop = 0.10f;
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

// --- where the first-person camera sits ---------------------------------------
//
// An earlier build hooked Camera vtable slots 29 and 30 (the two methods taking
// three floats) hoping one carried the position. The log settled it: slot 29 was
// called four times, slot 30 never, and both only ever received (0,0,0). They are
// initialisation, not the per-frame position, so that hook is gone.
//
// What works is the chase camera's own style height, which reone shows is
// absolute height above the character's feet. The camera therefore sits at the
// eye height this module measures, minus HeightDrop -- because eye level exactly
// felt too high off the ground, and a first-person camera reads better slightly
// below the eyes than dead level with them.

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
    g_cfg.heightDrop = config::GetFloat("FirstPerson", "HeightDrop", 0.10f);
    if (g_cfg.heightDrop < 0.0f) g_cfg.heightDrop = 0.0f;
    if (g_cfg.heightDrop > 0.6f) g_cfg.heightDrop = 0.6f;

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
    log::Writef("fpcam: height drop %d mm below the measured eyes",
                static_cast<int>(g_cfg.heightDrop * 1000.0f));
}

}  // namespace

bool Install() {
    if (g_st.installed) return true;
    if (!config::Present()) return false;
    ReadConfig();
    if (!g_cfg.enabled) return false;
    g_st.installed = true;
    log::Writef("fpcam: installed; %d appearance rows measured", eyetable::kCount);
    return true;
}

void Remove() {
    if (!g_st.installed) return;
    log::Writef("fpcam: removed after %u first-person frames, %u anchor jumps refused, "
                "live anchor %s (node offset %d)",
                g_st.frames, g_st.jumpsRejected, g_st.haveLive ? "yes" : "no",
                g_st.nodePosOffset);
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
        // so re-entering does not lerp in from a stale position.
        g_st.smoothValid = false;
        return;
    }
    ++g_st.frames;

    Probe(refs.appearance);

    // Rest pose is the baseline. The live path refines it when it is available
    // and plausible; nothing here can make the camera worse than the measurement.
    float raw[3] = {0.0f, g_st.restForward + g_cfg.forwardOffset, g_st.restHeight};
    if (!g_st.haveRest && g_st.hookHeight > 0.0f) raw[2] = g_st.hookHeight;
    if (g_cfg.heightOverride > 0.0f) raw[2] = g_cfg.heightOverride;

    // The live model is asked for camerahook, not for an eye bone. The probe
    // settled that: on the body model "eyeLA" and "eyeRA" both return null,
    // because the eye bones live in the HEAD model, which is a separate model
    // attached at headhook and not reachable through this one's FindNodeByName.
    // camerahook does resolve, and the measured distance from camerahook to the
    // eyes is known per appearance row -- so the live hook plus that delta gives
    // an eye point for the body the player is actually wearing (armour moves
    // both), without needing the head model at all.
    if (g_cfg.anchor != kAnchorStatic && g_st.nodePosOffset >= 0 && g_st.hookHeight > 0.0f) {
        void* model = ModelOf(refs.appearance);
        if (model) {
            const char* name = (g_cfg.anchor == kAnchorHeadHook) ? "headhook" : "camerahook";
            void* node = nullptr;
            float live[3] = {0, 0, 0};
            if (SafeCallFindNode(model, name, &node) && player::LooksLikePointer(node) &&
                NodePosition(node, live) && Finite(live[0]) && Finite(live[1]) &&
                Finite(live[2]) && live[2] > kMinPlausibleEye && live[2] < kMaxPlausibleEye) {
                const float deltaZ = g_st.haveRest ? (g_st.restHeight - g_st.hookHeight) : 0.0f;
                const float deltaY = g_st.haveRest ? (g_st.restForward - 0.0f) : 0.0f;
                raw[0] = live[0];
                raw[1] = live[1] + deltaY + g_cfg.forwardOffset;
                raw[2] = live[2] + deltaZ;
                if (!g_st.haveLive) {
                    g_st.haveLive = true;
                    log::Writef("fpcam: live anchor \"%s\" at %d mm + %d mm to the eyes "
                                "= %d mm", name, static_cast<int>(live[2] * 1000.0f),
                                static_cast<int>(deltaZ * 1000.0f),
                                static_cast<int>(raw[2] * 1000.0f));
                }
            }
        }
    }

    Stabilise(raw, g_st.eye);

}

// The camera height handed to the chase-camera style. The measured eye point is
// the anchor, dropped by HeightDrop: eye level exactly read as too high off the
// ground, and the exact millimetre matters far less than the camera being in the
// right place at all.
bool EyeOffset(float* forward, float* height) {
    if (!g_st.installed || !g_cfg.enabled) return false;
    if (g_cfg.heightOverride > 0.0f) {
        if (forward) *forward = g_st.restForward + g_cfg.forwardOffset;
        if (height) *height = g_cfg.heightOverride;
        return true;
    }
    if (g_st.smoothValid) {
        if (forward) *forward = g_st.eye[1];
        if (height) *height = g_st.eye[2] - g_cfg.heightDrop;
        return true;
    }
    if (g_st.haveRest) {
        if (forward) *forward = g_st.restForward + g_cfg.forwardOffset;
        if (height) *height = g_st.restHeight - g_cfg.heightDrop;
        return true;
    }
    if (g_st.hookHeight > 0.0f) {
        if (forward) *forward = g_cfg.forwardOffset;
        if (height) *height = g_st.hookHeight - g_cfg.heightDrop;
        return true;
    }
    return false;
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

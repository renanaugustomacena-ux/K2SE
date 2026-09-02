#include "movement.h"

#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "anim.h"
#include "callsite.h"
#include "camera.h"
#include "spawner.h"
#include "config.h"
#include "input.h"
#include "log.h"
#include "player.h"

#pragma intrinsic(_ReturnAddress)

namespace k2se {
namespace movement {
namespace {

// --- engine anchors (2026-09-02 disassembly; see data/k2se_addresses.csv) ----
constexpr uint32_t kGetMaxSpeed = 0x00867B40;
constexpr uint32_t kGetMaxSpeedSite1 = 0x0086603C;  // in Update: the first call each frame
constexpr uint32_t kGetMaxSpeedSite2 = 0x00867336;  // in Update
constexpr uint32_t kGetMaxSpeedSite3 = 0x00867AC7;  // in GetAcceleration
constexpr uint32_t kGetMaxSpeedSite1Return = kGetMaxSpeedSite1 + 5;

// CSWCCreature::SetDriveSpeed(float speed, float b), ret 8, called ONCE PER FRAME at the
// end of Update (0x00867A33) with the RK4 velocity magnitude. This is the frame
// boundary and the place where the speed factor takes effect immediately: the
// server mover reads exactly the value written here (client +0x3C8).
//
// Lesson from session S1 (2026-09-02): GetMaxSpeed is NOT per-frame. The RK4 block
// runs only while the velocity has not converged to the target, so after a second
// of steady running the controller stops calling it; a hook there only sees keys
// when the player changes direction.
constexpr uint32_t kSetDriveSpeed = 0x00776E10;
constexpr uint32_t kSetDriveSpeedSite = 0x00867A33;

// CSWPlayerControlCamRelative vtable (RTTI, 12 slots). Slot 10 is Update(float dt),
// virtual (the base CSWPlayerControl has a different implementation there), so a
// vtable-slot swap -- the same one-dword technique as the VM hook -- gives a
// per-frame entry that runs BEFORE the engine consumes the input axes:
//   [0] SetUpDown(float)   +0x10      [1] GetUpDown
//   [2] SetLeftRight(float) +0x14     [3] GetLeftRight
//   [4] SetPlayerWalking(int) +0x18   [5] GetWalking
//   [7] GetCurSpeed (|state+0x5C|)    [10] Update(dt)   [11] ResetDriveAcceleration
constexpr uint32_t kPlayerControlVtable = 0x009A4818;
constexpr uint32_t kPlayerControlUpdateSlot = kPlayerControlVtable + 10 * 4;   // 0x009A4840
constexpr uint32_t kPlayerControlUpdate = 0x00865830;
constexpr uint32_t kCtrlOffUpDown = 0x10;
constexpr uint32_t kCtrlOffLeftRight = 0x14;

constexpr uint32_t kSetPosition = 0x00543F50;       // CSWSObject::SetPosition(vec*, a, b, c) ret 0x10
// The `call SetPosition` sites that commit the PLAYER's server position, found by
// watching all 78 direct callers in session S1 (2026-09-02, fourth run):
//   0x00569881 (args 1,0,0) -- every frame, standing or running: the driven player's
//                             position commit (NOT in the 0x005Cxxxx mover we first
//                             assumed; the eleven sites there never fire for the PC);
//   0x00538C7B (args 1,1,0) -- once, at area entry / placement.
// Each site is verified (E8 + target) before redirection; pass-through for every
// other object.
constexpr uint32_t kMoverSetPositionSites[] = {0x00569881, 0x00538C7B};
constexpr int kMoverSetPositionSiteCount =
    static_cast<int>(sizeof(kMoverSetPositionSites) / sizeof(kMoverSetPositionSites[0]));

constexpr float kGravity = 9.81f;

using GetMaxSpeedFn = float(__fastcall*)(void* self, void* edx);
using UpdateFn = int(__fastcall*)(void* self, void* edx, float dt);
using SetDriveSpeedFn = void(__fastcall*)(void* self, void* edx, float speed, float b);
using SetPositionFn = int(__fastcall*)(void* self, void* edx, float* vec, int a, int b, int c);

// --- configuration ------------------------------------------------------------
struct SprintCfg {
    bool enabled = false;
    bool alwaysOn = false;   // S1 experiment: sprint without holding the key
    int key = 0;
    float factor = 1.6f;
    float rampSeconds = 0.25f;
    bool allowInCombat = false;
    float maxTotalFactor = 2.5f;
    bool cancelCrouch = true;
};
struct CrouchCfg {
    bool enabled = false;
    int key = 0;
    bool toggle = true;
    bool exitOnCombat = true;
};
struct JumpCfg {
    bool enabled = false;
    int key = 0;
    float height = 1.0f;
    float maxDistance = 2.5f;       // reserved for J2 (horizontal control)
    bool allowInCombat = false;
    float cooldown = 0.4f;
    bool placeholderAnim = false;   // diveroll as the jump animation: off (looks like a roll)
    bool clientLift = false;        // S1 experiment: also lift the client-side copies of the position
};
struct RollCfg {
    bool enabled = false;
    int key = 0;
    float cooldown = 1.0f;
    float boost = 1.3f;
    float boostSeconds = 0.6f;
    bool allowInCombat = false;
};
// Keyboard directional movement: WASD feed the controller's two input axes the
// way a gamepad stick does, so the existing camera-relative controller gives
// eight-way movement with the character turning toward the input heading.
// Requires the game's own CameraRotateLeft/Right to be moved off A/D
// (deploy_movement.py --remap-keys does it).
struct DirectionalCfg {
    bool enabled = false;
    int keyForward = 'W';
    int keyBack = 'S';
    int keyLeft = 'A';
    int keyRight = 'D';
    bool invertStrafe = false;
    bool invertForward = false;
};

SprintCfg g_sprint;
CrouchCfg g_crouch;
JumpCfg g_jump;
RollCfg g_roll;
DirectionalCfg g_dir;
bool g_banner = false;
bool g_installed = false;
bool g_updateHooked = false;
bool g_wroteAxes = false;
float g_engineDt = 0.0f;
uint32_t g_updateCount = 0;        // controller Update ticks: "gameplay is running" for other modules
UpdateFn g_origUpdate = nullptr;

// --- runtime state ------------------------------------------------------------
GetMaxSpeedFn g_origGetMaxSpeed = reinterpret_cast<GetMaxSpeedFn>(kGetMaxSpeed);
SetDriveSpeedFn g_origSetDriveSpeed = reinterpret_cast<SetDriveSpeedFn>(kSetDriveSpeed);
SetPositionFn g_origSetPosition = reinterpret_cast<SetPositionFn>(kSetPosition);
void* g_controller = nullptr;      // last CSWPlayerControlCamRelative seen by GetMaxSpeed
uint32_t g_maxSpeedCalls = 0;      // instrumentation: how rarely GetMaxSpeed really runs

LARGE_INTEGER g_qpcFreq = {};
LARGE_INTEGER g_lastFrame = {};
float g_dt = 0.0f;
uint32_t g_frame = 0;

player::Refs g_refs = {};
bool g_refsValid = false;

float g_sprintBlend = 0.0f;        // 0..1, ramped
bool g_sprintHeld = false;
bool g_sprinting = false;
float g_scriptSprintFactor = -1.0f;  // <0: use ini

bool g_crouching = false;

bool g_airborne = false;
float g_jumpT = 0.0f;
float g_jumpV0 = 0.0f;
float g_jumpP0[3] = {0, 0, 0};    // takeoff position (ground)
float g_jumpCooldown = 0.0f;
bool g_jumpRequested = false;
uint32_t g_jumpFrames = 0;
uint32_t g_jumpActiveFrames = 0;       // frames where K2SE committed the position itself
bool g_jumpCommittedThisFrame = false; // the mover committed the player's position (moving jump)
bool g_jumpCommittedLastFrame = false;

bool g_rolling = false;
float g_rollT = 0.0f;
float g_rollCooldown = 0.0f;
bool g_rollRequested = false;

// Transition logging helpers -------------------------------------------------
void Say(const char* what) { log::Writef("movement: %s at t+%u ms", what, log::MillisSinceInit()); }

float Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<float>(t.QuadPart) / static_cast<float>(g_qpcFreq.QuadPart);
}

void BeginFrame() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (g_lastFrame.QuadPart != 0) {
        double dt = static_cast<double>(t.QuadPart - g_lastFrame.QuadPart) /
                    static_cast<double>(g_qpcFreq.QuadPart);
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.1) dt = 0.1;  // a load screen or a stall: do not integrate the gap
        g_dt = static_cast<float>(dt);
    } else {
        g_dt = 0.0f;
    }
    // Prefer the engine's own frame time when the Update hook provided it.
    if (g_engineDt > 0.0f && g_engineDt <= 0.1f) g_dt = g_engineDt;
    g_lastFrame = t;
    ++g_frame;
    g_jumpCommittedLastFrame = g_jumpCommittedThisFrame;
    g_jumpCommittedThisFrame = false;
    input::BeginFrame();
}

// --- position scan (S1 diagnostic) ---------------------------------------------
// The server position lifted by SetPosition did not show on screen (third run, five
// jumps, 54 commits each). The model is placed from a client-side copy of the
// position that lives somewhere in CSWCCreature, its anim base or the model. This
// finds it empirically: at the first jump, scan those objects (and what their
// pointers point to) for three floats equal to the server position, log the hits,
// then follow them during the jump. With [Jump] ClientLift=1 the hits inside the
// client creature get the same height written into their Z each airborne frame.
bool SafeWriteF32(void* at, float v);  // defined below with the axis writer

struct PosHit {
    void* base;
    uint32_t off;
    float groundZ;
    char what[48];
};
PosHit g_posHits[16];
int g_posHitCount = 0;
int g_posScanLines = 0;
bool g_posScanned = false;

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ScanForPosition(const char* what, void* obj, size_t bytes, const float* pos, int depth) {
    if (!player::LooksLikePointer(obj) || !pos || g_posScanLines >= 60) return;
    unsigned char* buf = static_cast<unsigned char*>(HeapAlloc(GetProcessHeap(), 0, bytes));
    if (!buf) return;
    if (!SafeCopy(buf, obj, bytes)) {
        log::Writef("movement: posscan %s 0x%08X: unreadable", what, reinterpret_cast<uint32_t>(obj));
        ++g_posScanLines;
        HeapFree(GetProcessHeap(), 0, buf);
        return;
    }
    int hits = 0;
    for (size_t off = 0; off + 12 <= bytes; off += 4) {
        float v[3];
        memcpy(v, buf + off, 12);
        if (fabsf(v[0] - pos[0]) < 0.05f && fabsf(v[1] - pos[1]) < 0.05f && fabsf(v[2] - pos[2]) < 0.6f) {
            log::Writef("movement: posscan %s 0x%08X +0x%03X = (%.2f %.2f %.3f)", what,
                        reinterpret_cast<uint32_t>(obj), static_cast<unsigned>(off), v[0], v[1], v[2]);
            ++g_posScanLines;
            if (g_posHitCount < 16) {
                PosHit& h = g_posHits[g_posHitCount++];
                h.base = obj;
                h.off = static_cast<uint32_t>(off);
                h.groundZ = v[2];
                _snprintf(h.what, sizeof(h.what), "%s+0x%03X", what, static_cast<unsigned>(off));
                h.what[sizeof(h.what) - 1] = '\0';
            }
            if (++hits >= 6) break;
        }
    }
    if (depth > 0) {
        // Follow pointer-looking dwords (collected first: the recursion reuses no buffer,
        // but the list is cheap and keeps the loop bounded).
        void* ptrs[0xC0];
        int n = 0;
        const size_t ptrBytes = bytes < sizeof(ptrs) ? bytes : sizeof(ptrs);
        for (size_t off = 0; off + 4 <= ptrBytes && n < 0xC0; off += 4) {
            void* p;
            memcpy(&p, buf + off, 4);
            if (p != obj && player::LooksLikePointer(p)) ptrs[n++] = p;
        }
        for (int i = 0; i < n && g_posScanLines < 60; ++i) {
            char sub[40];
            _snprintf(sub, sizeof(sub), "%s->", what);
            sub[sizeof(sub) - 1] = '\0';
            ScanForPosition(sub, ptrs[i], 0x200, pos, depth - 1);
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

void RunPositionScan(const float* pos) {
    if (g_posScanned || !g_refsValid) return;
    g_posScanned = true;
    g_posHitCount = 0;
    g_posScanLines = 0;
    log::Writef("movement: posscan for (%.2f %.2f %.3f): server 0x%08X client 0x%08X controller 0x%08X",
                pos[0], pos[1], pos[2], reinterpret_cast<uint32_t>(g_refs.serverCreature),
                reinterpret_cast<uint32_t>(g_refs.clientCreature),
                reinterpret_cast<uint32_t>(g_refs.controller));
    ScanForPosition("server", g_refs.serverCreature, 0x1200, pos, 0);   // must find +0x094
    ScanForPosition("client", g_refs.clientCreature, 0xA00, pos, 1);
    ScanForPosition("controller", g_refs.controller, 0x200, pos, 0);
    void* animBase = anim::AnimBase(g_refs.clientCreature);
    if (animBase) ScanForPosition("animbase", animBase, 0x400, pos, 1);
    log::Writef("movement: posscan done, %d hit(s) recorded", g_posHitCount);
}

void FollowPositionHits(const char* when) {
    for (int i = 0; i < g_posHitCount; ++i) {
        float v[3] = {0, 0, 0};
        if (SafeCopy(v, static_cast<char*>(g_posHits[i].base) + g_posHits[i].off, 12))
            log::Writef("movement: posscan %s: %s = (%.2f %.2f %.3f)", when, g_posHits[i].what, v[0], v[1], v[2]);
    }
}

// Experiment: write the jump height into the Z of every client-creature hit.
void ClientLift(float height) {
    for (int i = 0; i < g_posHitCount; ++i) {
        if (g_posHits[i].base != g_refs.clientCreature) continue;
        SafeWriteF32(static_cast<char*>(g_posHits[i].base) + g_posHits[i].off + 8, g_posHits[i].groundZ + height);
    }
}

// Direct position commit, the same call and arguments the server mover uses
// ((vec, 1, 1, 0) -> notify client, dirty, no "silent" flag). SEH-guarded like
// every other engine call K2SE makes on the game's objects.
bool CommitPosition(float* vec) {
    if (!g_refsValid || !vec) return false;
    __try {
        g_origSetPosition(g_refs.serverCreature, nullptr, vec, 1, 1, 0);
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

bool SafeReadF32(const void* at, float* out) {
    __try {
        *out = *reinterpret_cast<const volatile float*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool g_wroteUd = false;
bool g_wroteLr = false;

// WASD -> the controller's input axes, before the engine reads them this frame.
// Cooperative, per axis: the game's own bindings (ActionUp/Down = W/S, and
// ActionLeft/Right once the player moves them onto A/D in Options) set these same
// axes from their key handlers before Update runs. An axis the engine already
// holds non-zero is left alone, so K2SE never fights the native strafe (or a
// gamepad); it only fills in an axis the game left at zero.
void ApplyKeyboardAxes(void* controller) {
    if (!g_dir.enabled || !player::LooksLikePointer(controller)) return;
    bool enabled = false;
    if (!player::ControllerEnabled(controller, &enabled) || !enabled) return;
    if (!input::Focused()) return;

    const int fwd = input::IsDown(g_dir.keyForward) ? 1 : 0;
    const int back = input::IsDown(g_dir.keyBack) ? 1 : 0;
    const int left = input::IsDown(g_dir.keyLeft) ? 1 : 0;
    const int right = input::IsDown(g_dir.keyRight) ? 1 : 0;
    float ud = static_cast<float>(fwd - back);
    float lr = static_cast<float>(right - left);
    if (g_dir.invertForward) ud = -ud;
    if (g_dir.invertStrafe) lr = -lr;

    char* base = static_cast<char*>(controller);
    float curUd = 0.0f, curLr = 0.0f;
    if (!SafeReadF32(base + kCtrlOffUpDown, &curUd) || !SafeReadF32(base + kCtrlOffLeftRight, &curLr))
        return;
    const bool engineUd = fabsf(curUd) > 0.01f && !g_wroteUd;
    const bool engineLr = fabsf(curLr) > 0.01f && !g_wroteLr;

    if ((fwd || back) && !engineUd) {
        SafeWriteF32(base + kCtrlOffUpDown, ud);
        g_wroteUd = true;
    } else if (g_wroteUd && !(fwd || back)) {
        SafeWriteF32(base + kCtrlOffUpDown, 0.0f);  // release: zero once, then hands off
        g_wroteUd = false;
    }
    if ((left || right) && !engineLr) {
        SafeWriteF32(base + kCtrlOffLeftRight, lr);
        g_wroteLr = true;
    } else if (g_wroteLr && !(left || right)) {
        SafeWriteF32(base + kCtrlOffLeftRight, 0.0f);
        g_wroteLr = false;
    }
    g_wroteAxes = g_wroteUd || g_wroteLr;
}

bool InCombat() {
    bool c = false;
    return g_refsValid && player::ServerInCombat(g_refs.serverCreature, &c) && c;
}
bool ServerStealth() {
    bool s = false;
    return g_refsValid && player::ServerStealthMode(g_refs.serverCreature, &s) && s;
}
bool Walking() {
    bool w = false;
    return g_refsValid && player::ControllerWalking(g_refs.controller, &w) && w;
}

// --- crouch -----------------------------------------------------------------
void ApplyCrouch(bool on) {
    if (!g_refsValid) return;
    // The server->client update handler (0x008079B0) rewrites the client flag
    // block whenever it syncs the creature, so the bit is re-asserted every
    // frame while crouching rather than set once.
    player::SetClientStealthBit(g_refs.clientCreature, on);
}

void SetCrouchState(bool on, const char* why) {
    if (g_crouching == on) return;
    g_crouching = on;
    ApplyCrouch(on);
    log::Writef("movement: crouch %s (%s) at t+%u ms", on ? "ON" : "OFF", why, log::MillisSinceInit());
}

// --- jump -------------------------------------------------------------------
void StartJump(const char* why) {
    if (!g_refsValid) return;
    if (!player::ServerPosition(g_refs.serverCreature, g_jumpP0)) return;
    g_jumpV0 = sqrtf(2.0f * kGravity * g_jump.height);
    g_jumpT = 0.0f;
    g_jumpFrames = 0;
    g_jumpActiveFrames = 0;
    g_jumpCommittedThisFrame = false;
    g_jumpCommittedLastFrame = false;
    g_airborne = true;
    if (g_crouching) SetCrouchState(false, "jump");
    // Session S1: diveroll as the jump animation reads as "a roll instead of a
    // jump", so it is off by default until an authored jump animation exists (J4).
    if (g_jump.placeholderAnim) anim::PlayRow(g_refs.clientCreature, anim::kRowDiveRoll, 1);
    log::Writef("movement: jump START (%s) p0=(%.2f %.2f %.3f) v0=%.2f h=%.2f at t+%u ms", why,
                g_jumpP0[0], g_jumpP0[1], g_jumpP0[2], g_jumpV0, g_jump.height, log::MillisSinceInit());
    RunPositionScan(g_jumpP0);
    // The client copies drift with the engine's own movement: refresh their ground Z
    // at every takeoff so the lift is relative to where the model actually is.
    for (int i = 0; i < g_posHitCount; ++i) {
        float v[3];
        if (SafeCopy(v, static_cast<char*>(g_posHits[i].base) + g_posHits[i].off, 12)) g_posHits[i].groundZ = v[2];
    }
}

void EndJump(const char* why) {
    g_airborne = false;
    g_jumpCooldown = g_jump.cooldown;
    // Land exactly on the tracked ground point, so a standing jump never ends a
    // few millimetres in the air when the last active frame still had height.
    if (g_jumpActiveFrames > 0) CommitPosition(g_jumpP0);
    if (g_jump.clientLift) ClientLift(0.0f);
    log::Writef("movement: jump END (%s) after %u mover + %u active frames, %.2f s", why, g_jumpFrames,
                g_jumpActiveFrames, g_jumpT);
}

float JumpHeightNow() {
    const float h = g_jumpV0 * g_jumpT - 0.5f * kGravity * g_jumpT * g_jumpT;
    return h > 0.0f ? h : 0.0f;
}

// --- roll -------------------------------------------------------------------
void StartRoll(const char* why) {
    if (!g_refsValid) return;
    if (!anim::PlayRow(g_refs.clientCreature, anim::kRowDiveRoll, 1)) return;
    g_rolling = true;
    g_rollT = 0.0f;
    g_rollCooldown = g_roll.cooldown;
    log::Writef("movement: roll (%s) boost x%.2f for %.2f s at t+%u ms", why, g_roll.boost,
                g_roll.boostSeconds, log::MillisSinceInit());
}

// --- per-frame logic, run once per controller frame -------------------------
// `subject` is the controller when the Update hook is in place (the normal case),
// otherwise the driven client creature handed to SetDriveSpeed.
void OnFrame(void* subject, bool subjectIsController) {
    // user32 is resolved here, on the game thread and long after process start,
    // never inside DllMain (loader lock).
    static bool inputReady = false;
    if (!inputReady) {
        inputReady = true;
        input::Init();
    }
    BeginFrame();
    if (subjectIsController) {
        g_controller = subject;
        g_refsValid = player::Resolve(subject, &g_refs);
    } else {
        g_refsValid = player::ResolveFromClient(subject, g_controller, &g_refs);
    }

    if (g_jumpCooldown > 0.0f) g_jumpCooldown -= g_dt;
    if (g_rollCooldown > 0.0f) g_rollCooldown -= g_dt;

    const bool combat = InCombat();
    const bool serverStealth = ServerStealth();
    const bool walking = Walking();

    // --- sprint (hold) ---
    bool wantSprint = false;
    if (g_sprint.enabled && g_refsValid) {
        wantSprint = g_sprint.alwaysOn || input::IsDown(g_sprint.key);
        if (wantSprint && !g_sprint.allowInCombat && combat) wantSprint = false;
        if (wantSprint && serverStealth) wantSprint = false;
        if (wantSprint && walking) wantSprint = false;
        if (wantSprint && g_crouching) {
            if (g_sprint.cancelCrouch)
                SetCrouchState(false, "sprint");
            else
                wantSprint = false;
        }
    }
    if (wantSprint != g_sprintHeld) {
        g_sprintHeld = wantSprint;
        if (!wantSprint && g_sprint.enabled && g_refsValid && input::IsDown(g_sprint.key)) {
            if (combat && !g_sprint.allowInCombat) Say("sprint OFF (combat)");
            else if (serverStealth) Say("sprint OFF (stealth mode)");
            else if (walking) Say("sprint OFF (walk modifier)");
        }
    }
    const float ramp = g_sprint.rampSeconds > 0.01f ? g_dt / g_sprint.rampSeconds : 1.0f;
    g_sprintBlend += g_sprintHeld ? ramp : -ramp;
    if (g_sprintBlend < 0.0f) g_sprintBlend = 0.0f;
    if (g_sprintBlend > 1.0f) g_sprintBlend = 1.0f;
    const bool sprintingNow = g_sprintBlend > 0.05f;
    if (sprintingNow != g_sprinting) {
        g_sprinting = sprintingNow;
        Say(g_sprinting ? "sprint ON" : "sprint OFF");
    }

    // --- crouch (toggle) ---
    if (g_crouch.enabled && g_refsValid) {
        if (input::Pressed(g_crouch.key)) {
            if (serverStealth) {
                Say("crouch ignored (stealth mode active)");
            } else if (g_airborne) {
                Say("crouch ignored (airborne)");
            } else {
                SetCrouchState(!g_crouching, "key");
            }
        }
        if (g_crouching && combat && g_crouch.exitOnCombat) SetCrouchState(false, "combat");
        if (g_crouching) ApplyCrouch(true);  // re-assert against the server sync
    }

    // --- roll ---
    if (g_roll.enabled && g_refsValid && (input::Pressed(g_roll.key) || g_rollRequested)) {
        g_rollRequested = false;
        if (g_rollCooldown > 0.0f) {
            // silent: cooldown
        } else if (combat && !g_roll.allowInCombat) {
            Say("roll ignored (combat)");
        } else if (g_airborne) {
            Say("roll ignored (airborne)");
        } else {
            StartRoll("key");
        }
    }
    if (g_rolling) {
        g_rollT += g_dt;
        if (g_rollT >= g_roll.boostSeconds) g_rolling = false;
    }

    // --- jump ---
    if (g_jump.enabled && g_refsValid && (input::Pressed(g_jump.key) || g_jumpRequested)) {
        g_jumpRequested = false;
        uint16_t moveFlags = 0;
        const bool immobile =
            player::ServerMoveFlags(g_refs.serverCreature, &moveFlags) && (moveFlags & 2u) == 0;
        if (g_airborne || g_jumpCooldown > 0.0f) {
            // silent: already airborne / cooldown
        } else if (combat && !g_jump.allowInCombat) {
            Say("jump ignored (combat)");
        } else if (serverStealth) {
            Say("jump ignored (stealth mode)");
        } else if (immobile) {
            Say("jump ignored (creature cannot move)");
        } else if (g_rolling) {
            Say("jump ignored (rolling)");
        } else {
            StartJump("key");
        }
    }
    if (g_airborne) {
        g_jumpT += g_dt;
        const float tEnd = 2.0f * g_jumpV0 / kGravity;
        if (g_jumpT >= tEnd) EndJump("landed");
        if (g_airborne && g_jumpT > 3.0f) EndJump("timeout");  // paranoia: never stay airborne
    }
    if (g_airborne && !g_jumpCommittedLastFrame) {
        // Standing jump. The server mover only runs while the player is driving,
        // so nothing commits the position for us: lift the tracked ground point
        // ourselves, once per frame, with the mover's own call and arguments.
        // While running, the mover commits every frame and the SetPosition hook
        // adds the height instead (the flag from the previous frame says so).
        float lifted[3] = {g_jumpP0[0], g_jumpP0[1], g_jumpP0[2] + JumpHeightNow()};
        if (CommitPosition(lifted)) {
            ++g_jumpActiveFrames;
            if (g_jumpActiveFrames <= 2)
                log::Writef("movement: jump active commit #%u z=%.3f (+%.3f)", g_jumpActiveFrames,
                            lifted[2], lifted[2] - g_jumpP0[2]);
        } else if (g_jumpActiveFrames == 0) {
            Say("jump: direct SetPosition faulted -> jump aborted");
            EndJump("fault");
        }
    }
    if (g_airborne) {
        const uint32_t f = g_jumpFrames + g_jumpActiveFrames;
        if (f == 12 || f == 30) FollowPositionHits(f == 12 ? "mid-air (frame 12)" : "mid-air (frame 30)");
        if (g_jump.clientLift) ClientLift(JumpHeightNow());
    }

    if (g_banner && g_refsValid && (g_frame % 30) == 0) {
        static char text[96];
        _snprintf(text, sizeof(text), "K2SE mv  spr %.2f  crouch %d  air %d  roll %d  dt %.1fms  gms %u",
                  1.0f + (g_sprint.factor - 1.0f) * g_sprintBlend, g_crouching ? 1 : 0,
                  g_airborne ? 1 : 0, g_rolling ? 1 : 0, g_dt * 1000.0f, g_maxSpeedCalls);
        text[sizeof(text) - 1] = '\0';
        using AurPostStringFn = void(__cdecl*)(const char*, int, int, float);
        reinterpret_cast<AurPostStringFn>(0x00474C00)(text, 5, 55, 0.6f);
    }
}

float SpeedFactor() {
    float f = 1.0f;
    const float sprintFactor = g_scriptSprintFactor > 0.0f ? g_scriptSprintFactor : g_sprint.factor;
    f *= 1.0f + (sprintFactor - 1.0f) * g_sprintBlend;
    if (g_rolling && g_roll.boost > 1.0f) f *= g_roll.boost;
    if (f > g_sprint.maxTotalFactor && g_sprint.maxTotalFactor > 1.0f) f = g_sprint.maxTotalFactor;
    return f;
}

// --- the hooks ----------------------------------------------------------------
// GetMaxSpeed: pass-through. It only runs while the RK4 velocity is converging,
// so it is neither a frame boundary nor the place to scale the speed. It is kept
// redirected to learn the controller pointer (for the walk-modifier flag) and to
// count how rarely it actually runs.
float __fastcall HookGetMaxSpeed(void* self, void* edx) {
    g_controller = self;
    ++g_maxSpeedCalls;
    (void)kGetMaxSpeedSite1Return;
    return g_origGetMaxSpeed(self, edx);
}

// Update: the per-frame entry (vtable slot 10). Frame logic and the WASD axes
// go in BEFORE the engine reads the input; the engine then integrates as usual.
int __fastcall HookUpdate(void* self, void* edx, float dt) {
    g_engineDt = dt;
    ++g_updateCount;
    OnFrame(self, true);
    ApplyKeyboardAxes(self);
    camera::OnGameplayFrame();   // camera views ride the same gameplay frame
    spawner::OnGameplayFrame(g_refs.serverCreature, g_refsValid, g_dt);
    return g_origUpdate ? g_origUpdate(self, edx, dt) : 0;
}

// SetDriveSpeed: once per frame at the end of Update, `self` = the driven client
// creature. The speed factor is applied here so the server mover reads it this
// very frame. If the Update hook could not be installed, this doubles as the
// frame boundary.
void __fastcall HookSetDriveSpeed(void* self, void* edx, float speed, float b) {
    if (!g_updateHooked) OnFrame(self, false);
    g_origSetDriveSpeed(self, edx, speed * SpeedFactor(), b);
}

bool InstallUpdateHook() {
    auto* slot = reinterpret_cast<UpdateFn*>(kPlayerControlUpdateSlot);
    UpdateFn current = nullptr;
    __try {
        current = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("movement: player controller vtable unreadable -> Update hook refused");
        return false;
    }
    if (reinterpret_cast<uint32_t>(current) != kPlayerControlUpdate) {
        log::Writef("movement: vtable slot 0x%08X holds 0x%08X, expected Update 0x%08X -> refused",
                    kPlayerControlUpdateSlot, reinterpret_cast<uint32_t>(current), kPlayerControlUpdate);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        log::Writef("movement: VirtualProtect failed on 0x%08X (error %lu)", kPlayerControlUpdateSlot,
                    GetLastError());
        return false;
    }
    g_origUpdate = current;
    *slot = &HookUpdate;
    DWORD restored = 0;
    VirtualProtect(slot, sizeof(void*), old, &restored);
    g_updateHooked = true;
    log::Writef("movement: vtable hook [0x%08X] Update 0x%08X -> 0x%08X", kPlayerControlUpdateSlot,
                kPlayerControlUpdate, reinterpret_cast<uint32_t>(&HookUpdate));
    return true;
}

void RemoveUpdateHook() {
    if (!g_updateHooked || !g_origUpdate) return;
    auto* slot = reinterpret_cast<UpdateFn*>(kPlayerControlUpdateSlot);
    DWORD old = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = g_origUpdate;
        DWORD restored = 0;
        VirtualProtect(slot, sizeof(void*), old, &restored);
        log::Write("movement: Update vtable slot restored");
    }
    g_updateHooked = false;
}

// Diagnostic: which call sites commit the PLAYER's server position at all. The
// first eight distinct ones are logged once each, with their arguments.
void NoteplayerSite(uint32_t site, int a, int b, int c, const float* vec) {
    static uint32_t seen[8];
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (seen[i] == site) return;
    if (count >= 8) return;
    seen[count++] = site;
    log::Writef("movement: player position committed from call site 0x%08X (args %d,%d,%d) "
                "pos=(%.2f %.2f %.3f) [distinct site %d]",
                site, a, b, c, vec[0], vec[1], vec[2], count);
}

int __fastcall HookSetPosition(void* self, void* edx, float* vec, int a, int b, int c) {
    const bool isPlayer = g_refsValid && self == g_refs.serverCreature && vec;
    if (isPlayer) NoteplayerSite(reinterpret_cast<uint32_t>(_ReturnAddress()) - 5, a, b, c, vec);
    if (g_airborne && isPlayer) {
        ++g_jumpFrames;
        g_jumpCommittedThisFrame = true;
        if (g_jumpFrames <= 2)
            log::Writef("movement: player SetPosition via call site 0x%08X (args %d,%d,%d) z=%.3f",
                        reinterpret_cast<uint32_t>(_ReturnAddress()) - 5, a, b, c, vec[2]);
        // The mover's vector is the new ground point (it ran GetGroundZ): track it,
        // so the active path and the landing continue from where the engine left us.
        g_jumpP0[0] = vec[0];
        g_jumpP0[1] = vec[1];
        g_jumpP0[2] = vec[2];
        float lifted[3] = {vec[0], vec[1], vec[2] + JumpHeightNow()};
        // J1: the engine keeps X/Y (running jump), K2SE owns Z. Landing is the
        // frame where the parabola returns to zero height, so no snapping is
        // needed: the engine's own ground Z comes back on its own.
        return g_origSetPosition(self, edx, lifted, a, b, c);
    }
    return g_origSetPosition(self, edx, vec, a, b, c);
}

void ReadConfig() {
    g_sprint.enabled = config::GetBool("Sprint", "Enabled", false);
    g_sprint.alwaysOn = config::GetBool("Sprint", "AlwaysOn", false);
    g_sprint.key = config::GetKey("Sprint", "Key", VK_LSHIFT);
    g_sprint.factor = config::GetFloat("Sprint", "Factor", 1.6f);
    g_sprint.rampSeconds = config::GetFloat("Sprint", "RampSeconds", 0.25f);
    g_sprint.allowInCombat = config::GetBool("Sprint", "AllowInCombat", false);
    g_sprint.maxTotalFactor = config::GetFloat("Sprint", "MaxTotalFactor", 2.5f);
    g_sprint.cancelCrouch = config::GetBool("Sprint", "CancelCrouch", true);
    if (g_sprint.factor < 0.5f) g_sprint.factor = 0.5f;
    if (g_sprint.factor > 4.0f) g_sprint.factor = 4.0f;

    g_crouch.enabled = config::GetBool("Crouch", "Enabled", false);
    g_crouch.key = config::GetKey("Crouch", "Key", 'C');
    g_crouch.toggle = config::GetBool("Crouch", "Toggle", true);
    g_crouch.exitOnCombat = config::GetBool("Crouch", "ExitOnCombat", true);

    g_jump.enabled = config::GetBool("Jump", "Enabled", false);
    g_jump.key = config::GetKey("Jump", "Key", VK_SPACE);
    g_jump.height = config::GetFloat("Jump", "Height", 1.0f);
    g_jump.maxDistance = config::GetFloat("Jump", "MaxDistance", 2.5f);
    g_jump.allowInCombat = config::GetBool("Jump", "AllowInCombat", false);
    g_jump.cooldown = config::GetFloat("Jump", "Cooldown", 0.4f);
    g_jump.placeholderAnim = config::GetBool("Jump", "PlaceholderAnim", false);
    g_jump.clientLift = config::GetBool("Jump", "ClientLift", false);
    if (g_jump.height < 0.1f) g_jump.height = 0.1f;
    if (g_jump.height > 3.0f) g_jump.height = 3.0f;

    g_roll.enabled = config::GetBool("Roll", "Enabled", false);
    g_roll.key = config::GetKey("Roll", "Key", VK_LMENU);
    g_roll.cooldown = config::GetFloat("Roll", "Cooldown", 1.0f);
    g_roll.boost = config::GetFloat("Roll", "Boost", 1.3f);
    g_roll.boostSeconds = config::GetFloat("Roll", "BoostSeconds", 0.6f);
    g_roll.allowInCombat = config::GetBool("Roll", "AllowInCombat", false);

    g_dir.enabled = config::GetBool("Directional", "Enabled", false);
    g_dir.keyForward = config::GetKey("Directional", "KeyForward", 'W');
    g_dir.keyBack = config::GetKey("Directional", "KeyBack", 'S');
    g_dir.keyLeft = config::GetKey("Directional", "KeyLeft", 'A');
    g_dir.keyRight = config::GetKey("Directional", "KeyRight", 'D');
    g_dir.invertStrafe = config::GetBool("Directional", "InvertStrafe", false);
    g_dir.invertForward = config::GetBool("Directional", "InvertForward", false);

    g_banner = config::GetBool("Debug", "Banner", false);

    log::Writef("movement: directional %s keys %s/%s/%s/%s invertStrafe %d",
                g_dir.enabled ? "ON" : "off", config::KeyName(g_dir.keyForward),
                config::KeyName(g_dir.keyBack), config::KeyName(g_dir.keyLeft),
                config::KeyName(g_dir.keyRight), g_dir.invertStrafe ? 1 : 0);
    log::Writef("movement: sprint %s%s key %s x%.2f ramp %.2fs combat %d | crouch %s key %s | "
                "jump %s key %s h %.2f | roll %s key %s boost x%.2f | banner %d",
                g_sprint.enabled ? "ON" : "off", g_sprint.alwaysOn ? " (ALWAYS ON)" : "",
                config::KeyName(g_sprint.key), g_sprint.factor,
                g_sprint.rampSeconds, g_sprint.allowInCombat ? 1 : 0,
                g_crouch.enabled ? "ON" : "off", config::KeyName(g_crouch.key),
                g_jump.enabled ? "ON" : "off", config::KeyName(g_jump.key), g_jump.height,
                g_roll.enabled ? "ON" : "off", config::KeyName(g_roll.key), g_roll.boost,
                g_banner ? 1 : 0);
}

}  // namespace

bool Install() {
    if (g_installed) return true;
    QueryPerformanceFrequency(&g_qpcFreq);
    if (g_qpcFreq.QuadPart == 0) g_qpcFreq.QuadPart = 1;

    if (!config::Present()) {
        log::Write("movement: no k2se_movement.ini -> features off, nothing redirected");
        return false;
    }
    ReadConfig();
    const bool anySpeedFeature = g_sprint.enabled || g_crouch.enabled || g_roll.enabled ||
                                 g_jump.enabled || g_dir.enabled;
    if (!anySpeedFeature) {
        log::Write("movement: every feature disabled in the ini -> nothing redirected");
        return false;
    }

    input::Track(g_sprint.key);
    input::Track(g_crouch.key);
    input::Track(g_jump.key);
    input::Track(g_roll.key);
    if (g_dir.enabled) {
        input::Track(g_dir.keyForward);
        input::Track(g_dir.keyBack);
        input::Track(g_dir.keyLeft);
        input::Track(g_dir.keyRight);
    }

    // Per-frame entry: the controller's virtual Update. Directional movement
    // needs it (axes must be written before the engine reads them); the other
    // features fall back to the SetDriveSpeed boundary if it is refused.
    if (!InstallUpdateHook() && g_dir.enabled) {
        log::Write("movement: directional movement needs the Update hook -> directional off");
        g_dir.enabled = false;
    }

    // The frame hook: SetDriveSpeed at the end of Update. Without it nothing works,
    // so a refusal here rolls everything back.
    if (!callsite::Redirect("SetDriveSpeed (Update tail)", kSetDriveSpeedSite, kSetDriveSpeed,
                            reinterpret_cast<const void*>(&HookSetDriveSpeed))) {
        log::Write("movement: SetDriveSpeed redirection refused -> features off");
        callsite::RestoreAll();
        return false;
    }
    // GetMaxSpeed: pass-through instrumentation (controller pointer, call count).
    bool ok = true;
    ok &= callsite::Redirect("GetMaxSpeed#1 (Update)", kGetMaxSpeedSite1, kGetMaxSpeed,
                             reinterpret_cast<const void*>(&HookGetMaxSpeed));
    ok &= callsite::Redirect("GetMaxSpeed#2 (Update)", kGetMaxSpeedSite2, kGetMaxSpeed,
                             reinterpret_cast<const void*>(&HookGetMaxSpeed));
    ok &= callsite::Redirect("GetMaxSpeed#3 (GetAcceleration)", kGetMaxSpeedSite3, kGetMaxSpeed,
                             reinterpret_cast<const void*>(&HookGetMaxSpeed));
    if (!ok) log::Write("movement: GetMaxSpeed instrumentation incomplete (harmless, pass-through)");

    if (g_jump.enabled) {
        int redirected = 0;
        for (int i = 0; i < kMoverSetPositionSiteCount; ++i) {
            static char names[kMoverSetPositionSiteCount][40];
            _snprintf(names[i], sizeof(names[i]), "SetPosition (site %d/%d)", i + 1,
                      kMoverSetPositionSiteCount);
            names[i][sizeof(names[i]) - 1] = '\0';
            if (callsite::Redirect(names[i], kMoverSetPositionSites[i], kSetPosition,
                                   reinterpret_cast<const void*>(&HookSetPosition)))
                ++redirected;
        }
        if (redirected == 0) {
            log::Write("movement: no SetPosition site redirected -> jump disabled (other features stay)");
            g_jump.enabled = false;
        } else if (redirected < kMoverSetPositionSiteCount) {
            log::Writef("movement: %d/%d SetPosition sites redirected (the rest refused, pass-through)",
                        redirected, kMoverSetPositionSiteCount);
        }
    }

    g_installed = true;
    log::Writef("movement: installed (%d call sites redirected)", callsite::Count());
    return true;
}

void Remove() {
    if (!g_installed) return;
    if (g_crouching && g_refsValid) player::SetClientStealthBit(g_refs.clientCreature, false);
    RemoveUpdateHook();
    callsite::RestoreAll();
    g_installed = false;
}

int Status() {
    int s = 0;
    if (g_installed) s |= kInstalled;
    if (g_sprinting) s |= kSprinting;
    if (g_crouching) s |= kCrouching;
    if (g_airborne) s |= kAirborne;
    if (g_rolling) s |= kRolling;
    if (g_sprint.enabled) s |= kSprintEnabled;
    if (g_crouch.enabled) s |= kCrouchEnabled;
    if (g_jump.enabled) s |= kJumpEnabled;
    if (g_roll.enabled) s |= kRollEnabled;
    return s;
}

void SetSprintFactor(float factor) {
    g_scriptSprintFactor = factor > 0.0f ? factor : -1.0f;
    log::Writef("movement: script set sprint factor %.2f", factor);
}

bool SetCrouch(bool on) {
    if (!g_installed || !g_crouch.enabled) return false;
    SetCrouchState(on, "script");
    return true;
}

bool RequestJump() {
    if (!g_installed || !g_jump.enabled) return false;
    g_jumpRequested = true;
    return true;
}

bool RequestRoll() {
    if (!g_installed || !g_roll.enabled) return false;
    g_rollRequested = true;
    return true;
}

uint32_t UpdateCount() { return g_updateCount; }

bool PlayerInCombat() { return InCombat(); }

}  // namespace movement
}  // namespace k2se

#include "player.h"

#include <windows.h>

#include "log.h"
#include "offsets.h"

namespace k2se {
namespace player {
namespace {

// Addresses and offsets recovered on 2026-09-02; each is listed in
// data/k2se_addresses.csv with its provenance. Kept here as named constants so
// the code reads like the design document.
constexpr uint32_t kAppManagerGlobal = off::kAppManagerGlobal;     // 0x00A1B4A4
constexpr uint32_t kAppManagerOffClient = 0x04;
constexpr uint32_t kAppManagerOffServer = off::kAppManagerOffServer;  // 0x08

constexpr uint32_t kServerGetCreatureById = off::kGetCreatureByGameObjectID;  // 0x0051C100
constexpr uint32_t kClientGetObjectById = 0x0073F550;   // CClientExoApp::GetClientObject(id)
// CSWCObject::GetServerObject: the engine's own client -> server hop, used by the
// player controller itself (Update, 0x008658D6). The id stored in the controller
// is the CLIENT object's id; the server-side lookup by that id came back empty in
// the first live session (2026-09-02, "resolve -> SERVER_CREATURE"), so the
// client object is resolved first and the server object taken from it.
constexpr uint32_t kClientGetServerObject = 0x0077D800;

constexpr uint32_t kCtrlOffPlayerId = 0x04;
constexpr uint32_t kCtrlOffEnabled = 0x0C;
constexpr uint32_t kCtrlOffUpDown = 0x10;
constexpr uint32_t kCtrlOffLeftRight = 0x14;
constexpr uint32_t kCtrlOffWalking = 0x18;

constexpr uint32_t kSrvOffPosition = 0x94;
constexpr uint32_t kSrvOffOrientation = 0xA0;
constexpr uint32_t kSrvOffCombatRound = 0x520;
constexpr uint32_t kSrvOffMoveFlags = 0x1114;
constexpr uint32_t kSrvOffModeFlags = 0x1120;

constexpr uint32_t kCliOffAppearance = 0x224;
constexpr uint32_t kCliOffFlags = 0x2EC;
constexpr uint32_t kCliOffDriveSpeed = 0x3C8;

constexpr uintptr_t kMinPlausible = 0x00010000;
constexpr uintptr_t kMaxPlausible = 0x7FFFFFFF;

using GetByIdFn = void*(__thiscall*)(void* app, uint32_t id);
using GetServerObjectFn = void*(__thiscall*)(void* clientObject);

bool SafeReadPtr(const void* at, void** out) {
    __try {
        *out = *reinterpret_cast<void* const*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SafeReadU32(const void* at, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint32_t*>(at);
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
bool SafeWriteU16(void* at, uint16_t value) {
    __try {
        *reinterpret_cast<volatile uint16_t*>(at) = value;
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
bool SafeCallById(uint32_t fn, void* app, uint32_t id, void** out) {
    __try {
        *out = reinterpret_cast<GetByIdFn>(fn)(app, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SafeCallGetServerObject(void* clientObject, void** out) {
    __try {
        *out = reinterpret_cast<GetServerObjectFn>(kClientGetServerObject)(clientObject);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* At(const void* base, uint32_t off) {
    return static_cast<const char*>(base) + off;
}
char* AtW(void* base, uint32_t off) { return static_cast<char*>(base) + off; }

// Transition-only logging of resolution failures, one line per distinct stage,
// so a healthy session costs nothing and a broken one names the hop.
int g_lastStage = -1;
const char* const kStageName[] = {"OK", "APP", "SERVER_APP", "CLIENT_APP", "PLAYER_ID",
                                  "SERVER_CREATURE", "CLIENT_CREATURE", "SERVER_OBJECT_FAULT"};
void ReportStage(int stage) {
    if (stage == g_lastStage) return;
    g_lastStage = stage;
    log::Writef("player: resolve -> %s", kStageName[stage]);
}

}  // namespace

bool LooksLikePointer(const void* p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= kMinPlausible && v <= kMaxPlausible && (v & 3) == 0;
}

void* ServerApp() {
    void* mgr = nullptr;
    if (!SafeReadPtr(reinterpret_cast<const void*>(kAppManagerGlobal), &mgr) || !LooksLikePointer(mgr))
        return nullptr;
    void* app = nullptr;
    if (!SafeReadPtr(At(mgr, kAppManagerOffServer), &app) || !LooksLikePointer(app)) return nullptr;
    return app;
}

void* ClientApp() {
    void* mgr = nullptr;
    if (!SafeReadPtr(reinterpret_cast<const void*>(kAppManagerGlobal), &mgr) || !LooksLikePointer(mgr))
        return nullptr;
    void* app = nullptr;
    if (!SafeReadPtr(At(mgr, kAppManagerOffClient), &app) || !LooksLikePointer(app)) return nullptr;
    return app;
}

void* ServerCreatureById(uint32_t id) {
    if (id == off::kObjectInvalid) return nullptr;
    void* app = ServerApp();
    if (!app) return nullptr;
    void* cre = nullptr;
    if (!SafeCallById(kServerGetCreatureById, app, id, &cre)) {
        log::Write("player: GetCreatureByGameObjectID faulted");
        return nullptr;
    }
    return LooksLikePointer(cre) ? cre : nullptr;
}

void* ClientCreatureById(uint32_t id) {
    if (id == off::kObjectInvalid) return nullptr;
    void* app = ClientApp();
    if (!app) return nullptr;
    void* cre = nullptr;
    if (!SafeCallById(kClientGetObjectById, app, id, &cre)) {
        log::Write("player: client GetObjectById faulted");
        return nullptr;
    }
    return LooksLikePointer(cre) ? cre : nullptr;
}

bool Resolve(void* controller, Refs* out) {
    if (!out) return false;
    out->controller = controller;
    out->playerId = off::kObjectInvalid;
    out->serverCreature = nullptr;
    out->clientCreature = nullptr;
    out->appearance = nullptr;
    if (!LooksLikePointer(controller)) return false;

    void* mgr = nullptr;
    if (!SafeReadPtr(reinterpret_cast<const void*>(kAppManagerGlobal), &mgr) || !LooksLikePointer(mgr)) {
        ReportStage(1);
        return false;
    }
    if (!ServerApp()) {
        ReportStage(2);
        return false;
    }
    if (!ClientApp()) {
        ReportStage(3);
        return false;
    }
    uint32_t id = 0;
    if (!SafeReadU32(At(controller, kCtrlOffPlayerId), &id) || id == off::kObjectInvalid) {
        ReportStage(4);
        return false;
    }
    out->playerId = id;

    // Client first, exactly as the controller does it; the server object is the
    // one the client object points back to.
    out->clientCreature = ClientCreatureById(id);
    if (!out->clientCreature) {
        ReportStage(6);
        return false;
    }
    void* srv = nullptr;
    if (!SafeCallGetServerObject(out->clientCreature, &srv)) {
        ReportStage(7);
        return false;
    }
    if (!LooksLikePointer(srv)) {
        // Fallback for the case where the id is a server id after all.
        srv = ServerCreatureById(id);
    }
    if (!srv) {
        ReportStage(5);
        return false;
    }
    out->serverCreature = srv;

    void* app = nullptr;
    if (SafeReadPtr(At(out->clientCreature, kCliOffAppearance), &app) && LooksLikePointer(app))
        out->appearance = app;
    if (g_lastStage != 0)
        log::Writef("player: resolved id=0x%08X client=0x%08X server=0x%08X appearance=0x%08X",
                    id, reinterpret_cast<uint32_t>(out->clientCreature),
                    reinterpret_cast<uint32_t>(srv), reinterpret_cast<uint32_t>(app));
    ReportStage(0);
    return true;
}

bool ResolveFromClient(void* clientCreature, void* controller, Refs* out) {
    if (!out) return false;
    out->controller = controller;
    out->playerId = off::kObjectInvalid;
    out->serverCreature = nullptr;
    out->clientCreature = nullptr;
    out->appearance = nullptr;
    if (!LooksLikePointer(clientCreature)) {
        ReportStage(6);
        return false;
    }
    out->clientCreature = clientCreature;
    void* srv = nullptr;
    if (!SafeCallGetServerObject(clientCreature, &srv)) {
        ReportStage(7);
        return false;
    }
    if (!LooksLikePointer(srv)) {
        ReportStage(5);
        return false;
    }
    out->serverCreature = srv;
    if (LooksLikePointer(controller)) {
        uint32_t id = 0;
        if (SafeReadU32(At(controller, kCtrlOffPlayerId), &id)) out->playerId = id;
    }
    void* app = nullptr;
    if (SafeReadPtr(At(clientCreature, kCliOffAppearance), &app) && LooksLikePointer(app))
        out->appearance = app;
    if (g_lastStage != 0)
        log::Writef("player: resolved (from client) client=0x%08X server=0x%08X appearance=0x%08X "
                    "controller=0x%08X",
                    reinterpret_cast<uint32_t>(clientCreature), reinterpret_cast<uint32_t>(srv),
                    reinterpret_cast<uint32_t>(app), reinterpret_cast<uint32_t>(controller));
    ReportStage(0);
    return true;
}

bool ControllerEnabled(void* controller, bool* out) {
    uint32_t v = 0;
    if (!LooksLikePointer(controller) || !SafeReadU32(At(controller, kCtrlOffEnabled), &v)) return false;
    *out = v != 0;
    return true;
}

bool ControllerWalking(void* controller, bool* out) {
    uint32_t v = 0;
    if (!LooksLikePointer(controller) || !SafeReadU32(At(controller, kCtrlOffWalking), &v)) return false;
    *out = v != 0;
    return true;
}

bool ControllerAxes(void* controller, float* upDown, float* leftRight) {
    if (!LooksLikePointer(controller)) return false;
    return SafeReadF32(At(controller, kCtrlOffUpDown), upDown) &&
           SafeReadF32(At(controller, kCtrlOffLeftRight), leftRight);
}

bool ServerPosition(void* srv, float out[3]) {
    if (!LooksLikePointer(srv)) return false;
    for (int i = 0; i < 3; ++i)
        if (!SafeReadF32(At(srv, kSrvOffPosition + 4 * i), &out[i])) return false;
    return true;
}

bool ServerOrientation(void* srv, float out[3]) {
    if (!LooksLikePointer(srv)) return false;
    for (int i = 0; i < 3; ++i)
        if (!SafeReadF32(At(srv, kSrvOffOrientation + 4 * i), &out[i])) return false;
    return true;
}

bool ServerInCombat(void* srv, bool* out) {
    void* round = nullptr;
    if (!LooksLikePointer(srv) || !SafeReadPtr(At(srv, kSrvOffCombatRound), &round)) return false;
    *out = round != nullptr;
    return true;
}

bool ServerStealthMode(void* srv, bool* out) {
    uint32_t flags = 0;
    if (!LooksLikePointer(srv) || !SafeReadU32(At(srv, kSrvOffModeFlags), &flags)) return false;
    *out = (flags & 1u) != 0;
    return true;
}

bool ServerMoveFlags(void* srv, uint16_t* out) {
    if (!LooksLikePointer(srv)) return false;
    return SafeReadU16(At(srv, kSrvOffMoveFlags), out);
}

bool ClientStealthBit(void* cli, bool* out) {
    uint16_t flags = 0;
    if (!LooksLikePointer(cli) || !SafeReadU16(At(cli, kCliOffFlags), &flags)) return false;
    *out = (flags & 1u) != 0;
    return true;
}

bool SetClientStealthBit(void* cli, bool on) {
    uint16_t flags = 0;
    if (!LooksLikePointer(cli) || !SafeReadU16(At(cli, kCliOffFlags), &flags)) return false;
    const uint16_t updated = on ? static_cast<uint16_t>(flags | 1u) : static_cast<uint16_t>(flags & ~1u);
    if (updated == flags) return true;
    return SafeWriteU16(AtW(cli, kCliOffFlags), updated);
}

bool ClientDriveSpeed(void* cli, float* out) {
    if (!LooksLikePointer(cli)) return false;
    return SafeReadF32(At(cli, kCliOffDriveSpeed), out);
}

bool AppearanceFloat(void* appearance, uint32_t offset, float* out) {
    if (!LooksLikePointer(appearance) || offset > 0x200) return false;
    return SafeReadF32(At(appearance, offset), out);
}

}  // namespace player
}  // namespace k2se

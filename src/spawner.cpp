#include "spawner.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "exostring.h"
#include "gameobj.h"
#include "input.h"
#include "log.h"
#include "offsets.h"
#include "player.h"

namespace k2se {
namespace spawner {
namespace {

constexpr uint32_t kRunScript = 0x006FD8D0;   // CVirtualMachine::RunScript, thiscall, ret 0xC
constexpr uint32_t kObjectInvalid = 0x7F000000;
constexpr int kMaxEntries = 128;
constexpr int kTypeCreature = 1;
constexpr int kTypePlaceable = 64;
constexpr float kPi = 3.14159265f;

using RunScriptFn = int(__fastcall*)(void* vm, void* edx, void* exoName, uint32_t objectId, int flag);

struct Entry {
    int type = kTypePlaceable;
    char tpl[17] = "";
    char area[33] = "";        // empty = any area of the module
    float x = 0, y = 0, z = 0, facing = 0;
    bool present = false;      // the script found it in the area this pass
    bool spawned = false;
    int fails = 0;
};

struct Cfg {
    bool enabled = false;
    int keyCapture = VK_F10;
    float pollSeconds = 4.0f;
    float areaDelay = 1.0f;
    char script[33] = "k2se_spawn";
    bool banner = true;
};
Cfg g_cfg;

Entry g_entries[kMaxEntries];
int g_count = 0;
char g_module[65] = "";
char g_area[65] = "";
char g_gameDir[MAX_PATH] = "";
bool g_installed = false;
float g_poll = 0.0f;
float g_pending = -1.0f;      // >= 0: seconds until the post-area-setup run
uint32_t g_runs = 0;
uint32_t g_runFailures = 0;
uint32_t g_begins = 0;
int g_captured = 0;
int g_bannerFrames = 0;
char g_bannerText[128] = "";

bool ReadFileAll(const char* path, char** out, DWORD* size) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz > 256 * 1024) {
        CloseHandle(h);
        return false;
    }
    char* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, sz + 1));
    if (!buf) {
        CloseHandle(h);
        return false;
    }
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf, sz, &read, nullptr);
    CloseHandle(h);
    if (!ok) {
        HeapFree(GetProcessHeap(), 0, buf);
        return false;
    }
    buf[read] = '\0';
    *out = buf;
    *size = read;
    return true;
}

char* Trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = '\0';
    return s;
}

void StripComment(char* s) {
    for (char* p = s; *p; ++p)
        if ((*p == ';' || *p == '#') && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            return;
        }
}

int ParseType(const char* v) {
    if (_stricmp(v, "creature") == 0 || _stricmp(v, "npc") == 0) return kTypeCreature;
    if (_stricmp(v, "placeable") == 0) return kTypePlaceable;
    const int n = atoi(v);
    return n == kTypeCreature ? kTypeCreature : kTypePlaceable;
}

// <game>\k2se_spawns\<MODULE>.ini -> g_entries. Every [section] with a Template is an entry.
void LoadModule(const char* module) {
    g_count = 0;
    _snprintf(g_module, sizeof(g_module), "%s", module);
    g_module[sizeof(g_module) - 1] = '\0';
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%sk2se_spawns\\%s.ini", g_gameDir, module);
    path[sizeof(path) - 1] = '\0';
    char* buf = nullptr;
    DWORD size = 0;
    if (!ReadFileAll(path, &buf, &size)) {
        log::Writef("spawner: module %s: no %s -> nothing to spawn here", module, path);
        return;
    }
    Entry cur;
    bool inSection = false;
    char* line = buf;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next++ = '\0';
        StripComment(line);
        char* s = Trim(line);
        if (*s == '[') {
            if (inSection && cur.tpl[0] && g_count < kMaxEntries) g_entries[g_count++] = cur;
            cur = Entry();
            inSection = true;
        } else if (inSection && *s) {
            char* eq = strchr(s, '=');
            if (eq) {
                *eq = '\0';
                const char* k = Trim(s);
                const char* v = Trim(eq + 1);
                if (_stricmp(k, "Type") == 0) cur.type = ParseType(v);
                else if (_stricmp(k, "Template") == 0) { _snprintf(cur.tpl, sizeof(cur.tpl), "%s", v); cur.tpl[16] = '\0'; }
                else if (_stricmp(k, "Area") == 0) { _snprintf(cur.area, sizeof(cur.area), "%s", v); cur.area[32] = '\0'; }
                else if (_stricmp(k, "X") == 0) cur.x = static_cast<float>(atof(v));
                else if (_stricmp(k, "Y") == 0) cur.y = static_cast<float>(atof(v));
                else if (_stricmp(k, "Z") == 0) cur.z = static_cast<float>(atof(v));
                else if (_stricmp(k, "Facing") == 0) cur.facing = static_cast<float>(atof(v));
            }
        }
        line = next;
    }
    if (inSection && cur.tpl[0] && g_count < kMaxEntries) g_entries[g_count++] = cur;
    HeapFree(GetProcessHeap(), 0, buf);
    log::Writef("spawner: module %s: %d entr%s from %s", module, g_count, g_count == 1 ? "y" : "ies", path);
}

bool ValidIndex(int index) { return index >= 1 && index <= g_count; }

bool EntryForArea(const Entry& e) { return e.area[0] == '\0' || _stricmp(e.area, g_area) == 0; }

// The PC's server object id, self-validated through the engine's own lookup
// (CGameObject+4 is documented by KPM but unverified; the round trip proves it).
uint32_t PlayerObjectId(void* serverCreature) {
    if (!player::LooksLikePointer(serverCreature)) return kObjectInvalid;
    uint32_t id = 0;
    __try {
        id = *reinterpret_cast<volatile uint32_t*>(static_cast<char*>(serverCreature) + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kObjectInvalid;
    }
    if (id == 0 || id == kObjectInvalid) return kObjectInvalid;
    return gameobj::CreatureFromObjectId(id) == serverCreature ? id : kObjectInvalid;
}

// SEH and C++ objects with destructors cannot share a function (C2712), so the
// guarded parts live in these two helpers.
void* ReadVirtualMachine() {
    __try {
        return *reinterpret_cast<void* volatile*>(off::kVirtualMachineGlobal);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool CallRunScript(void* vm, void* exoName, uint32_t oid, int* rc) {
    __try {
        *rc = reinterpret_cast<RunScriptFn>(kRunScript)(vm, nullptr, exoName, oid, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RunSpawnScript(void* serverCreature) {
    void* vm = ReadVirtualMachine();
    if (!player::LooksLikePointer(vm)) {
        if (g_runFailures++ < 3) log::Write("spawner: CVirtualMachine singleton not readable -> no run");
        return false;
    }
    ExoString name(g_cfg.script);
    if (!name.valid()) return false;
    const uint32_t oid = PlayerObjectId(serverCreature);
    int rc = 0;
    if (!CallRunScript(vm, name.raw(), oid, &rc)) {
        if (g_runFailures++ < 3) log::Write("spawner: RunScript faulted -> spawner off for this session");
        g_cfg.enabled = false;
        return false;
    }
    ++g_runs;
    if (g_runs <= 3 || (g_runs % 50) == 0)
        log::Writef("spawner: RunScript(%s, self 0x%08X) -> %d  [run %u]", g_cfg.script, oid, rc, g_runs);
    return true;
}

void CapturePosition(void* serverCreature) {
    float pos[3], ori[3];
    if (!player::ServerPosition(serverCreature, pos) || !player::ServerOrientation(serverCreature, ori)) return;
    const float facing = atan2f(ori[1], ori[0]) * 180.0f / kPi;
    ++g_captured;
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%sk2se_spawns\\_captured.txt", g_gameDir);
    path[sizeof(path) - 1] = '\0';
    char block[320];
    _snprintf(block, sizeof(block),
              "\r\n; module %s, area %s -- captured %d\r\n[spawn%d]\r\nType=placeable\r\nTemplate=plc_crate\r\n"
              "X=%.3f\r\nY=%.3f\r\nZ=%.3f\r\nFacing=%.1f\r\n",
              g_module[0] ? g_module : "?", g_area[0] ? g_area : "?", g_captured, g_captured, pos[0], pos[1], pos[2],
              facing);
    block[sizeof(block) - 1] = '\0';
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, block, static_cast<DWORD>(strlen(block)), &written, nullptr);
        CloseHandle(h);
    }
    log::Writef("spawner: captured module %s area %s pos (%.3f %.3f %.3f) facing %.1f -> %s", g_module, g_area,
                pos[0], pos[1], pos[2], facing, path);
    if (g_cfg.banner) {
        // AurPostString shows a string for the frame it is posted in, whatever the
        // life argument says (session S1: a one-shot post was never seen), so the
        // text is re-posted for a couple of seconds from OnGameplayFrame.
        _snprintf(g_bannerText, sizeof(g_bannerText), "K2SE: punto %d salvato  %.1f %.1f %.1f  dir %.0f  (%s)",
                  g_captured, pos[0], pos[1], pos[2], facing, g_module[0] ? g_module : "?");
        g_bannerText[sizeof(g_bannerText) - 1] = '\0';
        g_bannerFrames = 150;
    }
}

void ReadConfig() {
    g_cfg.enabled = config::GetBool("Spawner", "Enabled", false);
    g_cfg.keyCapture = config::GetKey("Spawner", "KeyCapture", VK_F10);
    g_cfg.pollSeconds = config::GetFloat("Spawner", "PollSeconds", 4.0f);
    g_cfg.areaDelay = config::GetFloat("Spawner", "AreaDelaySeconds", 1.0f);
    g_cfg.banner = config::GetBool("Spawner", "Banner", true);
    const char* script = config::GetString("Spawner", "Script", "k2se_spawn");
    _snprintf(g_cfg.script, sizeof(g_cfg.script), "%s", script);
    g_cfg.script[sizeof(g_cfg.script) - 1] = '\0';
    if (g_cfg.pollSeconds < 1.0f) g_cfg.pollSeconds = 1.0f;
    if (g_cfg.areaDelay < 0.2f) g_cfg.areaDelay = 0.2f;
    log::Writef("spawner: %s script %s poll %.1fs area delay %.1fs capture key %s", g_cfg.enabled ? "ON" : "off",
                g_cfg.script, g_cfg.pollSeconds, g_cfg.areaDelay, config::KeyName(g_cfg.keyCapture));
}

}  // namespace

bool Install() {
    if (g_installed) return true;
    if (!config::Present()) return false;
    ReadConfig();
    if (!g_cfg.enabled) return false;
    if (!GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH)) return false;
    char* slash = strrchr(g_gameDir, '\\');
    if (!slash) return false;
    slash[1] = '\0';
    input::Track(g_cfg.keyCapture);
    g_pending = g_cfg.areaDelay;   // first run shortly after the first gameplay frame
    g_installed = true;
    log::Writef("spawner: installed; data in %sk2se_spawns\\<MODULE>.ini", g_gameDir);
    return true;
}

void Remove() {
    if (!g_installed) return;
    log::Writef("spawner: removed after %u script runs, %u Begin calls, %d captured points", g_runs, g_begins,
                g_captured);
    g_installed = false;
}

int Status() {
    int s = g_installed ? 1 : 0;
    if (g_count > 0) s |= 2;
    return s;
}

void OnGameplayFrame(void* serverCreature, bool refsValid, float dt) {
    if (!g_installed || !g_cfg.enabled || !refsValid) return;
    if (dt < 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;
    if (input::Pressed(g_cfg.keyCapture)) CapturePosition(serverCreature);
    if (g_bannerFrames > 0) {
        --g_bannerFrames;
        using AurPostStringFn = void(__cdecl*)(const char*, int, int, float);
        reinterpret_cast<AurPostStringFn>(0x00474C00)(g_bannerText, 5, 115, 0.6f);
    }
    bool run = false;
    if (g_pending >= 0.0f) {
        g_pending -= dt;
        if (g_pending < 0.0f) run = true;
    }
    g_poll += dt;
    if (g_poll >= g_cfg.pollSeconds) {
        g_poll = 0.0f;
        run = true;
    }
    if (run) RunSpawnScript(serverCreature);
}

void OnAreaSetup() {
    if (!g_installed) return;
    g_pending = g_cfg.areaDelay;
}

int Begin(const char* module, const char* areaTag) {
    if (!g_installed) return 0;
    ++g_begins;
    if (!module) module = "";
    if (!areaTag) areaTag = "";
    if (_stricmp(module, g_module) != 0) LoadModule(module);
    if (_stricmp(areaTag, g_area) != 0) {
        _snprintf(g_area, sizeof(g_area), "%s", areaTag);
        g_area[sizeof(g_area) - 1] = '\0';
        log::Writef("spawner: area %s (module %s)", g_area, g_module);
    }
    for (int i = 0; i < g_count; ++i) g_entries[i].present = false;
    return g_count;
}

bool MarkPresent(int index) {
    if (!ValidIndex(index)) return false;
    g_entries[index - 1].present = true;
    return true;
}

bool Needed(int index) {
    if (!ValidIndex(index)) return false;
    const Entry& e = g_entries[index - 1];
    return EntryForArea(e) && !e.present && e.fails < 3;
}

int Type(int index) { return ValidIndex(index) ? g_entries[index - 1].type : kTypePlaceable; }
const char* Template(int index) { return ValidIndex(index) ? g_entries[index - 1].tpl : ""; }
float X(int index) { return ValidIndex(index) ? g_entries[index - 1].x : 0.0f; }
float Y(int index) { return ValidIndex(index) ? g_entries[index - 1].y : 0.0f; }
float Z(int index) { return ValidIndex(index) ? g_entries[index - 1].z : 0.0f; }
float Facing(int index) { return ValidIndex(index) ? g_entries[index - 1].facing : 0.0f; }

bool Report(int index, bool ok) {
    if (!ValidIndex(index)) return false;
    Entry& e = g_entries[index - 1];
    if (ok) {
        e.spawned = true;
        e.present = true;
        log::Writef("spawner: [spawn%d] %s %s at (%.2f %.2f %.2f) created in %s", index,
                    e.type == kTypeCreature ? "creature" : "placeable", e.tpl, e.x, e.y, e.z, g_area);
    } else {
        ++e.fails;
        log::Writef("spawner: [spawn%d] %s FAILED (%d/3) -- template missing or bad position?", index, e.tpl, e.fails);
    }
    return true;
}

}  // namespace spawner
}  // namespace k2se

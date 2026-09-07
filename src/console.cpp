#include "console.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "camera.h"
#include "config.h"
#include "fpcam.h"
#include "input.h"
#include "log.h"
#include "player.h"
#include "spawner.h"

namespace k2se {
namespace console {
namespace {

constexpr int kMaxLine = 256;
constexpr int kQueueSize = 16;
constexpr int kTypeCreature = 1;
constexpr int kTypePlaceable = 64;
constexpr float kPi = 3.14159265f;

// --- the catalogue ------------------------------------------------------------
// One row per object, read once from k2se_catalog.csv next to the exe. The file
// is ~150 KB; the strings are kept in the file image itself and the rows only
// point into it, so loading costs one allocation and one pass.
struct CatalogRow {
    const char* kind;
    const char* resref;
    const char* name;
    const char* model;
    const char* source;
};

constexpr int kMaxRows = 4096;
CatalogRow g_rows[kMaxRows];
int g_rowCount = 0;
char* g_catalog = nullptr;

// --- the command queue --------------------------------------------------------
CRITICAL_SECTION g_lock;
bool g_lockReady = false;
char g_queue[kQueueSize][kMaxLine];
int g_queueHead = 0;
int g_queueTail = 0;

struct Cfg {
    bool enabled = false;
    int keyToggle = VK_F10;
    bool openAtStart = false;
};
Cfg g_cfg;

struct State {
    bool installed = false;
    bool consoleOwned = false;   // we called AllocConsole and must free it
    bool visible = false;
    HANDLE thread = nullptr;
    volatile LONG stop = 0;
    char gameDir[MAX_PATH] = "";
    uint32_t commands = 0;
    int lastListed[16];          // resrefs from the last `list`, for `spawn #n`
    int lastListedCount = 0;
};
State g_st;

// --- output -------------------------------------------------------------------
void Out(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    DWORD written = 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        WriteConsoleA(h, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        WriteConsoleA(h, "\r\n", 2, &written, nullptr);
    }
}

void Prompt() {
    DWORD written = 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) WriteConsoleA(h, "k2se> ", 6, &written, nullptr);
}

// --- catalogue loading --------------------------------------------------------
char* NextField(char* p, const char** out) {
    *out = p;
    while (*p && *p != ',' && *p != '\r' && *p != '\n') ++p;
    const bool endOfLine = (*p != ',');
    if (*p) *p++ = '\0';
    if (endOfLine) {
        while (*p == '\n' || *p == '\r') ++p;
        return p;
    }
    return p;
}

bool LoadCatalog() {
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%sk2se_catalog.csv", g_st.gameDir);
    path[sizeof(path) - 1] = '\0';
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log::Writef("console: no %s -- the object list will be empty "
                    "(run tools/gen_catalog.py and redeploy)", path);
        return false;
    }
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size > 4 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    g_catalog = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, size + 1));
    if (!g_catalog) {
        CloseHandle(h);
        return false;
    }
    DWORD read = 0;
    const BOOL ok = ReadFile(h, g_catalog, size, &read, nullptr);
    CloseHandle(h);
    if (!ok) {
        HeapFree(GetProcessHeap(), 0, g_catalog);
        g_catalog = nullptr;
        return false;
    }
    g_catalog[read] = '\0';

    char* p = g_catalog;
    // Skip the header line.
    while (*p && *p != '\n') ++p;
    if (*p) ++p;

    while (*p && g_rowCount < kMaxRows) {
        const char* tag = nullptr;
        const char* appearance = nullptr;
        CatalogRow row;
        p = NextField(p, &row.kind);
        p = NextField(p, &row.resref);
        p = NextField(p, &row.name);
        p = NextField(p, &tag);
        p = NextField(p, &appearance);
        p = NextField(p, &row.model);
        p = NextField(p, &row.source);
        if (!row.kind || !*row.kind) continue;
        g_rows[g_rowCount++] = row;
    }
    log::Writef("console: catalogue loaded, %d objects from %s", g_rowCount, path);
    return g_rowCount > 0;
}

bool ContainsNoCase(const char* hay, const char* needle) {
    if (!hay || !needle || !*needle) return true;
    const size_t n = strlen(needle);
    for (const char* p = hay; *p; ++p)
        if (_strnicmp(p, needle, n) == 0) return true;
    return false;
}

// --- commands -----------------------------------------------------------------
void CmdHelp() {
    Out("");
    Out("  list <text>          search the object catalogue (%d objects)", g_rowCount);
    Out("  spawn <resref>       place that object where you are standing");
    Out("  spawn #<n>           place the nth object from the last list");
    Out("  npc <resref>         place a creature where you are standing");
    Out("  here                 print the module, area and your position");
    Out("  placed               list what this session has placed");
    Out("  undo                 drop the last placement from the table");
    Out("  save                 append the placements to k2se_spawns\\<MODULE>.ini");
    Out("  eyes                 the measured eye point of your character");
    Out("  view <0-3>           camera view: 0 game, 1 near, 2 far, 3 first person");
    Out("  help                 this");
    Out("");
    Out("  An object appears within a few seconds -- the spawn pass is periodic.");
    Out("  Placements are lost on restart until you `save` them.");
}

// The catalogue holds placeables and doors. Creatures are not listed: there are
// thousands of .utc blueprints and almost all of them are one specific scripted
// NPC, so `npc <resref>` takes a name directly rather than pretending to browse.
void CmdList(const char* filter) {
    g_st.lastListedCount = 0;
    int shown = 0;
    int matched = 0;
    for (int i = 0; i < g_rowCount; ++i) {
        const CatalogRow& r = g_rows[i];
        if (!ContainsNoCase(r.name, filter) && !ContainsNoCase(r.resref, filter) &&
            !ContainsNoCase(r.model, filter))
            continue;
        ++matched;
        if (shown >= 40) continue;
        if (r.resref && *r.resref && g_st.lastListedCount < 16)
            g_st.lastListed[g_st.lastListedCount++] = i;
        const int pick = (r.resref && *r.resref) ? g_st.lastListedCount : 0;
        if (pick)
            Out("  #%-2d %-9s %-18s %-32.32s model %-14s %s", pick, r.kind, r.resref, r.name,
                r.model, r.source);
        else
            Out("      %-9s %-18s %-32.32s model %-14s %s", r.kind, "(model only)", r.name,
                r.model, r.source);
        ++shown;
    }
    if (matched == 0) {
        Out("  nothing matches \"%s\"", filter ? filter : "");
        return;
    }
    Out("  %d match%s%s. Place one with `spawn <resref>` or `spawn #<n>`.", matched,
        matched == 1 ? "" : "es", matched > shown ? " (first 40 shown)" : "");
}

bool PlayerPlacement(const player::Refs& refs, float pos[3], float* facing) {
    if (!player::LooksLikePointer(refs.serverCreature)) return false;
    float ori[3];
    if (!player::ServerPosition(refs.serverCreature, pos) ||
        !player::ServerOrientation(refs.serverCreature, ori))
        return false;
    *facing = atan2f(ori[1], ori[0]) * 180.0f / kPi;
    return true;
}

void CmdSpawn(const player::Refs& refs, const char* what, int type) {
    if (!what || !*what) {
        Out("  spawn what? Try `list plant` first.");
        return;
    }
    const char* resref = what;
    if (what[0] == '#') {
        const int pick = atoi(what + 1);
        if (pick < 1 || pick > g_st.lastListedCount) {
            Out("  #%d is not in the last list (%d entries)", pick, g_st.lastListedCount);
            return;
        }
        resref = g_rows[g_st.lastListed[pick - 1]].resref;
    }
    float pos[3] = {0, 0, 0};
    float facing = 0.0f;
    if (!PlayerPlacement(refs, pos, &facing)) {
        Out("  cannot read your position right now -- are you in a loaded area?");
        return;
    }
    const int index = spawner::AddRuntimeEntry(type, resref, pos[0], pos[1], pos[2], facing);
    if (!index) {
        Out("  could not add it: the spawn table is full, or the spawner is off in the ini.");
        return;
    }
    Out("  placing %s at %.2f %.2f %.2f facing %.0f (entry %d) -- give it a moment", resref,
        pos[0], pos[1], pos[2], facing, index);
}

void CmdHere(const player::Refs& refs) {
    float pos[3] = {0, 0, 0};
    float facing = 0.0f;
    if (!PlayerPlacement(refs, pos, &facing)) {
        Out("  position unavailable -- not in a loaded area?");
        return;
    }
    Out("  module %s  area %s", spawner::ModuleName(), spawner::AreaName());
    Out("  position %.3f %.3f %.3f  facing %.1f", pos[0], pos[1], pos[2], facing);
    Out("  %d entr%s in the spawn table", spawner::Count(),
        spawner::Count() == 1 ? "y" : "ies");
}

void CmdEyes() {
    float forward = 0.0f;
    float height = 0.0f;
    if (!fpcam::EyeOffset(&forward, &height)) {
        Out("  no eye measurement for this character "
            "([FirstPerson] Enabled=0, or an unmeasured appearance)");
        return;
    }
    Out("  eye point: %d mm forward, %d mm up, in model space",
        static_cast<int>(forward * 1000.0f), static_cast<int>(height * 1000.0f));
    float live[3];
    if (fpcam::GetEyePosition(live))
        Out("  live anchor: %d %d %d mm", static_cast<int>(live[0] * 1000.0f),
            static_cast<int>(live[1] * 1000.0f), static_cast<int>(live[2] * 1000.0f));
}

void Execute(const player::Refs& refs, char* line) {
    while (*line == ' ' || *line == '\t') ++line;
    char* end = line + strlen(line);
    while (end > line && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) *--end = '\0';
    if (!*line) return;
    ++g_st.commands;

    char* arg = strchr(line, ' ');
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ') ++arg;
    }

    if (_stricmp(line, "help") == 0 || _stricmp(line, "?") == 0) {
        CmdHelp();
    } else if (_stricmp(line, "list") == 0) {
        CmdList(arg);
    } else if (_stricmp(line, "spawn") == 0) {
        CmdSpawn(refs, arg, kTypePlaceable);
    } else if (_stricmp(line, "npc") == 0) {
        CmdSpawn(refs, arg, kTypeCreature);
    } else if (_stricmp(line, "here") == 0) {
        CmdHere(refs);
    } else if (_stricmp(line, "placed") == 0) {
        Out("  %d entr%s in the spawn table for %s", spawner::Count(),
            spawner::Count() == 1 ? "y" : "ies", spawner::ModuleName());
    } else if (_stricmp(line, "undo") == 0) {
        const int n = spawner::Count();
        Out(spawner::RemoveEntry(n) ? "  entry %d removed (the object already in the area "
                                      "stays until the module reloads)"
                                    : "  nothing to undo",
            n);
    } else if (_stricmp(line, "save") == 0) {
        const int n = spawner::Persist();
        Out("  %d entr%s written to k2se_spawns\\%s.ini", n, n == 1 ? "y" : "ies",
            spawner::ModuleName());
    } else if (_stricmp(line, "eyes") == 0) {
        CmdEyes();
    } else if (_stricmp(line, "view") == 0) {
        const int v = arg ? atoi(arg) : -1;
        Out(camera::SetView(v) ? "  camera view %d" : "  view must be 0..3 and the camera "
                                                      "module must be on",
            v);
    } else {
        Out("  unknown command \"%s\" -- try `help`", line);
    }
}

// --- the reader thread --------------------------------------------------------
// Blocks on the console, pushes lines, touches nothing else. Every game object
// in this file is reached only from Drain(), which runs on the game thread.
DWORD WINAPI ReaderThread(LPVOID) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char line[kMaxLine];
    while (!InterlockedCompareExchange(&g_st.stop, 0, 0)) {
        Prompt();
        DWORD read = 0;
        if (!ReadConsoleA(in, line, kMaxLine - 1, &read, nullptr)) {
            Sleep(100);
            continue;
        }
        line[read < kMaxLine ? read : kMaxLine - 1] = '\0';
        EnterCriticalSection(&g_lock);
        const int next = (g_queueTail + 1) % kQueueSize;
        if (next != g_queueHead) {
            _snprintf(g_queue[g_queueTail], kMaxLine, "%s", line);
            g_queue[g_queueTail][kMaxLine - 1] = '\0';
            g_queueTail = next;
        }
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

void Drain(const player::Refs& refs) {
    for (;;) {
        char line[kMaxLine];
        EnterCriticalSection(&g_lock);
        const bool empty = (g_queueHead == g_queueTail);
        if (!empty) {
            memcpy(line, g_queue[g_queueHead], kMaxLine);
            g_queueHead = (g_queueHead + 1) % kQueueSize;
        }
        LeaveCriticalSection(&g_lock);
        if (empty) return;
        Execute(refs, line);
    }
}

void OpenWindow() {
    if (g_st.visible) return;
    if (!GetConsoleWindow()) {
        if (!AllocConsole()) {
            log::Writef("console: AllocConsole failed (%lu)", GetLastError());
            return;
        }
        g_st.consoleOwned = true;
    }
    SetConsoleTitleA("K2SE object console");
    // freopen is avoided on purpose: the log module explains why this DLL keeps
    // clear of the CRT's stdio, and WriteConsoleA/ReadConsoleA need no plumbing.
    g_st.visible = true;
    HWND hwnd = GetConsoleWindow();
    if (hwnd) ShowWindow(hwnd, SW_SHOW);

    Out("");
    Out("K2SE object console -- %d objects catalogued", g_rowCount);
    Out("The game keeps running. Type `help`, or `list plant` to start.");
    if (spawner::Status() == 0)
        Out("WARNING: the spawner is off in k2se_movement.ini, so nothing can be placed.");
    Out("");

    if (!g_st.thread) {
        g_st.stop = 0;
        g_st.thread = CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr);
        if (!g_st.thread) log::Write("console: reader thread could not start");
    }
    log::Write("console: window opened");
}

void HideWindow() {
    if (!g_st.visible) return;
    HWND hwnd = GetConsoleWindow();
    if (hwnd) ShowWindow(hwnd, SW_HIDE);
    g_st.visible = false;
    log::Write("console: window hidden");
}

void ReadConfig() {
    g_cfg.enabled = config::GetBool("Console", "Enabled", false);
    g_cfg.keyToggle = config::GetKey("Console", "KeyToggle", VK_F10);
    g_cfg.openAtStart = config::GetBool("Console", "OpenAtStart", false);
    log::Writef("console: %s toggle %s openAtStart %d", g_cfg.enabled ? "ON" : "off",
                config::KeyName(g_cfg.keyToggle), g_cfg.openAtStart ? 1 : 0);
}

}  // namespace

bool Install() {
    if (g_st.installed) return true;
    if (!config::Present()) return false;
    ReadConfig();
    if (!g_cfg.enabled) return false;
    if (!GetModuleFileNameA(nullptr, g_st.gameDir, MAX_PATH)) return false;
    char* slash = strrchr(g_st.gameDir, '\\');
    if (!slash) return false;
    slash[1] = '\0';

    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
    LoadCatalog();
    input::Track(g_cfg.keyToggle);
    g_st.installed = true;
    log::Writef("console: installed; toggle with %s", config::KeyName(g_cfg.keyToggle));
    if (g_cfg.openAtStart) OpenWindow();
    return true;
}

void Remove() {
    if (!g_st.installed) return;
    InterlockedExchange(&g_st.stop, 1);
    if (g_st.thread) {
        // The reader is parked inside ReadConsoleA. Closing the input handle is
        // what releases it; a bounded wait then keeps shutdown from hanging if
        // it does not.
        CloseHandle(GetStdHandle(STD_INPUT_HANDLE));
        WaitForSingleObject(g_st.thread, 500);
        CloseHandle(g_st.thread);
        g_st.thread = nullptr;
    }
    if (g_st.consoleOwned) {
        FreeConsole();
        g_st.consoleOwned = false;
    }
    if (g_catalog) {
        HeapFree(GetProcessHeap(), 0, g_catalog);
        g_catalog = nullptr;
    }
    if (g_lockReady) {
        DeleteCriticalSection(&g_lock);
        g_lockReady = false;
    }
    log::Writef("console: removed after %u command(s)", g_st.commands);
    g_st.installed = false;
    g_st.visible = false;
}

int Status() {
    int s = g_st.installed ? 1 : 0;
    if (g_st.visible) s |= 2;
    if (g_rowCount > 0) s |= 4;
    return s;
}

void Toggle() {
    if (!g_st.installed) return;
    if (g_st.visible) {
        HideWindow();
    } else {
        OpenWindow();
    }
}

bool Visible() { return g_st.visible; }

void OnGameplayFrame(const player::Refs& refs, float dt) {
    if (!g_st.installed) return;
    (void)dt;
    if (input::Pressed(g_cfg.keyToggle)) Toggle();
    if (g_st.visible) Drain(refs);
}

}  // namespace console
}  // namespace k2se

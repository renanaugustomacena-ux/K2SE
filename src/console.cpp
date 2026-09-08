#include "console.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "anim.h"
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
    const char* tag;      // the fallback when a creature has no localized name
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
    bool inGame = true;    // draw over the game rather than in a separate window
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
    // Sixteen was the real reason "the console only offers twelve objects":
    // the catalogue held 2563 rows, the list printed forty, and only the first
    // sixteen ever got a number you could spawn.
    static const int kMaxPicks = 200;
    int lastListed[kMaxPicks];
    int lastListedCount = 0;
    int lastPage = 0;
    int lastMatched = 0;
};
State g_st;

// ShowWindow is the one USER32 function this module wants, and linking it would
// break the rule tools/check_dll.py enforces: K2SE imports KERNEL32 and nothing
// else, so that the proxy DLL can never fail to load because of a dependency.
// Resolved by hand instead; if user32 is somehow absent the window simply stays
// as the console host left it, which is harmless.
using ShowWindowFn = int(__stdcall*)(HWND, int);
ShowWindowFn g_showWindow = nullptr;

// Closing an AllocConsole window sends CTRL_CLOSE_EVENT to the HOST process,
// and Windows then terminates it. That is what killed the game when Renan shut
// the console: nothing in K2SE asked to exit, the operating system simply took
// the game down with the window.
//
// Two defences, because either alone is leaky. The handler refuses the close
// signal, and the X is removed from the system menu so the signal is not raised
// in the first place. F10 is the only way to put the console away.
BOOL WINAPI ConsoleCtrlHandler(DWORD event) {
    if (event == CTRL_CLOSE_EVENT || event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        log::Writef("console: refused console control event %lu -- the game keeps running",
                    event);
        return TRUE;   // handled; do not pass it on to the default terminator
    }
    return FALSE;
}

using GetSystemMenuFn = HMENU(__stdcall*)(HWND, BOOL);
using DeleteMenuFn = BOOL(__stdcall*)(HMENU, UINT, UINT);
using DrawMenuBarFn = BOOL(__stdcall*)(HWND);

void DisableCloseButton() {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) user32 = LoadLibraryA("user32.dll");
    if (!user32) return;
    auto getSystemMenu =
        reinterpret_cast<GetSystemMenuFn>(GetProcAddress(user32, "GetSystemMenu"));
    auto deleteMenu = reinterpret_cast<DeleteMenuFn>(GetProcAddress(user32, "DeleteMenu"));
    auto drawMenuBar = reinterpret_cast<DrawMenuBarFn>(GetProcAddress(user32, "DrawMenuBar"));
    if (!getSystemMenu || !deleteMenu) return;
    HMENU menu = getSystemMenu(hwnd, FALSE);
    if (!menu) return;
    deleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
    if (drawMenuBar) drawMenuBar(hwnd);
    log::Write("console: close button disabled -- F10 closes it, the X would kill the game");
}

void ShowConsoleWindow(int how) {
    if (!g_showWindow) {
        HMODULE user32 = GetModuleHandleA("user32.dll");
        if (!user32) user32 = LoadLibraryA("user32.dll");
        if (user32)
            g_showWindow = reinterpret_cast<ShowWindowFn>(GetProcAddress(user32, "ShowWindow"));
        if (!g_showWindow) {
            log::Write("console: user32!ShowWindow unavailable -- the window cannot be hidden");
            return;
        }
    }
    HWND hwnd = GetConsoleWindow();
    if (hwnd) g_showWindow(hwnd, how);
}

// --- the in-game overlay ------------------------------------------------------
// AurPostString draws one line for the frame it is called in, so an overlay is
// just every line re-posted every frame. x = 5 and y grows downward; the other
// modules put their one-line banners at y 55..115, so the console takes the
// block below them.
constexpr uint32_t kAurPostString = 0x00474C00;
using AurPostStringFn = void(__cdecl*)(const char*, int, int, float);

constexpr int kScrollback = 14;
constexpr int kOverlayX = 5;
constexpr int kOverlayTop = 140;
constexpr int kOverlayStep = 16;
constexpr float kOverlaySize = 0.55f;

char g_scroll[kScrollback][160];
int g_scrollCount = 0;
char g_input[160] = "";
int g_inputLen = 0;
char g_render[192];

void PushLine(const char* text) {
    if (g_scrollCount == kScrollback) {
        for (int i = 1; i < kScrollback; ++i) memcpy(g_scroll[i - 1], g_scroll[i], 160);
        --g_scrollCount;
    }
    _snprintf(g_scroll[g_scrollCount], 160, "%s", text ? text : "");
    g_scroll[g_scrollCount][159] = '\0';
    ++g_scrollCount;
}

// --- output -------------------------------------------------------------------
void Out(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';

    if (g_cfg.inGame) {
        PushLine(line);
        return;
    }
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
        const char* appearance = nullptr;
        CatalogRow row;
        p = NextField(p, &row.kind);
        p = NextField(p, &row.resref);
        p = NextField(p, &row.name);
        p = NextField(p, &row.tag);
        p = NextField(p, &appearance);
        p = NextField(p, &row.model);
        p = NextField(p, &row.source);
        if (!row.kind || !*row.kind) continue;
        g_rows[g_rowCount++] = row;
    }
    log::Writef("console: catalogue loaded, %d objects from %s", g_rowCount, path);
    return g_rowCount > 0;
}

// Many creature rows carry no localized name at all -- c_drdsentry parses as
// name "" with tag "DrdSentry" -- and a list of blank lines is what "only
// twelve items" looked like. Something readable is always shown.
const char* Readable(const CatalogRow& r) {
    if (r.name && *r.name) return r.name;
    if (r.tag && *r.tag) return r.tag;
    if (r.resref && *r.resref) return r.resref;
    return "(unnamed)";
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
    Out("  list <text>          search by NAME (%d objects, people included)", g_rowCount);
    Out("                       e.g. list plant | list footlocker | list soldier");
    Out("  spawn #<n>           place the nth thing from the last list");
    Out("                       containers get 1-3 of the game's 100 best items;");
    Out("                       people wander off on their own instead of standing still");
    Out("  spawn <resref>       same, by file name, if you know it");
    Out("  here                 print the module, area and your position");
    Out("  placed               list what this session has placed");
    Out("  undo                 drop the last placement from the table");
    Out("  save                 append the placements to k2se_spawns\\<MODULE>.ini");
    Out("  eyes                 the measured eye point of your character");
    Out("  anim <row>           play an animations.2da row on your character");
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
    // `list plant 2` asks for the second page. The filter is everything before
    // a trailing number, so names with digits in them still work.
    char text[128] = "";
    int page = 1;
    if (filter && *filter) {
        _snprintf(text, sizeof(text), "%s", filter);
        text[sizeof(text) - 1] = '\0';
        char* space = strrchr(text, ' ');
        if (space && space[1] >= '1' && space[1] <= '9') {
            page = atoi(space + 1);
            *space = '\0';
        }
    }
    if (page < 1) page = 1;

    const int perPage = 40;
    const int skip = (page - 1) * perPage;

    g_st.lastListedCount = 0;
    g_st.lastPage = page;
    int matched = 0;
    int shown = 0;
    for (int i = 0; i < g_rowCount; ++i) {
        const CatalogRow& r = g_rows[i];
        if (!ContainsNoCase(Readable(r), text) && !ContainsNoCase(r.resref, text) &&
            !ContainsNoCase(r.model, text))
            continue;
        ++matched;
        if (matched <= skip || shown >= perPage) continue;

        int pick = 0;
        if (r.resref && *r.resref && g_st.lastListedCount < State::kMaxPicks)
            pick = ++g_st.lastListedCount;
        if (pick)
            g_st.lastListed[pick - 1] = i;

        if (pick)
            Out("  %-3d %-36.36s %-9s (%s)", pick, Readable(r), r.kind, r.resref);
        else
            Out("      %-36.36s %-9s (model only, cannot be placed)", Readable(r), r.kind);
        ++shown;
    }
    g_st.lastMatched = matched;

    if (matched == 0) {
        Out("  nothing matches \"%s\"", text);
        return;
    }
    const int first = skip + 1;
    const int last = skip + shown;
    Out("  showing %d-%d of %d.  place one with `spawn <number>`", first, last, matched);
    if (last < matched)
        Out("  more: `list %s %d`", text[0] ? text : "", page + 1);
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

// The kind is in the catalogue, so asking the player to know whether something
// is a creature or a placeable is asking them to do the computer's job. `type`
// is only a fallback for a resref typed in full that is not in the catalogue.
int TypeOfRow(const CatalogRow& r) {
    return (r.kind && strstr(r.kind, "creature")) ? kTypeCreature : kTypePlaceable;
}

void CmdSpawn(const player::Refs& refs, const char* what, int type) {
    if (!what || !*what) {
        Out("  spawn what? Try `list plant` or `list soldier` first.");
        return;
    }

    // A bare number means the nth row of the last list. Requiring the '#' made
    // `spawn 1` ask the engine for a template literally called "1", which it
    // dutifully queued and which of course produced nothing -- that is why
    // placing appeared to do nothing at all.
    const char* digits = (what[0] == '#') ? what + 1 : what;
    bool numeric = *digits != 0;
    for (const char* c = digits; *c; ++c)
        if (*c < '0' || *c > '9') numeric = false;

    const char* resref = what;
    const char* label = what;
    if (numeric) {
        const int pick = atoi(digits);
        if (pick < 1 || pick > g_st.lastListedCount) {
            Out("  %d is not in the last list (it has %d numbered entries)", pick,
                g_st.lastListedCount);
            return;
        }
        const CatalogRow& row = g_rows[g_st.lastListed[pick - 1]];
        resref = row.resref;
        label = Readable(row);
        type = TypeOfRow(row);
    } else {
        // A name typed in full is checked against the catalogue before it is
        // queued, so an unknown one is refused here rather than failing
        // silently inside the engine four seconds later.
        const CatalogRow* found = nullptr;
        for (int i = 0; i < g_rowCount; ++i) {
            if (g_rows[i].resref && _stricmp(g_rows[i].resref, resref) == 0) {
                found = &g_rows[i];
                break;
            }
        }
        if (!found) {
            Out("  no object called \"%s\" -- try `list %s` and spawn it by number",
                resref, resref);
            return;
        }
        label = Readable(*found);
        type = TypeOfRow(*found);
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
    Out("  placing %s (%s) -- give it a moment", label,
        type == kTypeCreature ? "npc, it will wander off" : "object");
    if (type == kTypePlaceable)
        Out("  if it is a container it now holds 1-3 of the game's 100 best items");
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

// The engine addresses animations by animations.2da row, and the game ships 571
// of them -- plus the 40 the k2-animations override recovers, which exist in the
// supermodels but had no row. Trying one is the only way to know what it looks
// like, so the console plays it directly rather than making that a rebuild.
void CmdAnim(const player::Refs& refs, const char* arg) {
    if (!arg || !*arg) {
        Out("  anim <row> -- e.g. 2 run, 5 stealth, 23 kneel, 567 diveroll.");
        Out("  With the k2-animations override installed, 571 is walkback.");
        return;
    }
    const int row = atoi(arg);
    if (row < 0 || row > 2000) {
        Out("  row must be 0..2000");
        return;
    }
    if (!player::LooksLikePointer(refs.clientCreature)) {
        Out("  no client creature right now -- are you in a loaded area?");
        return;
    }
    Out(anim::PlayRow(refs.clientCreature, static_cast<uint16_t>(row), 1)
            ? "  played animation row %d"
            : "  row %d could not be played (no such row, or the anim base was "
              "not reachable)",
        row);
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
    } else if (_stricmp(line, "anim") == 0) {
        CmdAnim(refs, arg);
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

// --- in-game mode -------------------------------------------------------------
//
// Driven entirely by the numeric keypad, and it writes nothing to the engine.
//
// The first attempt was a text prompt, which needed the player frozen so that
// typing "spawn" did not also walk the character around. Freezing meant writing
// 0 into the player controller's enabled flag -- a field K2SE had only ever
// read -- and that froze movement and area transitions for the whole session.
// A guess about engine state is not worth a text box.
//
// So the overlay browses instead of typing: the catalogue is bucketed into
// categories, the keypad moves a selection through them, and nothing the game
// owns is touched. The keypad is used because KOTOR binds almost none of it
// (Renan's own camera rebind took Numpad4/6, which is why those two are left
// alone here).
struct Category {
    const char* name;
    const char* needles[4];   // empty first entry = everything
};

const Category kCategories[] = {
    {"all",         {"", nullptr, nullptr, nullptr}},
    {"plants",      {"plant", "tropl", "planter", nullptr}},
    {"containers",  {"footlker", "crate", "cont", "bin"}},
    {"furniture",   {"table", "chair", "bench", "bed"}},
    {"statues",     {"statue", "monument", nullptr, nullptr}},
    {"lights",      {"light", "lamp", "glow", nullptr}},
    {"computers",   {"comp", "console", "panel", "term"}},
    {"doors",       {"door", nullptr, nullptr, nullptr}},
    {"people",      {"soldier", "merc", "civilian", "commoner"}},
    {"recoloured",  {"_a", "_b", "_c", "_d"}},
};
constexpr int kCategoryCount =
    static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0]));
constexpr int kVisibleRows = 10;

int g_category = 1;      // start on plants: the thing most worth adding
int g_selected = 0;
int g_matches = 0;
char g_status[160] = "";
int g_statusFrames = 0;

bool RowInCategory(const CatalogRow& row, int category) {
    const Category& c = kCategories[category];
    if (!c.needles[0] || !*c.needles[0]) return true;
    for (int i = 0; i < 4; ++i) {
        if (!c.needles[i]) break;
        if (ContainsNoCase(Readable(row), c.needles[i]) ||
            ContainsNoCase(row.resref, c.needles[i]) ||
            ContainsNoCase(row.model, c.needles[i]))
            return true;
    }
    return false;
}

// The catalogue is not copied per category; the nth match is found by walking.
int IndexOfMatch(int wanted) {
    int seen = 0;
    for (int i = 0; i < g_rowCount; ++i) {
        if (!RowInCategory(g_rows[i], g_category)) continue;
        if (seen == wanted) return i;
        ++seen;
    }
    return -1;
}

int CountMatches() {
    int n = 0;
    for (int i = 0; i < g_rowCount; ++i)
        if (RowInCategory(g_rows[i], g_category)) ++n;
    return n;
}

void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    _vsnprintf(g_status, sizeof(g_status), fmt, args);
    va_end(args);
    g_status[sizeof(g_status) - 1] = '\0';
    g_statusFrames = 180;
}

void Recount() {
    g_matches = CountMatches();
    if (g_selected >= g_matches) g_selected = g_matches > 0 ? g_matches - 1 : 0;
    if (g_selected < 0) g_selected = 0;
}

void OpenOverlay() {
    if (g_st.visible) return;
    g_st.visible = true;
    input::ResetEdges();
    Recount();
    g_status[0] = '\0';
    g_statusFrames = 0;
    log::Writef("console: overlay opened (%d objects, category %s)", g_rowCount,
                kCategories[g_category].name);
}

void CloseOverlay() {
    if (!g_st.visible) return;
    g_st.visible = false;
    log::Write("console: overlay closed");
}

// Every line is re-posted every frame: AurPostString shows a string only for
// the frame it is posted in, whatever its life argument claims. That was
// established in session S1 and is why the spawner re-posts its banner too.
void DrawOverlay() {
    auto post = reinterpret_cast<AurPostStringFn>(kAurPostString);
    int y = kOverlayTop;

    _snprintf(g_render, sizeof(g_render), "== K2SE  [%s]  %d/%d ==",
              kCategories[g_category].name, g_matches ? g_selected + 1 : 0, g_matches);
    g_render[sizeof(g_render) - 1] = '\0';
    post(g_render, kOverlayX, y, kOverlaySize);
    y += kOverlayStep;

    // Keep the selection in the middle of the window where possible.
    int first = g_selected - kVisibleRows / 2;
    if (first > g_matches - kVisibleRows) first = g_matches - kVisibleRows;
    if (first < 0) first = 0;

    for (int line = 0; line < kVisibleRows; ++line) {
        const int match = first + line;
        if (match >= g_matches) break;
        const int index = IndexOfMatch(match);
        if (index < 0) break;
        const CatalogRow& row = g_rows[index];
        _snprintf(g_render, sizeof(g_render), "%s %-30.30s %-14.14s %s",
                  match == g_selected ? ">" : " ", Readable(row),
                  (row.resref && *row.resref) ? row.resref : "(model only)", row.model);
        g_render[sizeof(g_render) - 1] = '\0';
        post(g_render, kOverlayX, y, kOverlaySize);
        y += kOverlayStep;
    }

    y += kOverlayStep;
    post("Num8/Num2 scroll   Num7/Num1 page   Num9/Num3 category", kOverlayX, y,
         kOverlaySize);
    y += kOverlayStep;
    post("Num5 place   Num0 save   Num. undo   F10 close", kOverlayX, y, kOverlaySize);

    if (g_statusFrames > 0) {
        --g_statusFrames;
        y += kOverlayStep;
        post(g_status, kOverlayX, y, kOverlaySize);
    }
}

void PlaceSelected(const player::Refs& refs) {
    if (g_matches == 0) {
        SetStatus("nothing selected");
        return;
    }
    const int index = IndexOfMatch(g_selected);
    if (index < 0) return;
    const CatalogRow& row = g_rows[index];
    if (!row.resref || !*row.resref) {
        SetStatus("%s is a model with no blueprint -- cannot be placed", Readable(row));
        return;
    }
    float pos[3] = {0, 0, 0};
    float facing = 0.0f;
    if (!PlayerPlacement(refs, pos, &facing)) {
        SetStatus("cannot read your position right now");
        return;
    }
    const int entry = spawner::AddRuntimeEntry(kTypePlaceable, row.resref, pos[0], pos[1],
                                               pos[2], facing);
    if (!entry) {
        SetStatus("could not place it -- is [Spawner] on in the ini?");
        return;
    }
    SetStatus("placing %s (entry %d) -- give it a moment", row.resref, entry);
}

// Returns true when the console consumed the frame's input.
void HandleKeys(const player::Refs& refs) {
    if (input::PollEdgeDown(VK_NUMPAD8)) {
        if (g_selected > 0) --g_selected;
    }
    if (input::PollEdgeDown(VK_NUMPAD2)) {
        if (g_selected + 1 < g_matches) ++g_selected;
    }
    if (input::PollEdgeDown(VK_NUMPAD7)) {
        g_selected -= kVisibleRows;
        if (g_selected < 0) g_selected = 0;
    }
    if (input::PollEdgeDown(VK_NUMPAD1)) {
        g_selected += kVisibleRows;
        if (g_selected >= g_matches) g_selected = g_matches > 0 ? g_matches - 1 : 0;
    }
    if (input::PollEdgeDown(VK_NUMPAD9)) {
        g_category = (g_category + kCategoryCount - 1) % kCategoryCount;
        g_selected = 0;
        Recount();
    }
    if (input::PollEdgeDown(VK_NUMPAD3)) {
        g_category = (g_category + 1) % kCategoryCount;
        g_selected = 0;
        Recount();
    }
    if (input::PollEdgeDown(VK_NUMPAD5)) PlaceSelected(refs);
    if (input::PollEdgeDown(VK_NUMPAD0)) {
        const int written = spawner::Persist();
        SetStatus("%d entr%s saved to k2se_spawns\\%s.ini", written,
                  written == 1 ? "y" : "ies", spawner::ModuleName());
    }
    if (input::PollEdgeDown(VK_DECIMAL)) {
        const int last = spawner::Count();
        SetStatus(spawner::RemoveEntry(last)
                      ? "entry removed (what is already in the area stays until reload)"
                      : "nothing to undo");
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
    // Both of these must happen before the window is ever shown, or the first
    // click on the X takes the game with it.
    SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);
    DisableCloseButton();
    // freopen is avoided on purpose: the log module explains why this DLL keeps
    // clear of the CRT's stdio, and WriteConsoleA/ReadConsoleA need no plumbing.
    g_st.visible = true;
    ShowConsoleWindow(SW_SHOW);

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
    ShowConsoleWindow(SW_HIDE);
    g_st.visible = false;
    log::Write("console: window hidden");
}

void ReadConfig() {
    g_cfg.enabled = config::GetBool("Console", "Enabled", false);
    g_cfg.keyToggle = config::GetKey("Console", "KeyToggle", VK_F10);
    g_cfg.openAtStart = config::GetBool("Console", "OpenAtStart", false);
    // This line went missing when the overlay was rewritten, so InGame in the
    // ini did nothing at all and the console always drew over the game -- which
    // looked exactly like F10 being dead. The mode is logged now, so the next
    // time the setting and the behaviour disagree the log says so.
    g_cfg.inGame = config::GetBool("Console", "InGame", true);
    log::Writef("console: %s toggle %s openAtStart %d mode %s", g_cfg.enabled ? "ON" : "off",
                config::KeyName(g_cfg.keyToggle), g_cfg.openAtStart ? 1 : 0,
                g_cfg.inGame ? "in-game overlay" : "separate window");
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
    log::Writef("console: installed; toggle with %s (%s)", config::KeyName(g_cfg.keyToggle),
                g_cfg.inGame ? "drawn over the game" : "separate window");
    if (g_cfg.openAtStart) {
        if (g_cfg.inGame) {
            OpenOverlay();
        } else {
            OpenWindow();
        }
    }
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
        // Deliberately NOT FreeConsole(): tearing the console down under a
        // running game is the same class of problem as the close button. The
        // handler is removed and the window hidden; the OS reclaims it at exit.
        SetConsoleCtrlHandler(&ConsoleCtrlHandler, FALSE);
        ShowConsoleWindow(SW_HIDE);
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
    if (g_cfg.inGame) {
        if (g_st.visible) {
            CloseOverlay();
        } else {
            OpenOverlay();
        }
        return;
    }
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

    if (g_cfg.inGame) {
        if (input::Pressed(g_cfg.keyToggle)) {
            if (g_st.visible) {
                CloseOverlay();
            } else {
                OpenOverlay();
            }
        }
        if (!g_st.visible) return;
        // Nothing here writes to the engine. The overlay reads the keypad,
        // draws text, and adds spawn entries -- the player keeps full control
        // of the character the whole time.
        HandleKeys(refs);
        DrawOverlay();
        return;
    }

    if (input::Pressed(g_cfg.keyToggle)) Toggle();
    if (g_st.visible) Drain(refs);
}

}  // namespace console
}  // namespace k2se

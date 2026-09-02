#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace k2se {
namespace config {
namespace {

constexpr DWORD kMaxFileBytes = 64 * 1024;
constexpr int kMaxEntries = 256;

struct Entry {
    const char* section;
    const char* key;
    const char* value;
};

char g_buffer[kMaxFileBytes + 1];
Entry g_entries[kMaxEntries];
int g_count = 0;
bool g_present = false;
char g_path[MAX_PATH] = "";

bool IEquals(const char* a, const char* b) {
    if (!a || !b) return false;
    return _stricmp(a, b) == 0;
}

char* Trim(char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
    char* end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
    return s;
}

// Strip an inline comment (`; ...` or `# ...`), but only when the marker is
// preceded by whitespace, so a value like "C:\path#1" survives.
void StripComment(char* s) {
    for (char* p = s; *p; ++p) {
        if ((*p == ';' || *p == '#') && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            return;
        }
    }
}

void Parse() {
    g_count = 0;
    const char* section = "";
    char* line = g_buffer;
    while (line && *line) {
        char* next = strchr(line, '\n');
        if (next) *next++ = '\0';
        StripComment(line);
        char* s = Trim(line);
        if (*s == '[') {
            char* close = strchr(s, ']');
            if (close) {
                *close = '\0';
                section = Trim(s + 1);
            }
        } else if (*s && *s != ';' && *s != '#') {
            char* eq = strchr(s, '=');
            if (eq && g_count < kMaxEntries) {
                *eq = '\0';
                Entry& e = g_entries[g_count++];
                e.section = section;
                e.key = Trim(s);
                e.value = Trim(eq + 1);
            }
        }
        line = next;
    }
}

const Entry* Find(const char* section, const char* key) {
    for (int i = 0; i < g_count; ++i) {
        if (IEquals(g_entries[i].section, section) && IEquals(g_entries[i].key, key))
            return &g_entries[i];
    }
    return nullptr;
}

struct KeyEntry {
    const char* name;
    int vk;
};

// Names are what a user would type, not what winuser.h calls them. Both
// spellings are accepted where people disagree (LCTRL/LCONTROL, ESC/ESCAPE).
const KeyEntry kKeys[] = {
    {"NONE", 0},
    {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"SHIFT", VK_SHIFT},
    {"LCTRL", VK_LCONTROL}, {"LCONTROL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
    {"RCONTROL", VK_RCONTROL}, {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL},
    {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"ALT", VK_MENU},
    {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN},
    {"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE}, {"BACKSPACE", VK_BACK}, {"CAPSLOCK", VK_CAPITAL},
    {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END},
    {"PGUP", VK_PRIOR}, {"PAGEUP", VK_PRIOR}, {"PGDN", VK_NEXT}, {"PAGEDOWN", VK_NEXT},
    {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
    {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6},
    {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
    {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1}, {"NUMPAD2", VK_NUMPAD2},
    {"NUMPAD3", VK_NUMPAD3}, {"NUMPAD4", VK_NUMPAD4}, {"NUMPAD5", VK_NUMPAD5},
    {"NUMPAD6", VK_NUMPAD6}, {"NUMPAD7", VK_NUMPAD7}, {"NUMPAD8", VK_NUMPAD8},
    {"NUMPAD9", VK_NUMPAD9}, {"NUMPADENTER", VK_RETURN},
    {"ADD", VK_ADD}, {"NUMPADPLUS", VK_ADD}, {"SUBTRACT", VK_SUBTRACT}, {"NUMPADMINUS", VK_SUBTRACT},
    {"MULTIPLY", VK_MULTIPLY}, {"NUMPADSTAR", VK_MULTIPLY}, {"DIVIDE", VK_DIVIDE}, {"NUMPADSLASH", VK_DIVIDE},
    {"DECIMAL", VK_DECIMAL}, {"NUMPADDOT", VK_DECIMAL},
    {"MOUSE3", VK_MBUTTON}, {"MOUSE4", VK_XBUTTON1}, {"MOUSE5", VK_XBUTTON2},
    {"GRAVE", VK_OEM_3}, {"TILDE", VK_OEM_3}, {"MINUS", VK_OEM_MINUS}, {"EQUALS", VK_OEM_PLUS},
    {"LBRACKET", VK_OEM_4}, {"RBRACKET", VK_OEM_6}, {"SEMICOLON", VK_OEM_1},
    {"APOSTROPHE", VK_OEM_7}, {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD},
    {"SLASH", VK_OEM_2}, {"BACKSLASH", VK_OEM_5},
};

}  // namespace

bool Load() {
    g_present = false;
    g_count = 0;

    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';

    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%sk2se_movement.ini", exe);
    path[MAX_PATH - 1] = L'\0';
    _snprintf(g_path, sizeof(g_path), "%S", path);
    g_path[sizeof(g_path) - 1] = '\0';

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log::Writef("config: %S not found -- movement features use built-in defaults (all OFF)",
                    path);
        return false;
    }

    DWORD read = 0;
    const BOOL ok = ReadFile(h, g_buffer, kMaxFileBytes, &read, nullptr);
    CloseHandle(h);
    if (!ok) {
        log::Writef("config: ReadFile failed on %S (error %lu)", path, GetLastError());
        return false;
    }
    g_buffer[read] = '\0';

    // A UTF-8 BOM is a common gift from Windows editors; skip it.
    char* start = g_buffer;
    if (read >= 3 && static_cast<unsigned char>(start[0]) == 0xEF &&
        static_cast<unsigned char>(start[1]) == 0xBB && static_cast<unsigned char>(start[2]) == 0xBF)
        memmove(g_buffer, g_buffer + 3, read - 3 + 1);

    Parse();
    g_present = true;
    log::Writef("config: %S loaded, %d entries (%lu bytes)", path, g_count, read);
    return true;
}

bool Present() { return g_present; }

const char* GetString(const char* section, const char* key, const char* def) {
    const Entry* e = Find(section, key);
    return (e && e->value && *e->value) ? e->value : def;
}

int GetInt(const char* section, const char* key, int def) {
    const Entry* e = Find(section, key);
    if (!e || !e->value || !*e->value) return def;
    char* end = nullptr;
    const long v = strtol(e->value, &end, 0);
    return (end && end != e->value) ? static_cast<int>(v) : def;
}

float GetFloat(const char* section, const char* key, float def) {
    const Entry* e = Find(section, key);
    if (!e || !e->value || !*e->value) return def;
    char* end = nullptr;
    const double v = strtod(e->value, &end);
    return (end && end != e->value) ? static_cast<float>(v) : def;
}

bool GetBool(const char* section, const char* key, bool def) {
    const Entry* e = Find(section, key);
    if (!e || !e->value || !*e->value) return def;
    const char* v = e->value;
    if (IEquals(v, "1") || IEquals(v, "true") || IEquals(v, "yes") || IEquals(v, "on")) return true;
    if (IEquals(v, "0") || IEquals(v, "false") || IEquals(v, "no") || IEquals(v, "off")) return false;
    return def;
}

int ParseKeyName(const char* name) {
    if (!name || !*name) return 0;
    for (const KeyEntry& k : kKeys)
        if (IEquals(k.name, name)) return k.vk;
    // Single letter or digit.
    if (strlen(name) == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    // Numeric: 0x41 or 65.
    char* end = nullptr;
    const long v = strtol(name, &end, 0);
    if (end && end != name && *end == '\0' && v > 0 && v < 256) return static_cast<int>(v);
    return 0;
}

const char* KeyName(int vk) {
    // A small ring of buffers: one log line often names several keys at once,
    // and a single static buffer made every one of them print as the last.
    static char ring[8][16];
    static int next = 0;
    for (const KeyEntry& k : kKeys)
        if (k.vk == vk && k.vk != 0) return k.name;
    char* out = ring[next++ & 7];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        out[0] = static_cast<char>(vk);
        out[1] = '\0';
        return out;
    }
    if (vk == 0) return "NONE";
    _snprintf(out, 16, "0x%02X", vk);
    return out;
}

int GetKey(const char* section, const char* key, int defVk) {
    const Entry* e = Find(section, key);
    if (!e || !e->value || !*e->value) return defVk;
    const int vk = ParseKeyName(e->value);
    if (vk == 0 && !IEquals(e->value, "NONE") && !IEquals(e->value, "0")) {
        log::Writef("config: [%s] %s=%s is not a key name I know -- using %s", section, key,
                    e->value, KeyName(defVk));
        return defVk;
    }
    return vk;
}

void LogAll() {
    if (!g_present) return;
    log::Writef("config: contents of %s", g_path);
    for (int i = 0; i < g_count; ++i)
        log::Writef("  [%s] %s = %s", g_entries[i].section, g_entries[i].key, g_entries[i].value);
}

}  // namespace config
}  // namespace k2se

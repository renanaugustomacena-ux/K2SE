#include "input.h"

#include <windows.h>

#include "log.h"

namespace k2se {
namespace input {
namespace {

using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetForegroundWindowFn = HWND(WINAPI*)();
using GetWindowThreadProcessIdFn = DWORD(WINAPI*)(HWND, LPDWORD);

GetAsyncKeyStateFn g_getAsyncKeyState = nullptr;
GetForegroundWindowFn g_getForegroundWindow = nullptr;
GetWindowThreadProcessIdFn g_getWindowThreadProcessId = nullptr;

int g_keys[kMaxKeys];
bool g_cur[kMaxKeys];
bool g_prev[kMaxKeys];
int g_count = 0;
bool g_focused = false;
DWORD g_pid = 0;

int Slot(int vk) {
    for (int i = 0; i < g_count; ++i)
        if (g_keys[i] == vk) return i;
    return -1;
}

}  // namespace

bool Init() {
    g_pid = GetCurrentProcessId();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        // The game always loads user32 long before any script runs, but be
        // explicit: LoadLibrary here is fine because Init() runs after DllMain.
        user32 = LoadLibraryW(L"user32.dll");
    }
    if (!user32) {
        log::Write("input: user32.dll unavailable -- keys will never register");
        return false;
    }
    g_getAsyncKeyState =
        reinterpret_cast<GetAsyncKeyStateFn>(GetProcAddress(user32, "GetAsyncKeyState"));
    g_getForegroundWindow =
        reinterpret_cast<GetForegroundWindowFn>(GetProcAddress(user32, "GetForegroundWindow"));
    g_getWindowThreadProcessId = reinterpret_cast<GetWindowThreadProcessIdFn>(
        GetProcAddress(user32, "GetWindowThreadProcessId"));
    const bool ok = g_getAsyncKeyState && g_getForegroundWindow && g_getWindowThreadProcessId;
    log::Writef("input: user32 resolved at runtime -> %s", ok ? "OK" : "INCOMPLETE");
    return ok;
}

// Edge state for the printable keys, kept apart from the tracked-key table so
// opening the console cannot exhaust kMaxKeys.
bool g_typedPrev[256];

int PollTypedChar() {
    if (!g_getAsyncKeyState || !g_focused) return 0;

    const bool shift = (g_getAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    int typed = 0;
    for (int vk = 0; vk < 256; ++vk) {
        const bool printable = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') ||
                               vk == VK_SPACE || vk == VK_BACK || vk == VK_RETURN ||
                               vk == VK_OEM_MINUS || vk == VK_OEM_PERIOD ||
                               vk == VK_OEM_1 || vk == VK_OEM_2;
        if (!printable) continue;
        const bool down = (g_getAsyncKeyState(vk) & 0x8000) != 0;
        const bool was = g_typedPrev[vk];
        g_typedPrev[vk] = down;
        if (!down || was || typed) continue;   // only the first new press per frame

        if (vk == VK_BACK) typed = 8;
        else if (vk == VK_RETURN) typed = 13;
        else if (vk == VK_SPACE) typed = ' ';
        else if (vk == VK_OEM_MINUS) typed = shift ? '_' : '-';
        else if (vk == VK_OEM_PERIOD) typed = '.';
        else if (vk == VK_OEM_1) typed = shift ? ':' : ';';
        else if (vk == VK_OEM_2) typed = '/';
        else if (vk >= '0' && vk <= '9') typed = vk;
        else typed = shift ? vk : (vk - 'A' + 'a');   // resrefs are lower case
    }
    return typed;
}

void Track(int vk) {
    if (vk <= 0 || vk >= 256) return;
    if (Slot(vk) >= 0 || g_count >= kMaxKeys) return;
    g_keys[g_count] = vk;
    g_cur[g_count] = false;
    g_prev[g_count] = false;
    ++g_count;
}

void BeginFrame() {
    // Focus first: a key held while alt-tabbed must not act on the game.
    g_focused = false;
    if (g_getForegroundWindow && g_getWindowThreadProcessId) {
        HWND fg = g_getForegroundWindow();
        DWORD pid = 0;
        if (fg) g_getWindowThreadProcessId(fg, &pid);
        g_focused = (pid == g_pid);
    }
    for (int i = 0; i < g_count; ++i) {
        g_prev[i] = g_cur[i];
        bool down = false;
        if (g_focused && g_getAsyncKeyState) down = (g_getAsyncKeyState(g_keys[i]) & 0x8000) != 0;
        g_cur[i] = down;
    }
}

bool Focused() { return g_focused; }

bool IsDown(int vk) {
    const int i = Slot(vk);
    return i >= 0 && g_cur[i];
}

bool Pressed(int vk) {
    const int i = Slot(vk);
    return i >= 0 && g_cur[i] && !g_prev[i];
}

bool Released(int vk) {
    const int i = Slot(vk);
    return i >= 0 && !g_cur[i] && g_prev[i];
}

}  // namespace input
}  // namespace k2se

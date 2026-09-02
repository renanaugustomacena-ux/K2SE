#pragma once
#include <cstdint>

// ============================================================================
// Keyboard input for the movement features.
//
// The engine reads the keyboard through DirectInput and its own keymap; K2SE
// does not hook any of that in this version. It polls GetAsyncKeyState for the
// handful of keys the ini names, once per frame, and only while the game window
// is in the foreground. Because the polling happens inside the player
// controller's own update (see movement.cpp), a key is never seen while the
// controller is disabled -- dialogues, menus, cutscenes and minigames get no
// sprint, crouch, jump or roll for free.
//
// USER32 is resolved at runtime: the shipped DLL must keep importing only
// KERNEL32 (tools/check_dll.py enforces that), and user32.dll is always loaded
// in the game process anyway.
// ============================================================================

namespace k2se {
namespace input {

// Resolves the user32 entry points. Safe from DllMain (GetModuleHandle +
// GetProcAddress only). Returns false if user32 is somehow unavailable, in
// which case every key reads as "up".
bool Init();

// Registers a key to track. At most kMaxKeys distinct keys; 0 is ignored.
constexpr int kMaxKeys = 16;
void Track(int vk);

// Samples every tracked key. Call exactly once per game frame; the movement
// module guards this with its own frame counter.
void BeginFrame();

bool Focused();             // the game window is the foreground window
bool IsDown(int vk);        // held right now (as of BeginFrame)
bool Pressed(int vk);       // went down this frame
bool Released(int vk);      // went up this frame

}  // namespace input
}  // namespace k2se

#pragma once
#include <cstdint>

// ============================================================================
// k2se_movement.ini -- the movement features' configuration.
//
// Lives next to swkotor2.exe (same folder as the DLL), never in %LOCALAPPDATA%:
// a mod's settings belong with the mod, and the user must be able to delete the
// whole thing by deleting two files.
//
// Deliberately tiny: sections, `key=value`, `;`/`#` comments, case-insensitive
// lookups. No CRT stream I/O (see log.h for why), no allocation after Load():
// the file is read once into a static buffer and every value is a pointer into
// it. Missing file == every getter returns its default, so the DLL keeps
// working exactly like K2SE 0.1 when the ini is absent.
// ============================================================================

namespace k2se {
namespace config {

// Reads <exe dir>\k2se_movement.ini. Returns true if the file was found and
// parsed. Safe to call from DllMain (CreateFile/ReadFile only).
bool Load();
bool Present();

int GetInt(const char* section, const char* key, int def);
float GetFloat(const char* section, const char* key, float def);
bool GetBool(const char* section, const char* key, bool def);
const char* GetString(const char* section, const char* key, const char* def);

// A key binding: a Win32 virtual-key code parsed from a name (LSHIFT, SPACE, C,
// F9, NUMPAD0, MOUSE4, ...) or a number (0x41 / 65). NONE / 0 = unbound.
int GetKey(const char* section, const char* key, int defVk);
int ParseKeyName(const char* name);   // 0 if unknown
const char* KeyName(int vk);          // "LSHIFT", "C", "0x41" fallback

// Writes every parsed entry to the log, once, so a bug report carries the
// configuration that produced it.
void LogAll();

}  // namespace config
}  // namespace k2se

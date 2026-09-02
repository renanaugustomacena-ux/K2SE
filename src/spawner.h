#pragma once
#include <cstdint>

// ============================================================================
// Spawner (k2-texture-pack / "object swapper", step 1): populate areas with
// extra placeables and creatures from data files, without touching module
// files or saves.
//
// How it works: K2SE does not create objects itself. It runs the compiled
// script `k2se_spawn.ncs` (override/) through CVirtualMachine::RunScript
// (0x006FD8D0, (CExoString* name, uint32 objectId, int) ret 0xC) a moment after
// every area setup (the camera-style load in camera.cpp is the signal) and then
// every few seconds of gameplay. The script asks K2SE for the entries of the
// current module (routines 898..907), checks which ones already exist in the
// area (LocalBoolean 150 + LocalNumber 25 stamped on every object it created,
// both saved with the area) and calls the engine's own CreateObject for the
// missing ones -- so walkmeshes, collisions and saving work as for any object.
//
// Data: <game>\k2se_spawns\<MODULE>.ini, one section per entry:
//   [spawn1]
//   Type=placeable        ; or creature
//   Template=plc_crate    ; a UTP/UTC resref (386 placeables ship in templates.bif)
//   X=-7.7  Y=19.9  Z=9.67
//   Facing=90             ; degrees
//   Area=101PERa          ; optional: only in this area tag
// F10 in game appends the player's position as a ready-made [spawnN] block to
// k2se_spawns\_captured.txt, so points are authored by walking there.
// ============================================================================
namespace k2se {
namespace spawner {

bool Install();
void Remove();
int Status();

// movement.cpp, once per gameplay frame, with the player's server creature.
void OnGameplayFrame(void* serverCreature, bool refsValid, float dt);
// camera.cpp, whenever the engine (re)loads a camera style = an area was set up.
void OnAreaSetup();

// Script-facing (routines 898..907).
int Begin(const char* module, const char* areaTag);   // loads the module file, returns entry count
bool MarkPresent(int index);                          // 1-based
bool Needed(int index);
int Type(int index);                                  // OBJECT_TYPE_PLACEABLE 64 / CREATURE 1
const char* Template(int index);
float X(int index);
float Y(int index);
float Z(int index);
float Facing(int index);
bool Report(int index, bool ok);

}  // namespace spawner
}  // namespace k2se

#pragma once
#include <cstdint>

// ============================================================================
// A real console window for placing objects (k2-object-console).
//
// F10 used to append the player's position to _captured.txt and that was the
// whole feature: you could record a point, but not see what could stand on it,
// and not put anything there without editing an ini and restarting.
//
// This opens an actual console window instead. It browses the 1348-row
// catalogue built offline by tools/gen_catalog.py -- every placeable and door
// blueprint in the BIFs and in all 246 module archives, with names resolved
// through dialog.tlk -- and places what you pick at the player's feet.
//
// Threading is the whole design problem. A console needs a blocking read, and
// the game thread cannot block, so the reader lives on its own thread and does
// exactly one thing: push the typed line into a queue. Nothing on that thread
// ever touches a game object. OnGameplayFrame drains the queue on the game
// thread, where touching game objects is legal. The queue is the only shared
// state and it is guarded by a critical section.
//
// The game must run windowed or borderless for the console to be usable; in
// exclusive fullscreen you cannot alt-tab to it without the game minimising.
// ============================================================================
namespace k2se {
namespace player {
struct Refs;
}

namespace console {

bool Install();
void Remove();
int Status();   // bit 0 installed, bit 1 window open, bit 2 catalogue loaded

// movement.cpp, once per gameplay frame. Drains the command queue.
void OnGameplayFrame(const player::Refs& refs, float dt);

// Opened and closed with the toggle key (F10 by default).
void Toggle();
bool Visible();

}  // namespace console
}  // namespace k2se

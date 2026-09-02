#include "anim.h"

#include <windows.h>

#include "log.h"
#include "player.h"

namespace k2se {
namespace anim {
namespace {

constexpr uint32_t kGetAnimBase = 0x007ED830;          // CSWCCreature::GetAnimBase, thiscall, no args
constexpr uint32_t kVtableSlotMapCode = 0xE0 / 4;      // uint16 (uint16 code), ret 4
constexpr uint32_t kVtableSlotPlay = 0x44 / 4;         // int (uint16 row, int flag), ret 8
constexpr uint32_t kExpectedAnimBaseVtable = 0x009A454C;  // CSWCAnimBase (RTTI), 62 slots

using GetAnimBaseFn = void*(__thiscall*)(void* creature);
using MapCodeFn = uint16_t(__thiscall*)(void* base, uint16_t code);
using PlayFn = int(__thiscall*)(void* base, uint16_t row, int flag);

bool g_vtableWarned = false;

void* AnimBaseImpl(void* clientCreature) {
    if (!player::LooksLikePointer(clientCreature)) return nullptr;
    void* base = nullptr;
    __try {
        base = reinterpret_cast<GetAnimBaseFn>(kGetAnimBase)(clientCreature);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("anim: GetAnimBase faulted");
        return nullptr;
    }
    if (!player::LooksLikePointer(base)) return nullptr;

    // The base may be a subclass (CSWCAnimBaseHead, ...); its vtable differs.
    // Log once when it is not the plain CSWCAnimBase we studied, but proceed:
    // the slots are inherited.
    uint32_t vt = 0;
    __try {
        vt = *reinterpret_cast<volatile uint32_t*>(base);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (vt != kExpectedAnimBaseVtable && !g_vtableWarned) {
        g_vtableWarned = true;
        log::Writef("anim: anim base vtable 0x%08X (expected 0x%08X for CSWCAnimBase) -- "
                    "subclass, slots assumed inherited", vt, kExpectedAnimBaseVtable);
    }
    return base;
}

template <typename Fn>
Fn Slot(void* base, uint32_t index) {
    uint32_t* vt = *reinterpret_cast<uint32_t**>(base);
    return reinterpret_cast<Fn>(vt[index]);
}

}  // namespace

// Public: the creature's anim base (CSWCCreature::GetAnimBase), SEH-guarded,
// null when the pointer or the call is not trustworthy. Used by the S1 position scan.
void* AnimBase(void* clientCreature) { return AnimBaseImpl(clientCreature); }

int MapCode(void* clientCreature, uint16_t code) {
    void* base = AnimBaseImpl(clientCreature);
    if (!base) return -1;
    __try {
        return Slot<MapCodeFn>(base, kVtableSlotMapCode)(base, code);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("anim: MapCode faulted");
        return -1;
    }
}

bool PlayRow(void* clientCreature, uint16_t row, int flag) {
    void* base = AnimBaseImpl(clientCreature);
    if (!base) return false;
    __try {
        Slot<PlayFn>(base, kVtableSlotPlay)(base, row, flag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Writef("anim: Play(row %u) faulted", row);
        return false;
    }
}

bool PlayCode(void* clientCreature, uint16_t code) {
    const int row = MapCode(clientCreature, code);
    if (row < 0 || row == 0xFFFF) {
        log::Writef("anim: code 0x%04X has no row in the engine's mapper", code);
        return false;
    }
    return PlayRow(clientCreature, static_cast<uint16_t>(row), 1);
}

}  // namespace anim
}  // namespace k2se

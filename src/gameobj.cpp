#include "gameobj.h"

#include <windows.h>

#include "log.h"
#include "offsets.h"

namespace k2se {
namespace gameobj {
namespace {

using GetCreatureByIdFn = void*(__thiscall*)(void* server, uint32_t objectId);
using GetSkillRankFn = uint8_t(__thiscall*)(void* stats, uint8_t skill, void* unused,
                                            int baseOnly);
using HasFeatFn = uint8_t(__thiscall*)(void* stats, uint16_t feat);
using HasSpellFn = uint8_t(__thiscall*)(void* stats, uint8_t cls, uint32_t spell, int flag);

// The game is a 32-bit process; with the 4GB/LAA patch its heap can reach up to
// 0x7FFFFFFF, without it 0x7FFFFFFF is still the ceiling for user space. Anything
// below the image base is not an object -- most importantly it catches a small
// integer that has been mistaken for a pointer, which is what a wrong struct
// offset usually produces.
constexpr uintptr_t kMinPlausible = 0x00010000;
constexpr uintptr_t kMaxPlausible = 0x7FFFFFFF;

// Every read through an imported struct offset goes through these. A wrong
// offset should cost us a logged refusal, not the player's session.
bool SafeReadPtr(const void* at, void** out) {
    __try {
        *out = *reinterpret_cast<void* const*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadByte(const void* at, uint8_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint8_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Calling into the engine with a pointer we are not certain about. The call
// itself is what might fault, so it is the call that is guarded.
bool SafeCallSkillRank(void* stats, uint8_t skill, int* out) {
    __try {
        auto fn = reinterpret_cast<GetSkillRankFn>(off::kStatsGetSkillRank);
        // (skill, NULL, baseOnly=1): the base rank, without item or effect
        // modifiers. Argument count confirmed by the epilogue's `ret 12`.
        *out = static_cast<int>(fn(stats, skill, nullptr, 1));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCallHasFeat(void* stats, uint16_t feat, int* out) {
    __try {
        auto fn = reinterpret_cast<HasFeatFn>(off::kStatsHasFeat);
        *out = fn(stats, feat) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCallHasSpell(void* stats, uint32_t spell, int* out) {
    __try {
        auto fn = reinterpret_cast<HasSpellFn>(off::kStatsHasSpell);
        *out = fn(stats, 0, spell, 0) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCallCreatureById(void* server, uint32_t objectId, void** out) {
    __try {
        auto fn = reinterpret_cast<GetCreatureByIdFn>(off::kGetCreatureByGameObjectID);
        *out = fn(server, objectId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool LooksLikePointer(const void* p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= kMinPlausible && v <= kMaxPlausible && (v & 3) == 0;
}

void* ServerExoApp() {
    void* appManager = nullptr;
    if (!SafeReadPtr(reinterpret_cast<const void*>(off::kAppManagerGlobal), &appManager))
        return nullptr;
    if (!LooksLikePointer(appManager)) return nullptr;

    void* server = nullptr;
    if (!SafeReadPtr(static_cast<const char*>(appManager) + off::kAppManagerOffServer,
                     &server))
        return nullptr;
    return LooksLikePointer(server) ? server : nullptr;
}

void* CreatureFromObjectId(uint32_t objectId) {
    if (objectId == off::kObjectInvalid) return nullptr;

    void* server = ServerExoApp();
    if (!server) return nullptr;

    void* creature = nullptr;
    if (!SafeCallCreatureById(server, objectId, &creature)) {
        log::Write("gameobj: GetCreatureByGameObjectID faulted");
        return nullptr;
    }
    return LooksLikePointer(creature) ? creature : nullptr;
}

void* CreatureStats(void* creature) {
    if (!LooksLikePointer(creature)) return nullptr;
    void* stats = nullptr;
    if (!SafeReadPtr(static_cast<const char*>(creature) + off::kCreatureOffStats, &stats))
        return nullptr;
    return LooksLikePointer(stats) ? stats : nullptr;
}

bool AbilityBase(void* stats, int ability, int* out) {
    if (!LooksLikePointer(stats) || !out) return false;
    if (ability < 0 || ability >= off::kAbilityCount) return false;

    const char* at = static_cast<const char*>(stats) + off::kStatsOffAbilityBase +
                     ability * off::kStatsAbilityStride;
    uint8_t value = 0;
    if (!SafeReadByte(at, &value)) return false;
    *out = value;
    return true;
}

bool SkillRankBase(void* stats, int skill, int* out) {
    if (!LooksLikePointer(stats) || !out) return false;
    if (skill < 0 || skill > 0xFF) return false;
    return SafeCallSkillRank(stats, static_cast<uint8_t>(skill), out);
}

bool HasFeat(void* stats, int feat, int* out) {
    if (!LooksLikePointer(stats) || !out) return false;
    if (feat < 0 || feat > 0xFFFF) return false;
    return SafeCallHasFeat(stats, static_cast<uint16_t>(feat), out);
}

bool HasSpell(void* stats, int spell, int* out) {
    if (!LooksLikePointer(stats) || !out) return false;
    if (spell < 0) return false;
    return SafeCallHasSpell(stats, static_cast<uint32_t>(spell), out);
}

}  // namespace gameobj
}  // namespace k2se

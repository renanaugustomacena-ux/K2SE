#include "npcvariety.h"

#include <windows.h>

#include <cstdio>

#include "callsite.h"
#include "config.h"
#include "gameobj.h"
#include "log.h"
#include "player.h"

namespace k2se {
namespace npcvariety {
namespace {

constexpr uint32_t kLoadAppearance = 0x0057FCE0;   // CSWSCreature::LoadAppearance, thiscall (int), ret 4
constexpr uint32_t kSites[] = {0x00576AAF, 0x00585241, 0x0058536E, 0x0065A9DD};
constexpr uint32_t kOffAppearanceRow = 0x1184;     // uint16
constexpr uint32_t kOffObjectId = 0x4;             // CGameObject id, validated per creature

using LoadAppearanceFn = int(__fastcall*)(void* self, void* edx, int a);

// Interchangeable rows of the stock appearance.2da (TSL 1.0b / TSLRCM keeps them).
struct Family {
    const char* name;
    uint16_t rows[16];
    int count;
};
const Family kFamilies[] = {
    {"N_CommF", {14, 185, 186, 187, 188, 189, 280, 339, 340, 341, 342, 343, 344}, 13},
    {"N_CommM", {17, 190, 191, 192, 193, 194, 345, 346, 347, 348, 349, 350}, 12},
    {"N_CzerkaOff", {18, 195, 196, 197, 198, 199}, 6},
    {"N_RepSold", {38, 283, 284, 285, 286, 355}, 6},
    {"N_RepOff", {37, 287, 288, 289, 290}, 5},
    {"N_RepSold_F", {356, 357, 358, 359}, 4},
    {"N_RepOff_F", {360, 361, 362}, 3},
};
constexpr int kFamilyCount = static_cast<int>(sizeof(kFamilies) / sizeof(kFamilies[0]));

bool g_enabled = false;
int g_chance = 100;
bool g_installed = false;
uint32_t g_loads = 0;
uint32_t g_swaps = 0;
uint32_t g_skippedNoId = 0;
int g_logged = 0;
LoadAppearanceFn g_orig = reinterpret_cast<LoadAppearanceFn>(kLoadAppearance);

const Family* FamilyOf(uint16_t row) {
    for (int f = 0; f < kFamilyCount; ++f)
        for (int i = 0; i < kFamilies[f].count; ++i)
            if (kFamilies[f].rows[i] == row) return &kFamilies[f];
    return nullptr;
}

uint32_t Mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

bool ReadRow(void* self, uint16_t* out) {
    __try {
        *out = *reinterpret_cast<volatile uint16_t*>(static_cast<char*>(self) + kOffAppearanceRow);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool WriteRow(void* self, uint16_t v) {
    __try {
        *reinterpret_cast<volatile uint16_t*>(static_cast<char*>(self) + kOffAppearanceRow) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ReadId(void* self, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<volatile uint32_t*>(static_cast<char*>(self) + kOffObjectId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int __fastcall HookLoadAppearance(void* self, void* edx, int a) {
    ++g_loads;
    uint16_t row = 0;
    if (g_enabled && player::LooksLikePointer(self) && ReadRow(self, &row)) {
        const Family* fam = FamilyOf(row);
        if (fam) {
            uint32_t id = 0;
            // The object id must round-trip through the engine's own lookup before
            // it is trusted as the seed; an unverifiable creature keeps its look.
            if (ReadId(self, &id) && id != 0 && id != 0x7F000000 && gameobj::CreatureFromObjectId(id) == self) {
                const uint32_t h = Mix(id * 2654435761u + 0x9E3779B9u);
                if (static_cast<int>(h % 100u) < g_chance) {
                    const uint16_t chosen = fam->rows[Mix(h ^ 0x5bd1e995u) % static_cast<uint32_t>(fam->count)];
                    if (chosen != row && WriteRow(self, chosen)) {
                        ++g_swaps;
                        if (g_logged < 30) {
                            ++g_logged;
                            log::Writef("npcvariety: creature id 0x%08X row %u -> %u (%s)", id, row, chosen, fam->name);
                        }
                    }
                }
            } else {
                ++g_skippedNoId;
            }
        }
    }
    return g_orig(self, edx, a);
}

}  // namespace

bool Install() {
    if (g_installed) return true;
    if (!config::Present()) return false;
    g_enabled = config::GetBool("NpcVariety", "Enabled", false);
    g_chance = config::GetInt("NpcVariety", "Chance", 100);
    if (g_chance < 0) g_chance = 0;
    if (g_chance > 100) g_chance = 100;
    log::Writef("npcvariety: %s chance %d%% families %d", g_enabled ? "ON" : "off", g_chance, kFamilyCount);
    if (!g_enabled) return false;
    int redirected = 0;
    for (int i = 0; i < 4; ++i) {
        static char names[4][40];
        _snprintf(names[i], sizeof(names[i]), "LoadAppearance (site %d/4)", i + 1);
        names[i][sizeof(names[i]) - 1] = '\0';
        if (callsite::Redirect(names[i], kSites[i], kLoadAppearance,
                               reinterpret_cast<const void*>(&HookLoadAppearance)))
            ++redirected;
    }
    if (redirected == 0) {
        log::Write("npcvariety: no LoadAppearance site redirected -> off");
        g_enabled = false;
        return false;
    }
    g_installed = true;
    log::Writef("npcvariety: installed (%d/4 sites)", redirected);
    return true;
}

void Remove() {
    if (!g_installed) return;
    // Call sites are restored by callsite::RestoreAll (movement::Remove).
    log::Writef("npcvariety: removed after %u appearance loads, %u swaps, %u without a verifiable id", g_loads,
                g_swaps, g_skippedNoId);
    g_installed = false;
}

int Status() {
    int s = g_installed ? 1 : 0;
    s |= static_cast<int>(g_swaps & 0xFFFF) << 8;
    return s;
}

}  // namespace npcvariety
}  // namespace k2se

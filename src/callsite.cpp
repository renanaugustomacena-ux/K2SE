#include "callsite.h"

#include <windows.h>

#include "log.h"

namespace k2se {
namespace callsite {
namespace {

constexpr int kMaxSites = 64;   // 32 filled up on 2026-09-02 (78-site SetPosition diagnostic) and silently starved camera.cpp

struct Site {
    uint32_t va;
    int32_t originalRel;
    const char* name;
};

Site g_sites[kMaxSites];
int g_count = 0;

bool SafeReadByte(uint32_t va, uint8_t* out) {
    __try {
        *out = *reinterpret_cast<volatile uint8_t*>(va);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeReadRel(uint32_t va, int32_t* out) {
    __try {
        *out = *reinterpret_cast<volatile int32_t*>(va);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteRel(uint32_t site, int32_t rel) {
    auto* p = reinterpret_cast<int32_t*>(site + 1);
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(int32_t), PAGE_EXECUTE_READWRITE, &old)) {
        log::Writef("callsite: VirtualProtect failed at 0x%08X (error %lu)", site, GetLastError());
        return false;
    }
    *p = rel;
    DWORD restored = 0;
    VirtualProtect(p, sizeof(int32_t), old, &restored);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(int32_t));
    return true;
}

}  // namespace

bool Redirect(const char* name, uint32_t site, uint32_t expected, const void* hook) {
    if (g_count >= kMaxSites) {
        log::Writef("callsite: table full, refusing %s at 0x%08X", name, site);
        return false;
    }

    uint8_t opcode = 0;
    int32_t rel = 0;
    if (!SafeReadByte(site, &opcode) || !SafeReadRel(site + 1, &rel)) {
        log::Writef("callsite: %s at 0x%08X UNREADABLE -- refused", name, site);
        return false;
    }
    if (opcode != 0xE8) {
        log::Writef("callsite: %s at 0x%08X is not a call (opcode 0x%02X) -- refused", name, site,
                    opcode);
        return false;
    }
    const uint32_t target = site + 5 + static_cast<uint32_t>(rel);
    if (target != expected) {
        log::Writef("callsite: %s at 0x%08X calls 0x%08X, expected 0x%08X -- refused "
                    "(another patch owns this site?)",
                    name, site, target, expected);
        return false;
    }

    const int32_t newRel =
        static_cast<int32_t>(reinterpret_cast<uint32_t>(hook) - (site + 5));
    if (!WriteRel(site, newRel)) return false;

    g_sites[g_count].va = site;
    g_sites[g_count].originalRel = rel;
    g_sites[g_count].name = name;
    ++g_count;
    log::Writef("callsite: %s at 0x%08X  E8 -> 0x%08X redirected to 0x%08X", name, site, expected,
                reinterpret_cast<uint32_t>(hook));
    return true;
}

void RestoreAll() {
    for (int i = g_count - 1; i >= 0; --i) {
        if (WriteRel(g_sites[i].va, g_sites[i].originalRel))
            log::Writef("callsite: %s at 0x%08X restored", g_sites[i].name, g_sites[i].va);
    }
    g_count = 0;
}

int Count() { return g_count; }

}  // namespace callsite
}  // namespace k2se

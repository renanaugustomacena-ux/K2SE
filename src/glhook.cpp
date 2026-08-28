#include "glhook.h"

#include <windows.h>

#include <cstring>

#include "log.h"
#include "offsets.h"

namespace k2se {
namespace glhook {
namespace {

// --- GL constants, spelled out so no GL headers are needed -------------------
constexpr uint32_t GL_FRAGMENT_PROGRAM_ARB = 0x8804;
constexpr uint32_t GL_LINEAR = 0x2601;
constexpr uint32_t GL_FOG_MODE = 0x0B65;
constexpr uint32_t GL_FOG_START = 0x0B63;
constexpr uint32_t GL_FOG_END = 0x0B64;
constexpr uint32_t GL_FOG_COLOR = 0x0B66;

using ProcFn = int(__stdcall*)();
using WglGetProcAddressFn = ProcFn(__stdcall*)(const char* name);
using ProgramStringFn = void(__stdcall*)(uint32_t target, uint32_t format, int len,
                                         const void* str);
using FogfFn = void(__stdcall*)(uint32_t pname, float param);
using FogfvFn = void(__stdcall*)(uint32_t pname, const float* params);
using FogiFn = void(__stdcall*)(uint32_t pname, int param);

// The import slots. Verified by NAME against the PE import directory rather than
// trusted as addresses -- tools/verify_offsets.py re-checks all of them.
constexpr uint32_t kIatWglGetProcAddress = 0x0098632C;

struct Original {
    WglGetProcAddressFn wglGetProcAddress = nullptr;
    ProgramStringFn programString = nullptr;
    FogfFn fogf = nullptr;
    FogfvFn fogfv = nullptr;
    FogiFn fogi = nullptr;
};

Original g_orig;
int g_status = kDisabled;

struct FogOverride {
    bool enabled = false;
    bool haveRange = false;
    bool haveColor = false;
    float start = 0.0f;
    float end = 0.0f;
    float color[3] = {0.0f, 0.0f, 0.0f};
};
FogOverride g_fog;

int g_patchedPrograms = 0;
bool g_loggedFirstProgram = false;

// Every patched slot, so removal is exact rather than reconstructed.
struct PatchedSlot {
    uint32_t va;
    void* original;
};
PatchedSlot g_patched[8];
int g_patchedCount = 0;

// --- import table patching ---------------------------------------------------
bool WriteSlot(uint32_t va, void* value, void** previous) {
    auto* slot = reinterpret_cast<void**>(va);
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        log::Writef("glhook: REFUSED TO WRITE 0x%08X, VirtualProtect failed (%lu)", va,
                    GetLastError());
        return false;
    }
    if (previous) *previous = *slot;
    *slot = value;
    DWORD restored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &restored);
    return true;
}

bool PatchSlot(const char* what, uint32_t va, void* replacement, void** original) {
    if (!WriteSlot(va, replacement, original)) return false;
    if (g_patchedCount < static_cast<int>(sizeof(g_patched) / sizeof(g_patched[0]))) {
        g_patched[g_patchedCount].va = va;
        g_patched[g_patchedCount].original = *original;
        ++g_patchedCount;
    }
    log::Writef("glhook: %s [0x%08X] 0x%08X -> 0x%08X", what, va,
                reinterpret_cast<uint32_t>(*original),
                reinterpret_cast<uint32_t>(replacement));
    return true;
}

// --- the fragment-program rewrite -------------------------------------------
// GL is context-affine and the game renders on one thread, so a single scratch
// buffer is sufficient. Sized well past the largest program in this build (the
// biggest is under 1.5 KB).
constexpr int kScratchSize = 64 * 1024;
char g_scratch[kScratchSize];

constexpr char kFragmentHeader[] = "!!ARBfp1.0";
constexpr char kFogOption[] = "OPTION ARB_fog_linear;\n";

// Substring search over a buffer that is not necessarily NUL-terminated: the
// length glProgramStringARB is given is authoritative, not any terminator.
bool ContainsFogOption(const char* hay, int hayLen) {
    static const char kNeedle[] = "ARB_fog_";
    const int needleLen = static_cast<int>(sizeof(kNeedle) - 1);
    if (hayLen < needleLen) return false;
    for (int i = 0; i + needleLen <= hayLen; ++i)
        if (memcmp(hay + i, kNeedle, needleLen) == 0) return true;
    return false;
}

// Returns true and fills g_scratch when the source was rewritten.
bool InjectFogOption(const char* src, int len, int* outLen) {
    if (!src || len <= 0) return false;

    const int headerLen = static_cast<int>(sizeof(kFragmentHeader) - 1);
    if (len < headerLen) return false;
    if (memcmp(src, kFragmentHeader, headerLen) != 0) return false;

    // Already fog-aware? Either the engine changed or another mod (3C-FD, the
    // community shader fixes) got here first. Do not stack a second OPTION on
    // top of theirs -- DESIGN.md M5 is explicit that K2SE must not fight an
    // existing shader fix.
    if (ContainsFogOption(src, len)) return false;

    const int optionLen = static_cast<int>(sizeof(kFogOption) - 1);
    if (len + optionLen + 1 > kScratchSize) {
        log::Writef("glhook: fragment program too large to rewrite (%d bytes)", len);
        return false;
    }

    // Insert immediately after the header line so the OPTION precedes every
    // instruction, which is where the ARB grammar requires options to appear.
    int split = headerLen;
    while (split < len && src[split] != '\n') ++split;
    if (split < len) ++split;  // keep the newline with the header

    memcpy(g_scratch, src, split);
    memcpy(g_scratch + split, kFogOption, optionLen);
    memcpy(g_scratch + split + optionLen, src + split, len - split);
    *outLen = len + optionLen;
    return true;
}

void __stdcall Hook_glProgramStringARB(uint32_t target, uint32_t format, int len,
                                       const void* str) {
    if (target == GL_FRAGMENT_PROGRAM_ARB) {
        int newLen = 0;
        if (InjectFogOption(static_cast<const char*>(str), len, &newLen)) {
            ++g_patchedPrograms;
            g_status |= kShaderPatched;
            if (!g_loggedFirstProgram) {
                g_loggedFirstProgram = true;
                log::Writef("glhook: first fragment program rewritten "
                            "(%d -> %d bytes, OPTION ARB_fog_linear inserted)",
                            len, newLen);
            }
            g_orig.programString(target, format, newLen, g_scratch);
            return;
        }
    }
    g_orig.programString(target, format, len, str);
}

// --- fog parameter interception ----------------------------------------------
void __stdcall Hook_glFogf(uint32_t pname, float param) {
    if (g_fog.enabled) {
        if (g_fog.haveRange && pname == GL_FOG_START) return g_orig.fogf(pname, g_fog.start);
        if (g_fog.haveRange && pname == GL_FOG_END) return g_orig.fogf(pname, g_fog.end);
    }
    g_orig.fogf(pname, param);
}

void __stdcall Hook_glFogfv(uint32_t pname, const float* params) {
    if (g_fog.enabled && g_fog.haveColor && pname == GL_FOG_COLOR) {
        const float rgba[4] = {g_fog.color[0], g_fog.color[1], g_fog.color[2], 1.0f};
        return g_orig.fogfv(pname, rgba);
    }
    g_orig.fogfv(pname, params);
}

void __stdcall Hook_glFogi(uint32_t pname, int param) {
    // Fog is linear on this engine and the ARB option we inject is the linear
    // one, so never let the mode be switched out from under it while an override
    // is live.
    if (g_fog.enabled && pname == GL_FOG_MODE)
        return g_orig.fogi(pname, static_cast<int>(GL_LINEAR));
    g_orig.fogi(pname, param);
}

// --- wglGetProcAddress interception ------------------------------------------
ProcFn __stdcall Hook_wglGetProcAddress(const char* name) {
    ProcFn real = g_orig.wglGetProcAddress ? g_orig.wglGetProcAddress(name) : nullptr;
    if (!real || !name) return real;

    if (strcmp(name, "glProgramStringARB") == 0) {
        g_orig.programString = reinterpret_cast<ProgramStringFn>(real);
        log::Writef("glhook: game resolved glProgramStringARB -> 0x%08X, wrapping it",
                    reinterpret_cast<uint32_t>(real));
        return reinterpret_cast<ProcFn>(&Hook_glProgramStringARB);
    }
    return real;
}

// --- the opt-in marker --------------------------------------------------------
bool MarkerPresent() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    *(slash + 1) = L'\0';
    lstrcatW(path, L"k2se_fog.txt");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

bool Install() {
    if (!MarkerPresent()) {
        log::Write("fog support: off (no k2se_fog.txt next to the game exe)");
        g_status = kDisabled;
        return false;
    }

    log::Write("fog support: marker found, patching GL imports");

    void* prev = nullptr;
    bool ok = PatchSlot("wglGetProcAddress", kIatWglGetProcAddress,
                        reinterpret_cast<void*>(&Hook_wglGetProcAddress), &prev);
    g_orig.wglGetProcAddress = reinterpret_cast<WglGetProcAddressFn>(prev);

    ok &= PatchSlot("glFogf", off::game::kGlobal_IAT_glFogf,
                    reinterpret_cast<void*>(&Hook_glFogf), &prev);
    g_orig.fogf = reinterpret_cast<FogfFn>(prev);

    ok &= PatchSlot("glFogfv", off::game::kGlobal_IAT_glFogfv,
                    reinterpret_cast<void*>(&Hook_glFogfv), &prev);
    g_orig.fogfv = reinterpret_cast<FogfvFn>(prev);

    ok &= PatchSlot("glFogi", off::game::kGlobal_IAT_glFogi,
                    reinterpret_cast<void*>(&Hook_glFogi), &prev);
    g_orig.fogi = reinterpret_cast<FogiFn>(prev);

    if (!ok || !g_orig.wglGetProcAddress || !g_orig.fogf) {
        log::Write("fog support: REFUSED -- imports did not look as expected, "
                   "rolling back");
        Remove();
        g_status = kRefused;
        return false;
    }

    g_status = kInstalled;
    log::Write("fog support: installed");
    return true;
}

void Remove() {
    for (int i = g_patchedCount - 1; i >= 0; --i)
        WriteSlot(g_patched[i].va, g_patched[i].original, nullptr);
    if (g_patchedCount)
        log::Writef("glhook: %d import slot(s) restored; %d fragment program(s) "
                    "had been rewritten", g_patchedCount, g_patchedPrograms);
    g_patchedCount = 0;
    g_status = kDisabled;
}

int Status() { return g_status; }

void SetEnabled(bool enabled) {
    if (!(g_status & kInstalled)) return;
    g_fog.enabled = enabled;
    if (enabled)
        g_status |= kOverrideActive;
    else
        g_status &= ~kOverrideActive;
    log::Writef("fog override %s", enabled ? "enabled" : "disabled");
}

void SetRange(float start, float end) {
    if (!(g_status & kInstalled)) return;
    g_fog.start = start;
    g_fog.end = end;
    g_fog.haveRange = true;
    log::Writef("fog range set: %d..%d (x1000)", static_cast<int>(start * 1000.0f),
                static_cast<int>(end * 1000.0f));
}

void SetColor(float r, float g, float b) {
    if (!(g_status & kInstalled)) return;
    g_fog.color[0] = r;
    g_fog.color[1] = g;
    g_fog.color[2] = b;
    g_fog.haveColor = true;
    log::Writef("fog colour set: %d,%d,%d (x255)", static_cast<int>(r * 255.0f),
                static_cast<int>(g * 255.0f), static_cast<int>(b * 255.0f));
}

}  // namespace glhook
}  // namespace k2se

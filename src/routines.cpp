#include "routines.h"

#include <windows.h>

#include <cstdio>

#include "exostring.h"
#include "log.h"
#include "offsets.h"
#include "vm.h"
#include "vmstack.h"

namespace k2se {
namespace routines {
namespace {

// The stack accessors now live in vmstack.h, which also owns
// K2SE_ENABLE_STACK_ABI. Track B roughly doubled their number and added the
// CExoString lifetime rules, which is more than an anonymous namespace in the
// routine table should be carrying.
using vmstack::PopFloat;
using vmstack::PopInt;
using vmstack::PopObject;
using vmstack::PopString;
using vmstack::PushFloat;
using vmstack::PushInt;
using vmstack::PushObject;
using vmstack::PushString;

bool g_probeSeen = false;
bool g_extendedSeen = false;
bool g_selfTestSeen = false;
bool g_echoSeen = false;
bool g_bannerPosted = false;
uint32_t g_reportSeenMask = 0;  // one-shot full logging per test id 0..31

// --- on-screen banner --------------------------------------------------------
// void __cdecl AurPostString(const char* text, int x, int y, float fLife)
// Verified by disassembly (see offsets.h). The text buffer is static because
// whether the engine copies the string or keeps the pointer is unproven --
// a static buffer is safe under either semantics.
using AurPostStringFn = void(__cdecl*)(const char* text, int x, int y, float fLife);

void PostBanner(int extendedCount) {
    if (g_bannerPosted) return;
    g_bannerPosted = true;
    static char banner[128];
    _snprintf(banner, sizeof(banner), "K2SE %d.%d.%d active - %d extended routines online",
              K2SE_VERSION_MAJOR, K2SE_VERSION_MINOR, K2SE_VERSION_PATCH, extendedCount);
    banner[sizeof(banner) - 1] = '\0';
    auto fn = reinterpret_cast<AurPostStringFn>(off::kAurPostString);
    fn(banner, 5, 5, 10.0f);
    log::Writef("banner posted on screen: \"%s\"", banner);
}

// --- the extended routine registry ------------------------------------------
// IDs are allocated from 877 upward and, once shipped in a release, are a
// PUBLIC CONTRACT: never reuse an ID. Keep this table, nss/k2se.nss and the
// header generator in tools/m4_deploy_test.py in lockstep -- nwnnsscomp
// assigns IDs by POSITION in nwscript.nss, so table order here must equal
// prototype order there.
//
// 878/879 are development-only self-test routines; they may be renumbered
// before the first public release (nothing has shipped yet).

using ExtendedHandler = int (*)(int nParams);

// 877: int K2SE_GetVersion()
int H_GetVersion(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    if (!PushInt(kVersionEncoded)) return off::kErrPushFailed;
#endif
    return 0;
}

// 878: int K2SE_SelfTest(int nFirst, float fSecond, int nThird)
// End-to-end check of the multi-argument ABI. The return value is
// order-sensitive on purpose: if arguments popped in any order other than
// declaration order, the checksum comes out different and the script-side
// comparison fails loudly.
int H_SelfTest(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int nFirst = 0, nThird = 0;
    float fSecond = 0.0f;
    if (!PopInt(&nFirst)) return off::kErrParam;
    if (!PopFloat(&fSecond)) return off::kErrParam;
    if (!PopInt(&nThird)) return off::kErrParam;

    const int checksum =
        nFirst * 1000000 + static_cast<int>(fSecond * 10.0f) * 10000 + nThird;

    if (!g_selfTestSeen) {
        g_selfTestSeen = true;
        log::Writef("SelfTest pop order: first=%d  second(float x10)=%d  third=%d  -> %d",
                    nFirst, static_cast<int>(fSecond * 10.0f), nThird, checksum);
    }
    if (!PushInt(checksum)) return off::kErrPushFailed;
#endif
    return 0;
}

// 879: int K2SE_ReportTest(int nTestId, int nExpected, int nActual)
// Lets a compiled test script report what IT observed into k2se.log; the
// script is the only place the full round trip (compiler -> bytecode ->
// interpreter -> K2SE -> back) is visible. Returns 1 on match, 0 otherwise.
int H_ReportTest(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    int nTestId = 0, nExpected = 0, nActual = 0;
    if (!PopInt(&nTestId)) return off::kErrParam;
    if (!PopInt(&nExpected)) return off::kErrParam;
    if (!PopInt(&nActual)) return off::kErrParam;

    const bool pass = (nExpected == nActual);
    const bool firstTime =
        nTestId >= 0 && nTestId < 32 && !(g_reportSeenMask & (1u << nTestId));
    if (firstTime) {
        g_reportSeenMask |= 1u << nTestId;
        log::Writef("TEST %2d: expected %d, got %d  ->  %s", nTestId, nExpected, nActual,
                    pass ? "PASS" : "FAIL");
    } else {
        log::Trace("test %d: expected %d, got %d -> %s", nTestId, nExpected, nActual,
                   pass ? "pass" : "FAIL");
    }
    if (!PushInt(pass ? 1 : 0)) return off::kErrPushFailed;
#endif
    return 0;
}

// 880: string K2SE_EchoString(string sIn)
// The string round trip, end to end: pop a script-supplied string, then push it
// straight back. A script comparing the result against what it sent proves the
// whole path -- compiler, bytecode, VM stack, CExoString construction, and the
// engine's copy semantics in both directions.
//
// It is deliberately an identity function. Anything cleverer would make a
// failure ambiguous between "the string ABI is wrong" and "the transformation is
// wrong", and the string ABI is the thing under test.
int H_EchoString(int /*nParams*/) {
#if K2SE_ENABLE_STACK_ABI
    // Must be constructed before the engine touches it: PopString copy-assigns,
    // which frees whatever pointer it finds in the destination first.
    ExoString value;
    if (!value.valid()) {
        log::Write("EchoString: CExoString construction failed");
        return off::kErrParam;
    }
    if (!PopString(&value)) return off::kErrParam;

    if (!g_echoSeen) {
        g_echoSeen = true;
        log::Writef("*** FIRST STRING ROUND TRIP: received \"%s\" (length %u) ***",
                    value.c_str(), value.length());
    }

    // The engine allocates its own copy here, so `value` stays ours and its
    // destructor still runs on the way out of this scope.
    if (!PushString(&value)) return off::kErrPushFailed;
#endif
    return 0;
}

struct Extended {
    int id;
    const char* name;
    int argc;  // Q7: the compiler always emits the full declared argc,
               // materializing omitted defaults itself -- so a mismatch here
               // means a foreign or corrupted script, and we refuse to touch
               // the VM stack rather than desynchronize it.
    ExtendedHandler handler;
};

constexpr Extended kExtended[] = {
    {877, "K2SE_GetVersion", 0, &H_GetVersion},
    {878, "K2SE_SelfTest", 3, &H_SelfTest},
    {879, "K2SE_ReportTest", 3, &H_ReportTest},
    {880, "K2SE_EchoString", 1, &H_EchoString},
};
constexpr int kExtendedCount = static_cast<int>(sizeof(kExtended) / sizeof(kExtended[0]));

}  // namespace

void Init() {
    log::Writef("extended routines registered: %d (first free vanilla ID is %d)", kExtendedCount,
                off::kFirstExtendedId);
    for (const Extended& e : kExtended)
        log::Writef("  id %d -> %s (argc %d)", e.id, e.name, e.argc);
    log::Writef("stack ABI: %s",
                K2SE_ENABLE_STACK_ABI ? "ENABLED" : "disabled (DESIGN.md Q5 unresolved)");
}

bool Intercepts(int id) { return id == off::kRoutineAbs; }

bool DispatchVanillaOverride(void* self, int id, int nParams, int* result) {
    if (id != off::kRoutineAbs) return false;

#if !K2SE_ENABLE_STACK_ABI
    // Without a verified stack ABI we must not touch arguments. Fall through to
    // the engine so abs() keeps working exactly as it always did.
    (void)self;
    (void)nParams;
    (void)result;
    if (!g_probeSeen) {
        g_probeSeen = true;
        log::Write("abs() intercept reached, but the stack ABI is disabled -> passing through.");
    }
    return false;
#else
    (void)self;
    // abs() takes exactly one argument and has no defaults, so any other argc
    // is not the routine we think it is -- hand it back to the engine.
    if (nParams != 1) return false;

    int value = 0;
    if (!PopInt(&value)) {
        log::Write("abs() intercept: StackPopInteger failed");
        *result = off::kErrParam;  // same code the engine's own handler returns
        return true;
    }

    if (value == kProbeMagic) {
        if (!g_probeSeen) {
            g_probeSeen = true;
            log::Writef("PRESENCE PROBE answered with version %d", kVersionEncoded);
        }
        PostBanner(kExtendedCount);
        if (!PushInt(kVersionEncoded)) {
            *result = off::kErrPushFailed;
            return true;
        }
        *result = 0;
        return true;
    }

    // Faithful reimplementation for every other input. abs() is pure, has no
    // default parameters and argc is always 1, so this is bit-identical to
    // vanilla (abs(INT_MIN) is UB in both).
    if (!PushInt(value < 0 ? -value : value)) {
        *result = off::kErrPushFailed;
        return true;
    }
    *result = 0;
    return true;
#endif
}

int DispatchExtended(void* /*self*/, int id, int nParams) {
    for (const Extended& e : kExtended) {
        if (e.id != id) continue;
        if (!g_extendedSeen) {
            g_extendedSeen = true;
            log::Writef("*** FIRST EXTENDED ROUTINE CALL: id=%d (%s), nParams=%d ***", id, e.name,
                        nParams);
            log::Write("*** a compiled NWScript reached a routine that did not exist "
                       "in this engine. ***");
        }
        if (nParams != e.argc) {
            // Q7 guarantees the compiler emits the full declared argc, so this
            // is a script compiled against a mismatched header. Popping an
            // unknown number of values would desynchronize the VM stack;
            // refuse instead. -2001 aborts only the calling script.
            log::Writef("%s called with nParams=%d, declared argc=%d -> refusing (-2001)", e.name,
                        nParams, e.argc);
            return off::kErrParam;
        }
        log::Trace("extended routine %d (%s), nParams=%d", id, e.name, nParams);
        const int rc = e.handler(nParams);
        log::Trace("  -> rc=%d", rc);
        PostBanner(kExtendedCount);
        return rc;
    }

    // Unregistered extended ID: return exactly what the engine returns for an
    // out-of-range routine, so behaviour with K2SE matches behaviour without it.
    log::Trace("unregistered extended routine %d -> %d", id, off::kErrCommandNotFound);
    return off::kErrCommandNotFound;
}

}  // namespace routines
}  // namespace k2se

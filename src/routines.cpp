#include "routines.h"

#include <windows.h>

#include "log.h"
#include "offsets.h"
#include "vm.h"

// ============================================================================
// K2SE_ENABLE_STACK_ABI -- now ON. Q5 is resolved.
//
// The ABI was not guessed; it was read out of the engine's own math handler at
// 0x0068C4A0, which implements routine 77 (abs):
//
//     cmp  dword ptr [ebp+8], 4Dh        ; nCommandId == 77
//     lea  eax, [ebp-4]                  ; &out
//     push eax
//     mov  ecx, dword ptr [0xA1B4A8]     ; ECX = the CVirtualMachine singleton
//     call 0x006FD9A0                    ; StackPopInteger -> EAX nonzero = ok
//     test eax, eax
//     jne  ok
//     mov  eax, 0FFFFF82Fh               ; -2001 on pop failure
//     ...
//     push edx                           ; the result
//     mov  ecx, dword ptr [0xA1B4A8]
//     call 0x006FD9C0                    ; StackPushInteger
//     test eax, eax
//     jne  ok
//     mov  eax, 0FFFFF830h               ; -2000 on push failure
//
// The correction that mattered: the VM comes from the GLOBAL at 0x00A1B4A8, not
// from the handler's own `this`. Reimplementing abs() faithfully means matching
// this sequence exactly, including both error codes.
// ============================================================================
#define K2SE_ENABLE_STACK_ABI 1

namespace k2se {
namespace routines {
namespace {

bool g_probeSeen = false;
bool g_extendedSeen = false;

#if K2SE_ENABLE_STACK_ABI
using StackPopIntegerFn = int(__thiscall*)(void* vm, int* out);
using StackPushIntegerFn = int(__thiscall*)(void* vm, int value);

// The VM singleton, exactly as every engine handler obtains it.
void* VirtualMachine() {
    return *reinterpret_cast<void**>(off::kVirtualMachineGlobal);
}

bool PopInt(int* out) {
    void* vm = VirtualMachine();
    if (!vm) return false;
    auto fn = reinterpret_cast<StackPopIntegerFn>(off::kStackPopInteger);
    return fn(vm, out) != 0;
}

bool PushInt(int value) {
    void* vm = VirtualMachine();
    if (!vm) return false;
    auto fn = reinterpret_cast<StackPushIntegerFn>(off::kStackPushInteger);
    return fn(vm, value) != 0;
}
#endif

// --- the extended routine registry ------------------------------------------
// IDs are allocated from 877 upward and are a PUBLIC CONTRACT: once an ID ships,
// it can never be reused for something else. Keep this table and nss/k2se.nss
// in lockstep, and publish the allocation in the repo so a second extender
// cannot collide with us.

using ExtendedHandler = int (*)(void* self, int nParams, int* result);

int H_GetVersion(void* self, int nParams, int* result) {
    (void)self;
    (void)nParams;
    *result = 0;
#if K2SE_ENABLE_STACK_ABI
    if (!PushInt(kVersionEncoded)) return off::kErrPushFailed;
#endif
    return 0;
}

struct Extended {
    int id;
    const char* name;
    ExtendedHandler handler;
};

constexpr Extended kExtended[] = {
    {877, "K2SE_GetVersion", &H_GetVersion},
};

}  // namespace

void Init() {
    log::Writef("extended routines registered: %d (first free vanilla ID is %d)",
                static_cast<int>(sizeof(kExtended) / sizeof(kExtended[0])), off::kFirstExtendedId);
    for (const Extended& e : kExtended) log::Writef("  id %d -> %s", e.id, e.name);
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

int DispatchExtended(void* self, int id, int nParams) {
    for (const Extended& e : kExtended) {
        if (e.id != id) continue;
        if (!g_extendedSeen) {
            g_extendedSeen = true;
            log::Writef("*** FIRST EXTENDED ROUTINE CALL: id=%d (%s), nParams=%d ***", id, e.name,
                        nParams);
            log::Write("*** a compiled NWScript reached a routine that did not exist "
                       "in this engine. ***");
        }
        log::Trace("extended routine %d (%s), nParams=%d", id, e.name, nParams);
        int result = 0;
        const int rc = e.handler(self, nParams, &result);
        log::Trace("  -> pushed %d, rc=%d", kVersionEncoded, rc);
        return rc;
    }

    // Unregistered extended ID: return exactly what the engine returns for an
    // out-of-range routine, so behaviour with K2SE matches behaviour without it.
    log::Trace("unregistered extended routine %d -> %d", id, off::kErrCommandNotFound);
    return off::kErrCommandNotFound;
}

}  // namespace routines
}  // namespace k2se

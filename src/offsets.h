#pragma once
#include <cstdint>

// =============================================================================
// Addresses for swkotor2.exe -- Aspyr/Steam build, FileVersion 1.0.2.0,
// TimeDateStamp 0x5603005D, ImageBase 0x00400000, RELOCS_STRIPPED (no ASLR).
//
// Provenance of every value in this file: read directly out of the binary.
// tools/verify_offsets.py re-checks all of them and must pass before any
// hook code is trusted. See DESIGN.md section 2.
// =============================================================================

namespace k2se {
namespace off {

constexpr uint32_t kImageBase = 0x00400000;
constexpr uint32_t kTimeDateStamp = 0x5603005D;

// PE Characteristics: 0x0103 pristine, 0x0123 once the 4GB/LAA patch is applied.
// Both are accepted -- many users run the LAA-patched exe. Never hash the whole
// file: that would reject the user's own working install.
constexpr uint16_t kCharacteristicsPristine = 0x0103;
constexpr uint16_t kCharacteristicsLaa = 0x0123;

// --- CSWVirtualMachineCommands ----------------------------------------------
// Found via the MSVC RTTI class-name string ".?AVCSWVirtualMachineCommands@@"
// at 0x00A0F4F8 -> type descriptor -> COL -> vtable.
constexpr uint32_t kRttiClassName = 0x00A0F4F8;
constexpr uint32_t kVTable = 0x009940D0;

constexpr uint32_t kVTableSlot0 = 0x00536BE0;         // ctor/dtor helper
constexpr uint32_t kInitializeCommands = 0x00665F50;  // vtable slot[1]
constexpr uint32_t kExecuteCommand = 0x00668FD0;      // vtable slot[2]  <-- hook target
constexpr uint32_t kVTableSlot3 = 0x00669020;

// The slot we overwrite. &vtable[2].
constexpr uint32_t kVTableSlotExecute = kVTable + 2 * 4;  // 0x009940D8

// Object layout, derived from the instructions that use these offsets.
constexpr uint32_t kCommandsFieldOffset = 0x0C;  // CSWVirtualMachineCommands::m_pCommands
constexpr uint32_t kVmInternalOffset = 0x1C;     // CVirtualMachine::m_pInternal

// --- the routine table ------------------------------------------------------
// InitializeCommands does: push 0DB4h ; new[] ; mov [this+0Ch], eax
// 0x0DB4 == 3508 == 877 * 4, i.e. a flat array of 877 function pointers,
// heap-allocated at runtime (which is why no static scan of .rdata/.data
// ever finds it).
constexpr int kVanillaRoutineCount = 877;  // 0x36D
constexpr uint32_t kTableAllocBytes = 0x0DB4;

// First routine ID K2SE may claim. See DESIGN.md Q3 -- whether IDs >= 877
// actually reach the dispatcher is the project's gating unknown.
constexpr int kFirstExtendedId = kVanillaRoutineCount;  // 877

// --- fingerprint probe sites ------------------------------------------------
constexpr uint32_t kSiteAllocSize = 0x00665F5A;      // dword == 0x0DB4
constexpr uint32_t kSiteInitBound = 0x00665F87;      // dword == 0x36D
constexpr uint32_t kSiteDispatchBound = 0x00668FDC;  // dword == 0x36D
constexpr uint32_t kSiteCmdOffInit = 0x00665F71;     // byte  == 0x0C  (mov [eax+0Ch],ecx)
constexpr uint32_t kSiteCmdOffDispatch = 0x00668FE7; // byte  == 0x0C  (mov ecx,[eax+0Ch])
constexpr uint32_t kSiteVmInternal = 0x006FD9B0;     // byte  == 0x1C  (mov ecx,[ecx+1Ch])

// --- known table entries, for runtime self-validation -----------------------
// Confirmed by tools/extract_routine_table.py against InitializeCommands.
// IDs 67..77 (fabs,cos,sin,tan,acos,asin,atan,log,pow,sqrt,abs) all share one
// handler -- that shared-handler pattern, cross-checked against xoreos-tools'
// independent name list, is what proves this really is the routine table.
constexpr int kRoutineAbs = 77;
constexpr int kRoutineAcos = 71;
constexpr int kRoutineRandom = 0;
constexpr int kRoutineLast = 876;

constexpr uint32_t kHandlerMath = 0x0068C4A0;         // cmds[67..77]
constexpr uint32_t kHandlerRandom = 0x0068F5D0;       // cmds[0]
constexpr uint32_t kHandlerRebuildParty = 0x0069C460; // cmds[876]

// --- engine helpers ---------------------------------------------------------
// AurPostString(text, x, y, seconds) -- on-screen debug text. Used to make a
// refused install visible instead of silently inert.
constexpr uint32_t kAurPostString = 0x00474C00;

// The CVirtualMachine singleton. RESOLVED (DESIGN.md Q5): engine routine
// handlers do NOT derive the VM from their own `this`. They load it from this
// global, e.g. inside the math handler at 0x0068C4A0:
//     mov ecx, dword ptr [0x00A1B4A8]
//     call 0x006FD9A0                  ; StackPopInteger
// The earlier design assumed `self + 0x1C`; that was wrong. 0x1C is used one
// level deeper, INSIDE the accessor (`mov ecx,[ecx+1Ch]`), not by the caller.
constexpr uint32_t kVirtualMachineGlobal = 0x00A1B4A8;

// VM stack accessors -- VERIFIED by disassembly (DESIGN.md Q5).
//   int __thiscall StackPopInteger (void* vm, int* out);    ret 4
//   int __thiscall StackPushInteger(void* vm, int  value);  ret 4
// Both take the VM in ECX, one stack argument, and return EAX != 0 on success.
// Each forwards to *(vm + 0x1C) internally.
constexpr uint32_t kStackPopInteger = 0x006FD9A0;
constexpr uint32_t kStackPushInteger = 0x006FD9C0;
constexpr uint32_t kStackPopFloat = 0x006FD9E0;

// --- dispatcher / handler return codes --------------------------------------
// Read straight out of the engine's own error paths:
//   ExecuteCommand out-of-range / NULL slot -> mov eax, 0FFFFF82Eh  (-2002)
//   handler, pop failed                     -> mov eax, 0FFFFF82Fh  (-2001)
//   handler, push failed                    -> mov eax, 0FFFFF830h  (-2000)
// Opaque internal codes; NOT dialog.tlk strrefs.
constexpr int kErrCommandNotFound = -2002;
constexpr int kErrParam = -2001;
constexpr int kErrPushFailed = -2000;

}  // namespace off
}  // namespace k2se

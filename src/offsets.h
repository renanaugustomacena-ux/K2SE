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
// void __cdecl AurPostString(const char* text, int x, int y, float fLife)
// On-screen debug text. VERIFIED by disassembly: plain `ret` (cdecl), four
// stack args with the float passed by value; allocates a 0x434-byte text
// object and constructs it via 0x004744D0. Engine call site 0x0040820A passes
// a raw .rdata char* literal (">") and 5.0f -- so `text` is a C string, not a
// CExoString. Used for the K2SE banner and to make a refused install visible
// instead of silently inert.
constexpr uint32_t kAurPostString = 0x00474C00;

// The CVirtualMachine singleton. RESOLVED (DESIGN.md Q5): engine routine
// handlers do NOT derive the VM from their own `this`. They load it from this
// global, e.g. inside the math handler at 0x0068C4A0:
//     mov ecx, dword ptr [0x00A1B4A8]
//     call 0x006FD9A0                  ; StackPopInteger
// The earlier design assumed `self + 0x1C`; that was wrong. 0x1C is used one
// level deeper, INSIDE the accessor (`mov ecx,[ecx+1Ch]`), not by the caller.
constexpr uint32_t kVirtualMachineGlobal = 0x00A1B4A8;

// VM stack accessors -- VERIFIED by disassembly (DESIGN.md Q5 + session of
// 2026-08-26: float pair read out of the shared math handler 0x0068C4A0,
// object pair read out of GetArea 0x0067A070 / GetFirstPC 0x006875E0).
//   int __thiscall StackPopInteger (void* vm, int*   out);   ret 4
//   int __thiscall StackPushInteger(void* vm, int    value); ret 4
//   int __thiscall StackPopFloat   (void* vm, float* out);   ret 4
//   int __thiscall StackPushFloat  (void* vm, float  value); ret 4
//   int __thiscall StackPopObject  (void* vm, uint32_t* out);   ret 4
//   int __thiscall StackPushObject (void* vm, uint32_t objId);  ret 4
// All take the VM in ECX and return EAX != 0 on success; each forwards to
// *(vm + 0x1C) internally. Arguments pop in DECLARATION order (Q6, settled by
// the pow branch: the first pop feeds CRT pow() as the base, i.e. fValue).
constexpr uint32_t kStackPopInteger = 0x006FD9A0;
constexpr uint32_t kStackPushInteger = 0x006FD9C0;
constexpr uint32_t kStackPopFloat = 0x006FD9E0;
constexpr uint32_t kStackPushFloat = 0x006FDA00;
constexpr uint32_t kStackPopObject = 0x006FDAF0;
constexpr uint32_t kStackPushObject = 0x006FDB10;

// Shape-verified only (thin wrappers around *(vm+0x1C) methods; no consuming
// handler has been read yet). Do NOT wrap these until a ground-truth handler
// confirms them:
//   0x006FDA20  StackPopVector? (void* vm, float out[3])          ret 4
//   0x006FDA40  StackPushVector?(void* vm, float x, float y, float z) ret 0xC

// The engine's own object-id sentinel, seen as the default/failure value in
// GetFirstPC (mov [ebp-4], 0x7F000000) -- matches community OBJECT_INVALID.
constexpr uint32_t kObjectInvalid = 0x7F000000;

// --- accessor call-site probe values ----------------------------------------
// Each is the rel32 of a verified E8 call to the accessor, read from a handler
// that provably uses it. Checking the call site (not the callee's first bytes)
// also re-derives the accessor address at fingerprint time.
constexpr uint32_t kSitePopFloatCall = 0x0068C514;   // dword == 0x000714C8
constexpr uint32_t kSitePopFloatRel = 0x000714C8;    // -> 0x006FD9E0 (math handler)
constexpr uint32_t kSitePushFloatCall = 0x0068C774;  // dword == 0x00071288
constexpr uint32_t kSitePushFloatRel = 0x00071288;   // -> 0x006FDA00 (math handler)
constexpr uint32_t kSitePopObjectCall = 0x0067A0BA;  // dword == 0x00083A32
constexpr uint32_t kSitePopObjectRel = 0x00083A32;   // -> 0x006FDAF0 (GetArea)
constexpr uint32_t kSitePushObjectCall = 0x00687692; // dword == 0x0007647A
constexpr uint32_t kSitePushObjectRel = 0x0007647A;  // -> 0x006FDB10 (GetFirstPC)
// AurPostString prologue: 55 8B EC 6A (push ebp; mov ebp,esp; push -1 ...)
constexpr uint32_t kSiteAurPostString = 0x00474C00;  // dword == 0x6AEC8B55
constexpr uint32_t kAurPostStringPrologue = 0x6AEC8B55;

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

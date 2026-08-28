// =============================================================================
// GENERATED FILE -- DO NOT EDIT.
//
//   source: data/k2se_addresses.csv
//   regen : python tools/gen_offsets.py
//
// Addresses for swkotor2.exe, Aspyr/Steam build, FileVersion 1.0.2.0,
// TimeDateStamp 0x5603005D. The executable is RELOCS_STRIPPED with no ASLR, so
// every value here is a stable absolute VA.
//
// Provenance is recorded per constant:
//   kpm-db       Kotor-Patch-Manager AddressDatabases/kotor2_steam_aspyr.db (MIT).
//                Data only -- no code from that project is used in K2SE.
//   k2se-ghidra  derived in this project.
//
// Verification is a claim about THIS binary, not about the source:
//   runtime      exercised in a live game session
//   disasm       the function body was read
//   callsite     reached via the rel32 of a verified E8 in a real handler
//   prologue     entry point is in .text and starts with 55 8B EC
//   unverified   imported but unchecked -- MUST NOT be called from shipping code
//
// tools/verify_offsets.py re-checks every row against the executable.
// =============================================================================

#pragma once
#include <cstdint>

namespace k2se {
namespace off {
namespace game {

// --- functions ---------------------------------------------------
// engine entry points

// CClientExoApp
constexpr uint32_t kCClientExoApp_GetClientOptions = 0x0072FB00;  // prologue

// CClientOptions
constexpr uint32_t kCClientOptions_SetCameraMode = 0x007B5E50;  // prologue

// CExoString
constexpr uint32_t kCExoString_CStrConstructor    = 0x00733570;  // prologue
constexpr uint32_t kCExoString_CStrLenConstructor = 0x00733680;  // prologue
constexpr uint32_t kCExoString_DefaultConstructor = 0x00733540;  // prologue
constexpr uint32_t kCExoString_Destructor         = 0x00733780;  // prologue

// CGameObjectArray
constexpr uint32_t kCGameObjectArray_GetGameObject = 0x0053DFB0;  // prologue

// CSWInventory
constexpr uint32_t kCSWInventory_GetItemInSlot = 0x006D0620;  // prologue

// CSWItem
constexpr uint32_t kCSWItem_GetBaseItem = 0x006D6E30;  // prologue

// CSWSCreature
constexpr uint32_t kCSWSCreature_GetClientCreature = 0x0058AE90;  // prologue

// CSWSCreatureStats
constexpr uint32_t kCSWSCreatureStats_AddFeat         = 0x006F5B40;  // prologue
constexpr uint32_t kCSWSCreatureStats_AddKnownSpell   = 0x006B86A0;  // prologue
constexpr uint32_t kCSWSCreatureStats_GetClass        = 0x00844E40;  // prologue
constexpr uint32_t kCSWSCreatureStats_GetSkillRank    = 0x006B7BB0;  // prologue
constexpr uint32_t kCSWSCreatureStats_HasFeat         = 0x006B83F0;  // prologue
constexpr uint32_t kCSWSCreatureStats_HasSpell        = 0x006BD9C0;  // prologue
constexpr uint32_t kCSWSCreatureStats_RemoveFeat      = 0x006B85F0;  // prologue
constexpr uint32_t kCSWSCreatureStats_SetCHABase      = 0x006B69A0;  // prologue
constexpr uint32_t kCSWSCreatureStats_SetCONBase      = 0x006B6850;  // prologue
constexpr uint32_t kCSWSCreatureStats_SetDEXBase      = 0x006B6810;  // prologue
constexpr uint32_t kCSWSCreatureStats_SetINTBase      = 0x006B6920;  // prologue
constexpr uint32_t kCSWSCreatureStats_SetMovementRate = 0x006BA320;  // prologue
constexpr uint32_t kCSWSCreatureStats_SetSTRBase      = 0x006B67D0;  // prologue
constexpr uint32_t kCSWSCreatureStats_SetSkillRank    = 0x006B81C0;  // prologue
constexpr uint32_t kCSWSCreatureStats_SetWISBase      = 0x006B6960;  // prologue

// CSWSObject
constexpr uint32_t kCSWSObject_AddActionToFront = 0x00540CA0;  // prologue

// CServerExoApp
constexpr uint32_t kCServerExoApp_GetCreatureByGameObjectID = 0x0051C100;  // prologue
constexpr uint32_t kCServerExoApp_GetObjectArray            = 0x0051C080;  // prologue
constexpr uint32_t kCServerExoApp_GetPlayerCreatureId       = 0x0051C8F0;  // prologue

// CVirtualMachine
constexpr uint32_t kCVirtualMachine_RunScript                = 0x006FD8D0;  // prologue
constexpr uint32_t kCVirtualMachine_StackPopCommand          = 0x006FDB30;  // prologue
constexpr uint32_t kCVirtualMachine_StackPopEngineStructure  = 0x006FDAB0;  // prologue
constexpr uint32_t kCVirtualMachine_StackPopFloat            = 0x006FD9E0;  // runtime -- exercised in a live K2SE session
constexpr uint32_t kCVirtualMachine_StackPopInteger          = 0x006FD9A0;  // runtime -- exercised in a live K2SE session
constexpr uint32_t kCVirtualMachine_StackPopObject           = 0x006FDAF0;  // runtime -- exercised in a live K2SE session
constexpr uint32_t kCVirtualMachine_StackPopString           = 0x006FDA70;  // prologue
constexpr uint32_t kCVirtualMachine_StackPopVector           = 0x006FDA20;  // prologue
constexpr uint32_t kCVirtualMachine_StackPushEngineStructure = 0x006FDAD0;  // prologue
constexpr uint32_t kCVirtualMachine_StackPushFloat           = 0x006FDA00;  // runtime -- exercised in a live K2SE session
constexpr uint32_t kCVirtualMachine_StackPushInteger         = 0x006FD9C0;  // runtime -- exercised in a live K2SE session
constexpr uint32_t kCVirtualMachine_StackPushObject          = 0x006FDB10;  // runtime -- exercised in a live K2SE session
constexpr uint32_t kCVirtualMachine_StackPushString          = 0x006FDA90;  // prologue
constexpr uint32_t kCVirtualMachine_StackPushVector          = 0x006FDA40;  // prologue

// ConsoleFunc
constexpr uint32_t kConsoleFunc_Destructor         = 0x004759B0;  // prologue
constexpr uint32_t kConsoleFunc_IntConstructor     = 0x00475910;  // prologue
constexpr uint32_t kConsoleFunc_NoParamConstructor = 0x004757D0;  // prologue
constexpr uint32_t kConsoleFunc_StringConstructor  = 0x00475870;  // prologue

// Other
constexpr uint32_t kOther_AurPostString = 0x00474C00;  // runtime -- exercised in a live K2SE session

// --- globals -----------------------------------------------------
// pointer variables; dereference to reach the object
constexpr uint32_t kGlobal_APP_MANAGER_PTR          = 0x00A1B4A4;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_AURORA_PTR               = 0x00A1B4A0;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_EXO_RESOURCE_MANAGER_PTR = 0x00A1B490;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_GUI_MANAGER_PTR          = 0x00A1B49C;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_RENDER_AABB              = 0x00A73284;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_RENDER_GOB_BBS           = 0x00A73740;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_RENDER_GUI               = 0x00A32A30;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_RENDER_PERSONAL_SPACE    = 0x00A2006C;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_RENDER_QA_TRIGGERS       = 0x00A2003C;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_RENDER_TRIGGERS          = 0x00A7E0E8;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_RENDER_WIREFRAME         = 0x00A32A50;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_RULES_PTR                = 0x00A1B4D0;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_TLK_TABLE_PTR            = 0x00A1B4B0;  // unverified -- in .data; holds a runtime pointer, not statically checkable
constexpr uint32_t kGlobal_VIRTUAL_MACHINE_PTR      = 0x00A1B4A8;  // runtime -- exercised in a live K2SE session

// --- struct offsets ----------------------------------------------
// byte displacement from `this`

// CAppManager
constexpr uint32_t kOff_CAppManager_Client = 0x0004;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CAppManager_Server = 0x0008;  // unverified -- struct layout; confirm against a consuming handler

// CExoString
constexpr uint32_t kOff_CExoString_CStr   = 0x0000;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CExoString_Length = 0x0004;  // unverified -- struct layout; confirm against a consuming handler

// CGameObject
constexpr uint32_t kOff_CGameObject_Id         = 0x0004;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CGameObject_ObjectType = 0x0008;  // unverified -- struct layout; confirm against a consuming handler

// CSWBaseItem
constexpr uint32_t kOff_CSWBaseItem_WeaponWield = 0x0008;  // unverified -- struct layout; confirm against a consuming handler

// CSWCCreature
constexpr uint32_t kOff_CSWCCreature_Running = 0x03F8;  // unverified -- struct layout; confirm against a consuming handler

// CSWSCombatRound
constexpr uint32_t kOff_CSWSCombatRound_OnHandAttacks = 0x0AE0;  // unverified -- struct layout; confirm against a consuming handler

// CSWSCreature
constexpr uint32_t kOff_CSWSCreature_CreatureStats = 0x1198;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CSWSCreature_Inventory     = 0x1150;  // unverified -- struct layout; confirm against a consuming handler

// CSWSCreatureStats
constexpr uint32_t kOff_CSWSCreatureStats_CHABase    = 0x00F7;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CSWSCreatureStats_CONBase    = 0x00F1;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CSWSCreatureStats_ClassCount = 0x008D;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CSWSCreatureStats_DEXBase    = 0x00EF;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CSWSCreatureStats_INTBase    = 0x00F3;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CSWSCreatureStats_STRBase    = 0x00ED;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CSWSCreatureStats_WISBase    = 0x00F5;  // unverified -- struct layout; confirm against a consuming handler

// CSWSObject
constexpr uint32_t kOff_CSWSObject_AreaId      = 0x0090;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CSWSObject_Orientation = 0x00A0;  // unverified -- struct layout; confirm against a consuming handler
constexpr uint32_t kOff_CSWSObject_Position    = 0x0094;  // unverified -- struct layout; confirm against a consuming handler

}  // namespace game
}  // namespace off
}  // namespace k2se

#include <windows.h>

#include "fingerprint.h"
#include "glhook.h"
#include "log.h"
#include "offsets.h"
#include "routines.h"
#include "vm.h"

// The proxy stubs resolve the real version.dll lazily, on first call -- not
// here. LoadLibrary from DllMain runs under the loader lock and is exactly the
// pattern MSDN warns about.
//
// The VM hook, by contrast, IS installed here: it needs only VirtualProtect and
// a dword store (both loader-lock safe), and DllMain runs before the EXE entry
// point at RVA 0x0051D5A2, therefore before CSWVirtualMachineCommands is ever
// constructed. That ordering is what makes a vtable-slot swap sufficient.

namespace {

void OnAttach(HMODULE self) {
    DisableThreadLibraryCalls(self);

    k2se::log::Init();
    k2se::fingerprint::LogEnvironment();

    const auto verdict = k2se::fingerprint::Check();
    if (verdict != k2se::fingerprint::Verdict::kMatch) {
        k2se::log::Writef("K2SE_LOAD_REFUSED: %s", k2se::fingerprint::LastFailure());
        k2se::log::Write("Nothing was hooked. The game runs vanilla.");
        return;  // fail safe: an inert hook is worse than no hook
    }

    k2se::log::Write("fingerprint OK");
    k2se::routines::Init();

    // Opt-in and self-contained: if fog support refuses to install, the VM hook
    // is unaffected and K2SE still loads. It is deliberately attempted after the
    // fingerprint has passed, so it never touches an unrecognised binary.
    k2se::glhook::Install();

    if (k2se::vm::InstallHook()) {
        k2se::log::Write("K2SE_LOAD_OK");
    } else {
        k2se::log::Writef("K2SE_LOAD_REFUSED: hook installation failed");
    }
}

void OnDetach() {
    k2se::glhook::Remove();
    k2se::vm::RemoveHook();
    k2se::log::Shutdown();
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            OnAttach(module);
            break;
        case DLL_PROCESS_DETACH:
            OnDetach();
            break;
        default:
            break;
    }
    return TRUE;
}

#pragma once
#include <cstdint>

// ============================================================================
// Call-site redirection.
//
// K2SE's first hook was one dword in a vtable. The movement features need to
// intercept plain functions the engine calls directly (`call rel32`), and the
// least invasive way to do that is to rewrite the rel32 of the specific call
// sites -- four bytes each, inside .text, no trampolines, no relocated
// prologues, no guessing at instruction boundaries. The original function stays
// intact, so our stub can call it.
//
// Rules, in the K2SE tradition:
//   * never write unless the five bytes at the site are exactly `E8 <rel32>`
//     AND the rel32 resolves to the function we expect;
//   * log every write and every refusal;
//   * remember every write so DLL_PROCESS_DETACH can put the bytes back.
// ============================================================================

namespace k2se {
namespace callsite {

// Redirects the `call` at `site` (currently targeting `expected`) to `hook`.
// Returns false and writes nothing if the site does not look like that call.
bool Redirect(const char* name, uint32_t site, uint32_t expected, const void* hook);

// Restores every redirected site to its original rel32.
void RestoreAll();

int Count();

}  // namespace callsite
}  // namespace k2se

#include "exostring.h"

#include "log.h"
#include "offsets.h"

namespace k2se {
namespace {

// All __thiscall, all returning `this` in EAX (the MSVC constructor convention).
using CtorDefaultFn = void*(__thiscall*)(void* self);
using CtorCStrFn = void*(__thiscall*)(void* self, const char* src);
using DtorFn = void*(__thiscall*)(void* self);

}  // namespace

ExoString::ExoString() : constructed_(false) {
    Construct(nullptr);
}

ExoString::ExoString(const char* text) : constructed_(false) {
    Construct(text);
}

void ExoString::Construct(const char* text) {
    // Zero first. If the engine call is somehow not made, the shell still looks
    // like a valid empty string rather than holding stack garbage -- which is
    // the difference between a harmless no-op and the engine free()ing whatever
    // happened to be on the stack.
    for (uint8_t& b : storage_) b = 0;

    static_assert(off::kExoStringSize == sizeof(storage_),
                  "CExoString is 8 bytes on this build; see offsets.h");

    if (text == nullptr) {
        auto ctor = reinterpret_cast<CtorDefaultFn>(off::kExoStringCtorDefault);
        ctor(storage_);
    } else {
        auto ctor = reinterpret_cast<CtorCStrFn>(off::kExoStringCtorCStr);
        ctor(storage_, text);
    }
    constructed_ = true;
}

ExoString::~ExoString() {
    if (!constructed_) return;
    // Frees the game-owned buffer and nulls the pointer. Running this exactly
    // once per construction is the whole contract.
    auto dtor = reinterpret_cast<DtorFn>(off::kExoStringDtor);
    dtor(storage_);
    constructed_ = false;
}

const char* ExoString::c_str() const {
    const char* p = *reinterpret_cast<const char* const*>(storage_ + off::kExoStringOffCStr);
    return p ? p : "";
}

uint32_t ExoString::length() const {
    return *reinterpret_cast<const uint32_t*>(storage_ + off::kExoStringOffLength);
}

}  // namespace k2se

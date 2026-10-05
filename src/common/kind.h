// Value kinds: the coarse runtime classification shared by the IR, the
// bytecode format, the verifier and the VM. Every static type maps to one
// kind; all heap types share the Ref kind.
#pragma once

#include <cstdint>

namespace ember {

enum class Kind : uint8_t { Unit = 0, Bool = 1, Int = 2, Float = 3, Ref = 4, Any = 5 };
// Any appears only in native-function signatures (print, str): the native
// accepts a value of any kind. It is never the kind of a stored value.

inline const char* kind_name(Kind k) {
    switch (k) {
        case Kind::Unit: return "unit";
        case Kind::Bool: return "bool";
        case Kind::Int: return "int";
        case Kind::Float: return "float";
        case Kind::Ref: return "ref";
        case Kind::Any: return "any";
    }
    return "?";
}

inline bool kind_valid_for_storage(uint8_t k) { return k <= static_cast<uint8_t>(Kind::Ref); }

}  // namespace ember

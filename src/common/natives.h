// The native function table: builtins that need the runtime (print, str,
// input, parse_int, panic, assert) followed by the std-module functions in
// the same order as the resolver's native_table(). Native ids are part of
// the bytecode format: never reorder, only append.
#pragma once

#include <cstdint>
#include <vector>

#include "common/kind.h"

namespace ember {

struct NativeDesc {
    const char* module;  // "" for builtins
    const char* name;
    std::vector<Kind> params;
    Kind ret;
};

enum NativeId : uint16_t {
    kNativePrint = 0,
    kNativeStr = 1,
    kNativeInput = 2,
    kNativeParseInt = 3,
    kNativePanic = 4,
    kNativeAssert = 5,
    kFirstModuleNative = 6,
};

const std::vector<NativeDesc>& natives();

}  // namespace ember

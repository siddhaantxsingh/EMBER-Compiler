#pragma once

#include <cstdint>

#include "vm/value.h"

namespace ember::vm {

class VM;

// Execute native function `id` (see common/natives.h) with arguments at
// args[0..n). The arguments stay on the VM stack (rooted) during the call.
Value call_native(VM& vm, uint16_t id, Value* args);

}  // namespace ember::vm

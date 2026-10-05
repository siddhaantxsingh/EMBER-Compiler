// libFuzzer target: compile and run the program with tight limits. Any
// crash, sanitizer report, internal compiler error or VM internal error is a
// bug; runtime errors of the program itself are fine.
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>

#include "driver/driver.h"
#include "vm/vm.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 10000) return 0;
    std::string text(reinterpret_cast<const char*>(data), size);
    auto c = ember::compile("fuzz.em", text, ember::CompileOptions{});
    if (c->internal_error) __builtin_trap();
    if (!c->ok) return 0;
    ember::vm::VMOptions o;
    o.instruction_budget = 200000;
    o.max_heap_bytes = 16u << 20;
    o.max_call_depth = 500;
    o.max_stack_values = 1u << 16;
    o.max_string_bytes = 1u << 20;
    o.max_array_length = 1u << 16;
    o.gc_stress = size % 7 == 0;
    std::ostringstream out;
    std::istringstream in("fuzz input\n");
    ember::vm::VM vm(c->bytecode, o);
    vm.set_output(&out);
    vm.set_input(&in);
    auto r = vm.run();
    if (!r.ok && (r.kind == ember::vm::ErrorKind::Internal || r.kind == ember::vm::ErrorKind::TypeMismatch))
        __builtin_trap();  // compiled code must never hit these
    return 0;
}

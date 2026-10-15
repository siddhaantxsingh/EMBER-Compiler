// Native backend: lowers the typed CFG IR (ir::Module) to LLVM IR and runs
// it with LLVM's ORC JIT.
//
// Supported subset (checked by check_support before any code is generated):
//   * kinds Unit, Bool, Int, Float, and Ref values that are strings
//   * all arithmetic / comparison / conversion ops, with the VM's traps
//   * locals, globals (with the "read before initializer" check), direct
//     calls, recursion (with the VM's call-depth limit), branches, loops
//   * strings: constants, concat, len, index, comparison, str(), print()
//   * arrays of any supported element kind (including nested arrays):
//     literals, [x; n], index get/set, len, push, pop, for-each
//   * natives: print, str, input, parse_int, panic, assert, math.*,
//     strings.*,
//     time.clock
// Not supported (the program is rejected with a reason, never miscompiled):
// closures and upvalues, indirect calls / function values, structs, enums.
// Strings and arrays are arena-allocated and never collected (runtime.h).
#pragma once

#include <iosfwd>
#include <string>

#include "ir/ir.h"
#include "native/runtime.h"

namespace ember::native {

struct Support {
    bool ok = true;
    std::string reason;  // first unsupported construct, when !ok
};

Support check_support(const ir::Module& m);

struct NativeOptions {
    int llvm_opt_level = 2;  // 0..3, LLVM's own pipeline
    uint32_t max_call_depth = 10000;
    size_t max_string_bytes = 64u * 1024 * 1024;
    size_t arena_limit_bytes = 2ull * 1024 * 1024 * 1024;
};

struct NativeRun {
    bool unsupported = false;
    std::string unsupported_reason;
    bool internal_error = false;  // LLVM verifier or JIT failure (a backend bug)
    std::string internal_message;
    RtOutcome outcome;
    double compile_seconds = 0;  // IR -> LLVM -> optimized machine code
    double run_seconds = 0;
};

// Compiles and runs init + main. Output goes to `out`, input() reads `in`.
NativeRun compile_and_run(const ir::Module& m, std::ostream& out, std::istream& in, const NativeOptions& opts);

// Textual LLVM IR after optimization (for `ember native --emit-llvm`).
// Returns false and sets *error if the module is unsupported or invalid.
bool emit_llvm_ir(const ir::Module& m, const NativeOptions& opts, std::string& text, std::string& error);

}  // namespace ember::native

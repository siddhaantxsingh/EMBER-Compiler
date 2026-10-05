// Bytecode verifier. Run on every module before execution (compiled or
// loaded from a file). It proves, by abstract interpretation over value
// kinds, that for every reachable instruction:
//   * the instruction and its operands lie inside the code and jumps land on
//     instruction boundaries;
//   * every index operand (local, upvalue, global, string, function, struct,
//     field, enum, variant, signature, native) is in range;
//   * the operand stack never underflows, never exceeds the declared
//     max_stack, and has the same depth and kinds on every path into a pc;
//   * every instruction receives operands of the kinds it requires;
//   * execution cannot fall off the end of a function.
// What it does NOT prove (checked by the VM at run time instead): which
// object type a Ref points to (string/array/struct/enum/closure and their
// element/struct/enum identity), that globals and captured variables are
// initialized, and the signature of a closure called indirectly.
#pragma once

#include <string>
#include <vector>

#include "bc/module.h"

namespace ember::bc {

struct VerifyResult {
    std::vector<std::string> errors;  // empty = valid
    bool ok() const { return errors.empty(); }
};

VerifyResult verify(const Module& m);

}  // namespace ember::bc

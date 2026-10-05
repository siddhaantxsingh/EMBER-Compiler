// IR -> bytecode. Consumes only the IR (never the AST).
//
// Temporaries are mapped onto the VM operand stack where possible: a temp
// with one use in its defining block stays on the stack if it is consumed in
// stack order; constants are re-materialized at their use; every other temp
// gets a dedicated frame slot. The operand stack is empty at every block
// boundary, which keeps verification simple and makes jumps cheap.
#pragma once

#include <stdexcept>
#include <string>

#include "bc/module.h"
#include "ir/ir.h"

namespace ember::codegen {

// Thrown when a function exceeds a format limit (e.g. 65535 frame slots);
// reported to the user as a "limit" error.
struct LimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct CodegenOptions {
    bool block_layout = true;       // greedy trace layout (false: creation order)
    bool superinstructions = true;  // fuse LOAD/LOAD, STORE/LOAD, int compare + branch
};

bc::Module generate(const ir::Module& m, const std::string& source_name, CodegenOptions opts = {});

}  // namespace ember::codegen

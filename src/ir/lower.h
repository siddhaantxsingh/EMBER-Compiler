#pragma once

#include "front/ast.h"
#include "ir/ir.h"
#include "sema/types.h"

namespace ember::ir {

struct LowerOptions {
    // Loop rotation: test the loop condition at the bottom of the loop (with
    // one extra test before entry), so each iteration executes one jump less.
    bool rotate_loops = true;
};

// Lower a fully type-checked program to IR. Must only be called when
// semantic analysis reported no errors.
Module lower_program(Program& prog, SemaResult& sema, LowerOptions opts = {});

Kind kind_of(const Type* t);

}  // namespace ember::ir

// Reference interpreter: a deliberately simple tree-walking evaluator over
// the type-checked AST. It shares the front end with the compiler but
// nothing after type checking (no IR, no code generator, no bytecode, no
// VM, no GC: values are reference counted). It exists so that differential
// tests can compare the full compile+VM pipeline against an independent
// implementation of the language semantics.
//
// It is slow and leaks reference cycles; it is a testing tool only.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

#include "front/ast.h"
#include "sema/types.h"

namespace ember {

struct RefResult {
    bool ok = true;
    bool limit_hit = false;  // stopped by the step or depth limit (inconclusive)
    std::string error;       // runtime error message ("division by zero", ...)
};

struct RefOptions {
    uint64_t max_steps = 50'000'000;  // evaluation steps before giving up
    uint32_t max_depth = 2000;        // Ember call depth ("stack overflow" past it)
};

// Run a program that passed type checking. Output goes to `out`.
// Must be called on a thread with a large stack when max_depth is large;
// run_reference_big_stack() does that.
RefResult run_reference(Program& prog, SemaResult& sema, std::ostream& out, const RefOptions& opts);
RefResult run_reference_big_stack(Program& prog, SemaResult& sema, std::ostream& out, const RefOptions& opts);

}  // namespace ember

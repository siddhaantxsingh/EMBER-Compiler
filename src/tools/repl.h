// Interactive read-eval-print loop (`ember repl`).
//
// Design: replay. The REPL keeps the declarations entered so far (functions,
// structs, enums, consts, imports, globals) and the statements entered so far
// (the body of an implicit main). Each new input is compiled together with
// that state and the whole program is re-run on a fresh VM; only output
// beyond what was already shown is printed. Inputs that fail to compile or
// raise a runtime error are reported and discarded. Consequence: side
// effects are replayed (input() is read again), and long sessions get
// slower; documented in docs/ARCHITECTURE.md.
#pragma once

#include <iosfwd>

namespace ember {

int run_repl(std::istream& in, std::ostream& out, bool interactive);

}  // namespace ember

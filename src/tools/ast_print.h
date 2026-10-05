#pragma once

#include <string>

#include "front/ast.h"

namespace ember {

// Indented tree dump of the AST. If the program has been type checked,
// expression types are shown in brackets. with_locations adds line:col.
std::string print_ast(const Program& prog, bool with_locations = true);

}  // namespace ember

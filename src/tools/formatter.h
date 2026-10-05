// Source formatter (`ember fmt`). Parses the file and prints it in the
// canonical style: 4-space indentation, one statement per line, minimal
// parentheses, normalized literals. Comments are preserved: comments between
// statements/items stay in place, end-of-line comments stay on their line;
// a comment inside an expression is moved to the next statement boundary.
// Single blank lines between statements and items are kept.
#pragma once

#include <string>

namespace ember {

struct FormatResult {
    bool ok = false;
    std::string output;       // formatted source when ok
    std::string diagnostics;  // syntax errors when !ok
};

FormatResult format_source(const std::string& name, const std::string& text);

}  // namespace ember

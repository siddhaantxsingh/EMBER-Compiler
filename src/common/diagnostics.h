// Compile-time diagnostics: collected by every front-end stage and rendered
// in a rustc-like format with the offending source line and a caret.
#pragma once

#include <string>
#include <vector>

#include "common/source.h"

namespace ember {

enum class Severity { Error, Warning, Note };

// Stable category names. They appear in output ("error[type]") and tests
// match on them, so treat them as part of the user-facing interface.
enum class DiagKind { Lexical, Syntax, Name, Type, Limit, Module };

const char* diag_kind_name(DiagKind k);

struct Diagnostic {
    Severity severity = Severity::Error;
    DiagKind kind = DiagKind::Syntax;
    std::string message;
    Span span;
    std::vector<std::string> notes;  // "expected: X", "found: Y", hints
};

class Diagnostics {
public:
    // Stop collecting after this many errors: later errors are usually
    // cascades of the first, and unbounded output is itself a hazard.
    static constexpr size_t kMaxErrors = 50;

    void error(DiagKind kind, Span span, std::string message, std::vector<std::string> notes = {});
    bool has_errors() const { return error_count_ > 0; }
    size_t error_count() const { return error_count_; }
    bool full() const { return error_count_ >= kMaxErrors; }
    const std::vector<Diagnostic>& all() const { return diags_; }

    std::string render(const SourceFile& file) const;
    static std::string render_one(const Diagnostic& d, const SourceFile& file);

private:
    std::vector<Diagnostic> diags_;
    size_t error_count_ = 0;
};

}  // namespace ember

#include "common/diagnostics.h"

#include <algorithm>
#include <sstream>

namespace ember {

const char* diag_kind_name(DiagKind k) {
    switch (k) {
        case DiagKind::Lexical: return "lexical";
        case DiagKind::Syntax: return "syntax";
        case DiagKind::Name: return "name";
        case DiagKind::Type: return "type";
        case DiagKind::Limit: return "limit";
        case DiagKind::Module: return "module";
    }
    return "error";
}

void Diagnostics::error(DiagKind kind, Span span, std::string message, std::vector<std::string> notes) {
    if (full()) return;
    diags_.push_back(Diagnostic{Severity::Error, kind, std::move(message), span, std::move(notes)});
    ++error_count_;
    if (full()) {
        diags_.push_back(Diagnostic{Severity::Note, kind, "too many errors; stopping", Span{}, {}});
    }
}

std::string Diagnostics::render_one(const Diagnostic& d, const SourceFile& file) {
    std::ostringstream out;
    const char* sev = d.severity == Severity::Error ? "error" : d.severity == Severity::Warning ? "warning" : "note";
    out << sev << "[" << diag_kind_name(d.kind) << "]: " << d.message << "\n";
    if (d.span.valid()) {
        out << "  --> " << file.name() << ":" << d.span.line << ":" << d.span.column << "\n";
        std::string ln = std::to_string(d.span.line);
        std::string pad(ln.size(), ' ');
        std::string_view text = file.line_text(d.span.line);
        // Very long lines (generated or minified code) are shown as a window
        // around the error so the report stays readable.
        constexpr uint32_t kWindow = 100;
        uint32_t col = d.span.column;
        std::string prefix, suffix;
        if (text.size() > 2 * kWindow) {
            uint32_t from = col > kWindow / 2 ? col - kWindow / 2 : 1;
            if (from > 1) prefix = "...";
            size_t len = std::min<size_t>(kWindow, text.size() - (from - 1));
            if ((from - 1) + len < text.size()) suffix = "...";
            text = text.substr(from - 1, len);
            col = col - from + 1 + static_cast<uint32_t>(prefix.size());
        }
        out << pad << " |\n";
        out << ln << " | " << prefix << text << suffix << "\n";
        out << pad << " | ";
        std::string shown = prefix + std::string(text);
        // Reproduce tabs so the caret lines up under tab-indented code.
        for (uint32_t i = 1; i < col && i - 1 < shown.size(); ++i) out << (shown[i - 1] == '\t' ? '\t' : ' ');
        uint32_t width = 1;
        if (d.span.end > d.span.start) {
            uint32_t remaining = static_cast<uint32_t>(shown.size()) >= col ? static_cast<uint32_t>(shown.size()) - col + 1 : 1;
            width = std::min(d.span.end - d.span.start, remaining);
            if (width == 0) width = 1;
        }
        out << std::string(width, '^') << "\n";
    }
    for (const auto& n : d.notes) out << "  = " << n << "\n";
    return out.str();
}

std::string Diagnostics::render(const SourceFile& file) const {
    std::string s;
    for (const auto& d : diags_) {
        s += render_one(d, file);
        s += "\n";
    }
    return s;
}

}  // namespace ember

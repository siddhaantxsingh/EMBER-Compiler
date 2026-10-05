// Source files and locations.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ember {

// A loaded source file. Line starts are computed once so that offsets can be
// mapped to (line, column) cheaply for diagnostics and debug info.
class SourceFile {
public:
    SourceFile(std::string name, std::string text);

    const std::string& name() const { return name_; }
    const std::string& text() const { return text_; }

    // 1-based line and column (column counts bytes).
    uint32_t line_of(uint32_t offset) const;
    uint32_t column_of(uint32_t offset) const;
    // The text of a 1-based line without its newline.
    std::string_view line_text(uint32_t line) const;
    uint32_t line_count() const { return static_cast<uint32_t>(line_starts_.size()); }

private:
    std::string name_;
    std::string text_;
    std::vector<uint32_t> line_starts_;
};

// A half-open byte range [start, end) in one source file plus its start
// position. Spans are small and copied freely.
struct Span {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t line = 0;    // 1-based; 0 = unknown
    uint32_t column = 0;  // 1-based

    bool valid() const { return line != 0; }
    static Span merge(const Span& a, const Span& b);
};

}  // namespace ember

#include "common/source.h"

#include <algorithm>

namespace ember {

SourceFile::SourceFile(std::string name, std::string text)
    : name_(std::move(name)), text_(std::move(text)) {
    line_starts_.push_back(0);
    for (uint32_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') line_starts_.push_back(i + 1);
    }
}

uint32_t SourceFile::line_of(uint32_t offset) const {
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
    return static_cast<uint32_t>(it - line_starts_.begin());
}

uint32_t SourceFile::column_of(uint32_t offset) const {
    uint32_t line = line_of(offset);
    return offset - line_starts_[line - 1] + 1;
}

std::string_view SourceFile::line_text(uint32_t line) const {
    if (line == 0 || line > line_starts_.size()) return {};
    uint32_t start = line_starts_[line - 1];
    uint32_t end = line < line_starts_.size() ? line_starts_[line] : static_cast<uint32_t>(text_.size());
    std::string_view s(text_.data() + start, end - start);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

Span Span::merge(const Span& a, const Span& b) {
    if (!a.valid()) return b;
    if (!b.valid()) return a;
    Span r = a.start <= b.start ? a : b;
    r.end = std::max(a.end, b.end);
    return r;
}

}  // namespace ember

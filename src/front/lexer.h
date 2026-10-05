// Lexer: bytes -> tokens. Never throws or crashes on malformed input; every
// problem becomes a diagnostic and an Error token, and lexing continues so
// that later errors are still reported.
#pragma once

#include <vector>

#include "common/diagnostics.h"
#include "front/token.h"

namespace ember {

struct Comment {
    Span span;
    std::string text;  // including the // or /* */ markers
};

class Lexer {
public:
    Lexer(const SourceFile& file, Diagnostics& diags);

    std::vector<Token> tokenize();
    // Comments seen during tokenize(), in order (used by the formatter).
    const std::vector<Comment>& comments() const { return comments_; }

private:
    Token next();
    void skip_trivia();
    Token number();
    Token string_lit();
    Token ident_or_keyword();
    Token make(Tok k, uint32_t start);
    Span span(uint32_t start, uint32_t end) const;
    void error(uint32_t start, uint32_t end, std::string msg);

    char peek(uint32_t ahead = 0) const {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }
    bool at_end() const { return pos_ >= src_.size(); }

    const SourceFile& file_;
    const std::string& src_;
    Diagnostics& diags_;
    uint32_t pos_ = 0;
    std::vector<Comment> comments_;
};

}  // namespace ember

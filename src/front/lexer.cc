#include "front/lexer.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace ember {

const char* tok_desc(Tok t) {
    switch (t) {
#define X(name, desc) \
    case Tok::name: return desc;
        EMBER_TOKENS(X)
#undef X
    }
    return "token";
}

namespace {

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }
int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const std::unordered_map<std::string_view, Tok>& keywords() {
    static const std::unordered_map<std::string_view, Tok> kw = {
        {"fn", Tok::KwFn},         {"let", Tok::KwLet},       {"var", Tok::KwVar},
        {"const", Tok::KwConst},   {"struct", Tok::KwStruct}, {"enum", Tok::KwEnum},
        {"match", Tok::KwMatch},   {"if", Tok::KwIf},         {"else", Tok::KwElse},
        {"while", Tok::KwWhile},   {"for", Tok::KwFor},       {"in", Tok::KwIn},
        {"break", Tok::KwBreak},   {"continue", Tok::KwContinue}, {"return", Tok::KwReturn},
        {"true", Tok::KwTrue},     {"false", Tok::KwFalse},   {"import", Tok::KwImport},
    };
    return kw;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

}  // namespace

Lexer::Lexer(const SourceFile& file, Diagnostics& diags)
    : file_(file), src_(file.text()), diags_(diags) {}

Span Lexer::span(uint32_t start, uint32_t end) const {
    Span s;
    s.start = start;
    s.end = end;
    s.line = file_.line_of(start);
    s.column = file_.column_of(start);
    return s;
}

void Lexer::error(uint32_t start, uint32_t end, std::string msg) {
    diags_.error(DiagKind::Lexical, span(start, end), std::move(msg));
}

Token Lexer::make(Tok k, uint32_t start) {
    Token t;
    t.kind = k;
    t.span = span(start, pos_);
    return t;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    for (;;) {
        Token t = next();
        bool eof = t.kind == Tok::Eof;
        out.push_back(std::move(t));
        if (eof) break;
    }
    return out;
}

void Lexer::skip_trivia() {
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++pos_;
        } else if (c == '/' && peek(1) == '/') {
            uint32_t start = pos_;
            while (!at_end() && peek() != '\n') ++pos_;
            comments_.push_back({span(start, pos_), src_.substr(start, pos_ - start)});
        } else if (c == '/' && peek(1) == '*') {
            uint32_t start = pos_;
            pos_ += 2;
            bool closed = false;
            while (!at_end()) {
                if (peek() == '*' && peek(1) == '/') {
                    pos_ += 2;
                    closed = true;
                    break;
                }
                ++pos_;
            }
            if (!closed) {
                error(start, start + 2, "unterminated block comment");
            } else {
                comments_.push_back({span(start, pos_), src_.substr(start, pos_ - start)});
            }
        } else {
            return;
        }
    }
}

Token Lexer::next() {
    skip_trivia();
    uint32_t start = pos_;
    if (at_end()) return make(Tok::Eof, start);
    char c = peek();
    if (is_ident_start(c)) return ident_or_keyword();
    if (is_digit(c)) return number();
    if (c == '"') return string_lit();
    ++pos_;
    auto two = [&](char second, Tok yes, Tok no) {
        if (peek() == second) {
            ++pos_;
            return make(yes, start);
        }
        return make(no, start);
    };
    switch (c) {
        case '+': return two('=', Tok::PlusEq, Tok::Plus);
        case '*': return two('=', Tok::StarEq, Tok::Star);
        case '/': return two('=', Tok::SlashEq, Tok::Slash);
        case '%': return two('=', Tok::PercentEq, Tok::Percent);
        case '!': return two('=', Tok::BangEq, Tok::Bang);
        case '<': return two('=', Tok::LtEq, Tok::Lt);
        case '>': return two('=', Tok::GtEq, Tok::Gt);
        case '-':
            if (peek() == '>') { ++pos_; return make(Tok::Arrow, start); }
            return two('=', Tok::MinusEq, Tok::Minus);
        case '=':
            if (peek() == '>') { ++pos_; return make(Tok::FatArrow, start); }
            return two('=', Tok::EqEq, Tok::Eq);
        case ':': return two(':', Tok::ColonColon, Tok::Colon);
        case '.': return two('.', Tok::DotDot, Tok::Dot);
        case '&':
            if (peek() == '&') { ++pos_; return make(Tok::AndAnd, start); }
            error(start, pos_, "unexpected character '&' (did you mean '&&'?)");
            return make(Tok::Error, start);
        case '|':
            if (peek() == '|') { ++pos_; return make(Tok::OrOr, start); }
            error(start, pos_, "unexpected character '|' (did you mean '||'?)");
            return make(Tok::Error, start);
        case ',': return make(Tok::Comma, start);
        case ';': return make(Tok::Semicolon, start);
        case '(': return make(Tok::LParen, start);
        case ')': return make(Tok::RParen, start);
        case '[': return make(Tok::LBracket, start);
        case ']': return make(Tok::RBracket, start);
        case '{': return make(Tok::LBrace, start);
        case '}': return make(Tok::RBrace, start);
        default: break;
    }
    // Consume a whole UTF-8 sequence so one stray character gives one error.
    if (static_cast<unsigned char>(c) >= 0x80) {
        while (!at_end() && (static_cast<unsigned char>(peek()) & 0xC0) == 0x80) ++pos_;
        error(start, pos_, "non-ASCII character outside a string or comment");
    } else {
        char buf[64];
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
            std::snprintf(buf, sizeof buf, "unexpected control character (byte 0x%02X)", static_cast<unsigned char>(c));
        } else {
            std::snprintf(buf, sizeof buf, "unexpected character '%c'", c);
        }
        error(start, pos_, buf);
    }
    return make(Tok::Error, start);
}

Token Lexer::ident_or_keyword() {
    uint32_t start = pos_;
    while (is_ident_char(peek())) ++pos_;
    std::string_view word(src_.data() + start, pos_ - start);
    if (word == "_") return make(Tok::Underscore, start);
    auto it = keywords().find(word);
    if (it != keywords().end()) return make(it->second, start);
    Token t = make(Tok::Ident, start);
    t.text = std::string(word);
    return t;
}

Token Lexer::number() {
    uint32_t start = pos_;
    bool hex = peek() == '0' && (peek(1) == 'x' || peek(1) == 'X');
    if (hex) {
        pos_ += 2;
        uint64_t v = 0;
        bool overflow = false, any = false, bad_sep = false;
        while (hex_val(peek()) >= 0 || peek() == '_') {
            char d = peek();
            ++pos_;
            if (d == '_') {
                if (!any || hex_val(peek()) < 0) bad_sep = true;
                continue;
            }
            any = true;
            if (v > (UINT64_MAX >> 4)) overflow = true;
            v = (v << 4) | static_cast<uint64_t>(hex_val(d));
        }
        while (is_ident_char(peek())) ++pos_;  // swallow e.g. 0x12g
        Token t = make(Tok::Int, start);
        std::string_view all(src_.data() + start, pos_ - start);
        bool trailing_garbage = false;
        for (size_t i = 2; i < all.size(); ++i)
            if (hex_val(all[i]) < 0 && all[i] != '_') trailing_garbage = true;
        if (!any || trailing_garbage) {
            error(start, pos_, "malformed hexadecimal literal '" + std::string(all) + "'");
            t.kind = Tok::Error;
        } else if (bad_sep) {
            error(start, pos_, "misplaced '_' in integer literal");
            t.kind = Tok::Error;
        } else if (overflow || v > static_cast<uint64_t>(INT64_MAX)) {
            error(start, pos_, "integer literal does not fit in 64 bits");
            t.kind = Tok::Error;
        } else {
            t.int_value = static_cast<int64_t>(v);
        }
        return t;
    }

    std::string digits;
    bool bad_sep = false;
    auto read_digits = [&]() {
        bool any = false;
        while (is_digit(peek()) || peek() == '_') {
            char d = peek();
            ++pos_;
            if (d == '_') {
                if (!any || !is_digit(peek())) bad_sep = true;
                continue;
            }
            any = true;
            digits += d;
        }
        return any;
    };
    read_digits();
    bool is_float = false;
    if (peek() == '.' && is_digit(peek(1))) {
        is_float = true;
        digits += '.';
        ++pos_;
        read_digits();
        if (peek() == 'e' || peek() == 'E') {
            uint32_t save = pos_;
            std::string save_digits = digits;
            digits += 'e';
            ++pos_;
            if (peek() == '+' || peek() == '-') digits += peek(), ++pos_;
            if (!read_digits()) {
                // "1.5e" with nothing after: report it rather than guess.
                pos_ = save + 1;
                error(start, pos_, "missing exponent digits in float literal");
                while (is_ident_char(peek())) ++pos_;
                Token t = make(Tok::Error, start);
                (void)save_digits;
                return t;
            }
        }
    }
    // Letters directly after a number (e.g. 12abc, 1.5f) are an error, not
    // a number followed by an identifier.
    if (is_ident_char(peek())) {
        while (is_ident_char(peek())) ++pos_;
        error(start, pos_, "malformed number '" + src_.substr(start, pos_ - start) + "'");
        return make(Tok::Error, start);
    }
    Token t = make(is_float ? Tok::Float : Tok::Int, start);
    if (bad_sep) {
        error(start, pos_, "misplaced '_' in number literal");
        t.kind = Tok::Error;
        return t;
    }
    if (is_float) {
        double v = 0;
        auto res = std::from_chars(digits.data(), digits.data() + digits.size(), v);
        if (res.ec != std::errc() || !std::isfinite(v)) {
            error(start, pos_, "float literal is out of range");
            t.kind = Tok::Error;
        } else {
            t.float_value = v;
        }
    } else {
        int64_t v = 0;
        auto res = std::from_chars(digits.data(), digits.data() + digits.size(), v);
        if (res.ec != std::errc()) {
            error(start, pos_, "integer literal does not fit in 64 bits");
            t.kind = Tok::Error;
        } else {
            t.int_value = v;
        }
    }
    return t;
}

Token Lexer::string_lit() {
    uint32_t start = pos_;
    ++pos_;  // opening quote
    std::string value;
    bool ok = true;
    for (;;) {
        if (at_end() || peek() == '\n') {
            error(start, pos_, "unterminated string literal");
            Token t = make(Tok::Error, start);
            return t;
        }
        char c = peek();
        if (c == '"') {
            ++pos_;
            break;
        }
        if (c != '\\') {
            value += c;
            ++pos_;
            continue;
        }
        uint32_t esc = pos_;
        ++pos_;
        char e = peek();
        if (at_end()) continue;  // reported as unterminated on next iteration
        ++pos_;
        switch (e) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case '0': value += '\0'; break;
            case '\\': value += '\\'; break;
            case '"': value += '"'; break;
            case 'x': {
                int h1 = hex_val(peek()), h2 = hex_val(peek(1));
                if (h1 < 0 || h2 < 0) {
                    error(esc, pos_, "invalid \\x escape: expected two hex digits");
                    ok = false;
                } else {
                    pos_ += 2;
                    value += static_cast<char>(h1 * 16 + h2);
                }
                break;
            }
            case 'u': {
                if (peek() != '{') {
                    error(esc, pos_, "invalid \\u escape: expected '{'");
                    ok = false;
                    break;
                }
                ++pos_;
                uint32_t cp = 0;
                int n = 0;
                while (hex_val(peek()) >= 0 && n < 7) {
                    cp = cp * 16 + static_cast<uint32_t>(hex_val(peek()));
                    ++pos_;
                    ++n;
                }
                if (peek() != '}' || n == 0 || n > 6) {
                    while (!at_end() && peek() != '}' && peek() != '"' && peek() != '\n') ++pos_;
                    if (peek() == '}') ++pos_;
                    error(esc, pos_, "invalid \\u escape: expected 1 to 6 hex digits in braces");
                    ok = false;
                    break;
                }
                ++pos_;
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    error(esc, pos_, "invalid \\u escape: not a Unicode scalar value");
                    ok = false;
                    break;
                }
                append_utf8(value, cp);
                break;
            }
            default: {
                std::string msg = "unknown escape sequence '\\";
                if (static_cast<unsigned char>(e) >= 0x20 && static_cast<unsigned char>(e) < 0x7F) msg += e;
                msg += "'";
                error(esc, pos_, msg);
                ok = false;
            }
        }
    }
    Token t = make(ok ? Tok::String : Tok::Error, start);
    t.text = std::move(value);
    return t;
}

}  // namespace ember

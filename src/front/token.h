#pragma once

#include <cstdint>
#include <string>

#include "common/source.h"

namespace ember {

#define EMBER_TOKENS(X)                                                        \
    X(Eof, "end of file")                                                      \
    X(Error, "invalid token")                                                  \
    X(Ident, "identifier")                                                     \
    X(Int, "integer literal")                                                  \
    X(Float, "float literal")                                                  \
    X(String, "string literal")                                                \
    X(KwFn, "'fn'") X(KwLet, "'let'") X(KwVar, "'var'") X(KwConst, "'const'")  \
    X(KwStruct, "'struct'") X(KwEnum, "'enum'") X(KwMatch, "'match'")          \
    X(KwIf, "'if'") X(KwElse, "'else'") X(KwWhile, "'while'")                  \
    X(KwFor, "'for'") X(KwIn, "'in'") X(KwBreak, "'break'")                    \
    X(KwContinue, "'continue'") X(KwReturn, "'return'") X(KwTrue, "'true'")    \
    X(KwFalse, "'false'") X(KwImport, "'import'")                              \
    X(Underscore, "'_'")                                                       \
    X(Plus, "'+'") X(Minus, "'-'") X(Star, "'*'") X(Slash, "'/'")              \
    X(Percent, "'%'") X(EqEq, "'=='") X(BangEq, "'!='") X(Lt, "'<'")           \
    X(LtEq, "'<='") X(Gt, "'>'") X(GtEq, "'>='") X(AndAnd, "'&&'")             \
    X(OrOr, "'||'") X(Bang, "'!'") X(Eq, "'='") X(PlusEq, "'+='")              \
    X(MinusEq, "'-='") X(StarEq, "'*='") X(SlashEq, "'/='")                    \
    X(PercentEq, "'%='") X(Arrow, "'->'") X(FatArrow, "'=>'")                  \
    X(ColonColon, "'::'") X(Dot, "'.'") X(DotDot, "'..'") X(Comma, "','")      \
    X(Semicolon, "';'") X(Colon, "':'") X(LParen, "'('") X(RParen, "')'")      \
    X(LBracket, "'['") X(RBracket, "']'") X(LBrace, "'{'") X(RBrace, "'}'")

enum class Tok : uint8_t {
#define X(name, desc) name,
    EMBER_TOKENS(X)
#undef X
};

const char* tok_desc(Tok t);

struct Token {
    Tok kind = Tok::Eof;
    Span span;
    std::string text;   // identifier name, or decoded string literal bytes
    int64_t int_value = 0;
    double float_value = 0;
};

}  // namespace ember

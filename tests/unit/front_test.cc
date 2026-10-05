// Lexer, parser, resolver and type checker tests.
#include <random>

#include "common/diagnostics.h"
#include "front/lexer.h"
#include "front/parser.h"
#include "helpers.h"
#include "test.h"
#include "tools/ast_print.h"

using namespace ember;
using th::compile_errors;

namespace {

std::vector<Token> lex(const std::string& s, Diagnostics& d) {
    static std::vector<std::unique_ptr<SourceFile>> keep;  // tokens hold views into text
    keep.push_back(std::make_unique<SourceFile>("t.em", s));
    Lexer l(*keep.back(), d);
    return l.tokenize();
}

std::string parse_dump(const std::string& expr_src) {
    Diagnostics d;
    SourceFile f("t.em", "fn main() { let x = " + expr_src + "; }");
    Lexer l(f, d);
    Parser p(l.tokenize(), d);
    Program prog = p.parse_program();
    if (d.has_errors()) return "ERROR " + d.render(f);
    return print_ast(prog, false);
}

}  // namespace

TEST(lexer_numbers) {
    Diagnostics d;
    auto toks = lex("0 42 0x1F 1_000 3.25 1.0e10 2.5E-3", d);
    CHECK(!d.has_errors());
    CHECK_EQ(toks[0].int_value, 0);
    CHECK_EQ(toks[1].int_value, 42);
    CHECK_EQ(toks[2].int_value, 31);
    CHECK_EQ(toks[3].int_value, 1000);
    CHECK_EQ(toks[4].float_value, 3.25);
    CHECK_EQ(toks[5].float_value, 1.0e10);
    CHECK_EQ(toks[6].float_value, 2.5e-3);
}

TEST(lexer_rejects_bad_numbers) {
    for (const char* bad : {"1__0", "1_", "0x", "1e5", "99999999999999999999", "0xFFFFFFFFFFFFFFFFF", "1.0e999"}) {
        Diagnostics d;
        lex(bad, d);
        CHECK(d.has_errors());
    }
}

TEST(lexer_string_escapes) {
    Diagnostics d;
    auto toks = lex(R"("a\nb\t\"\\\x41\u{263A}\0")", d);
    CHECK(!d.has_errors());
    CHECK_EQ(toks[0].text, std::string("a\nb\t\"\\A\xE2\x98\xBA") + std::string(1, '\0'));
}

TEST(lexer_errors_continue) {
    Diagnostics d;
    auto toks = lex("let x = \"unterminated\nlet y = 2 $ 3;", d);
    CHECK(d.error_count() >= 2);
    CHECK(toks.back().kind == Tok::Eof);
}

TEST(lexer_comments_recorded) {
    Diagnostics d;
    SourceFile f("t.em", "// one\nfn /* two */ main() {}\n");
    Lexer l(f, d);
    l.tokenize();
    CHECK_EQ(l.comments().size(), size_t(2));
    CHECK_EQ(l.comments()[1].text, std::string("/* two */"));
}

TEST(lexer_unterminated_block_comment) {
    Diagnostics d;
    lex("/* never closed", d);
    CHECK(d.has_errors());
}

TEST(parser_precedence) {
    std::string a = parse_dump("1 + 2 * 3");
    CHECK_CONTAINS(a, "Binary +\n        Int 1\n        Binary *");
    std::string b = parse_dump("a || b && c");
    CHECK_CONTAINS(b, "Or\n        Ident a\n        And");
    std::string c = parse_dump("-x.f[0]");
    CHECK_CONTAINS(c, "Neg\n        Index\n          Field .f");
    std::string d = parse_dump("1 - 2 - 3");  // left associative
    CHECK_CONTAINS(d, "Binary -\n        Binary -\n          Int 1");
}

TEST(parser_recovers_and_reports_several_errors) {
    std::string err = compile_errors("fn main() {\n let = 1;\n let y = ;\n print(1)\n}\n");
    size_t n = 0;
    for (size_t p = err.find("error["); p != std::string::npos; p = err.find("error[", p + 1)) ++n;
    CHECK(n >= 3);
}

TEST(parser_depth_limit) {
    std::string deep(10000, '[');
    std::string err = compile_errors("fn main() { let x = " + deep + "; }");
    CHECK_CONTAINS(err, "nesting is too deep");
    std::string blocks;
    for (int i = 0; i < 5000; ++i) blocks += "{";
    CHECK_CONTAINS(compile_errors("fn main() " + blocks), "nesting is too deep");
}

TEST(parser_struct_literal_in_condition_needs_parens) {
    std::string src = "struct P { x: Int }\nfn main() {\n if (P { x: 1 }).x == 1 { print(1); }\n}\n";
    CHECK_EQ(compile_errors(src), std::string());
}

TEST(parser_never_crashes_on_random_input) {
    std::mt19937 rng(7);
    const char alphabet[] = "fnletvar(){}[];:,.=+-*/%<>!&|\"' \n0123456789abcxyz_matchif else while for in return";
    for (int i = 0; i < 3000; ++i) {
        std::string s;
        int n = static_cast<int>(rng() % 200);
        for (int k = 0; k < n; ++k) s += alphabet[rng() % (sizeof(alphabet) - 1)];
        auto c = compile("fuzz.em", s, CompileOptions{});
        CHECK(!c->internal_error);
    }
}

TEST(sema_errors_have_categories) {
    CHECK_CONTAINS(compile_errors("fn main() { print(nope); }"), "error[name]");
    CHECK_CONTAINS(compile_errors("fn main() { let x: Int = true; }"), "error[type]");
    CHECK_CONTAINS(compile_errors("fn main() { let x = 1 $ 2; }"), "error[lexical]");
    CHECK_CONTAINS(compile_errors("fn main() { let x = ; }"), "error[syntax]");
    CHECK_CONTAINS(compile_errors("import nope;\nfn main() {}"), "error[module]");
}

TEST(sema_const_folding_matches_runtime) {
    auto r = th::run_source("const A: Int = 7 / -2;\nconst B: Int = -7 % 3;\nconst C: Float = 1.0 / 3.0;\n"
                            "const D: String = \"a\" + \"b\";\nfn main() { print(A); print(B); print(C); print(D); }");
    CHECK(r.compiled);
    CHECK_EQ(r.out, std::string("-3\n-1\n0.3333333333333333\nab\n"));
}

TEST(sema_const_overflow_is_compile_error) {
    CHECK_CONTAINS(compile_errors("const A: Int = 9223372036854775807 + 1;\nfn main() {}"), "overflow");
    CHECK_CONTAINS(compile_errors("const A: Int = 1 / 0;\nfn main() {}"), "division by zero");
}

TEST(sema_match_exhaustiveness) {
    CHECK_CONTAINS(compile_errors("fn main() { let n = match 1 { 1 => 2 }; }"), "non-exhaustive");
    CHECK_CONTAINS(compile_errors("enum E { A, B }\nfn main() { let n = match E::A { E::A => 1, E::A => 2, E::B => 3 }; }"),
                   "unreachable");
    CHECK_EQ(compile_errors("fn main() { let n = match true { true => 1, false => 0 }; print(n); }"), std::string());
}

TEST(sema_return_paths) {
    CHECK_EQ(compile_errors("fn f(x: Int) -> Int { if x > 0 { return 1; } else { return 2; } }\nfn main() {}"), std::string());
    CHECK_EQ(compile_errors("fn f() -> Int { while true { } }\nfn main() {}"), std::string());
    CHECK_CONTAINS(compile_errors("fn f() -> Int { while true { break; } }\nfn main() {}"), "without returning");
}

TEST(sema_closure_capture_of_loop_variable_is_fresh) {
    auto r = th::run_source("fn main() { var fs: [fn() -> Int] = []; for i in 0..3 { push(fs, fn() -> Int { return i; }); }"
                            " for f in fs { print(f()); } }");
    CHECK_EQ(r.out, std::string("0\n1\n2\n"));
}

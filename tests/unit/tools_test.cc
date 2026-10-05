// Formatter, reference interpreter and driver tests.
#include <sstream>

#include "driver/driver.h"
#include "helpers.h"
#include "test.h"
#include "tools/formatter.h"
#include "tools/progen.h"
#include "tools/refinterp.h"

using namespace ember;

TEST(formatter_preserves_comments) {
    std::string src = "// header\nfn main() {\n    // leading\n    let x = 1; // trailing\n    /* block */\n    print(x);\n    // last\n}\n";
    auto r = format_source("t.em", src);
    CHECK(r.ok);
    CHECK_EQ(r.output, src);
}

TEST(formatter_normalizes) {
    auto r = format_source("t.em", "fn main(){let x=(1+2)*3-(4-5);if (x>1){print(x);}else if x<0{print(0);}}");
    CHECK(r.ok);
    CHECK_EQ(r.output, std::string("fn main() {\n    let x = (1 + 2) * 3 - (4 - 5);\n    if x > 1 {\n        print(x);\n    } else if x < 0 {\n"
                                   "        print(0);\n    }\n}\n"));
}

TEST(formatter_keeps_needed_parentheses) {
    auto r = format_source("t.em", "fn main() { print(1 - (2 - 3)); print((1 + 2) * 3); print(-(-1)); print(!(true && false)); }");
    CHECK(r.ok);
    CHECK_CONTAINS(r.output, "1 - (2 - 3)");
    CHECK_CONTAINS(r.output, "(1 + 2) * 3");
    CHECK_CONTAINS(r.output, "- -1");
    CHECK_CONTAINS(r.output, "!(true && false)");
}

TEST(formatter_reports_syntax_errors) {
    auto r = format_source("t.em", "fn main( {");
    CHECK(!r.ok);
    CHECK_CONTAINS(r.diagnostics, "error[syntax]");
}

TEST(formatter_on_generated_programs_is_idempotent_and_preserves_behaviour) {
    for (uint64_t seed = 1; seed <= 60; ++seed) {
        std::string src = generate_program(seed);
        auto f1 = format_source("g.em", src);
        CHECK(f1.ok);
        auto f2 = format_source("g.em", f1.output);
        CHECK(f2.ok);
        CHECK(f1.output == f2.output);
        auto a = th::run_source(src);
        auto b = th::run_source(f1.output);
        CHECK(a.compiled && b.compiled);
        CHECK(a.out == b.out);
        CHECK(a.result.message == b.result.message);
    }
}

TEST(reference_interpreter_agrees_on_basics) {
    const char* src = "fn fib(n: Int) -> Int { if n < 2 { return n; } return fib(n - 1) + fib(n - 2); }\n"
                      "fn main() { print(fib(15)); let xs = [3, 1, 2]; xs[0] = 9; print(xs); print(10 / 0); }";
    auto c = compile("t.em", src, CompileOptions{});
    std::ostringstream out;
    auto r = run_reference(c->program, c->sema, out, RefOptions{});
    CHECK_EQ(out.str(), std::string("610\n[9, 1, 2]\n"));
    CHECK(!r.ok);
    CHECK_EQ(r.error, std::string("division by zero"));
}

TEST(driver_reports_size_limit) {
    CompileOptions o;
    o.max_source_bytes = 10;
    auto c = compile("t.em", "fn main() { print(1); }", o);
    CHECK(!c->ok);
    CHECK_CONTAINS(c->diagnostics, "too large");
}

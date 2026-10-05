// Parser: tokens -> AST. Recursive descent for items and statements, Pratt
// parsing for expressions. On a syntax error it reports a diagnostic and
// resynchronizes at the next statement/item boundary, so one mistake does not
// hide the rest. Nesting depth is bounded so that adversarial input cannot
// overflow the C++ stack.
#pragma once

#include <vector>

#include "common/diagnostics.h"
#include "front/ast.h"
#include "front/token.h"

namespace ember {

class Parser {
public:
    static constexpr int kMaxDepth = 256;

    Parser(std::vector<Token> tokens, Diagnostics& diags);
    Program parse_program();

    // Entry points used by tests and the REPL.
    ExprPtr parse_expression_only();

private:
    struct SyntaxError {};  // thrown to unwind to the nearest sync point

    // Items and statements
    void parse_item(Program& prog);
    std::unique_ptr<FnDecl> parse_fn(bool lambda);
    std::unique_ptr<StructDecl> parse_struct();
    std::unique_ptr<EnumDecl> parse_enum();
    std::unique_ptr<ConstDecl> parse_const();
    std::unique_ptr<LetStmt> parse_let();
    std::unique_ptr<Block> parse_block();
    StmtPtr parse_stmt();
    StmtPtr parse_if();
    StmtPtr parse_for();
    std::unique_ptr<TypeExpr> parse_type();

    // Expressions
    ExprPtr parse_expr(int min_prec = 0);
    ExprPtr parse_unary();
    ExprPtr parse_postfix(ExprPtr e);
    ExprPtr parse_primary();
    ExprPtr parse_match();
    Pattern parse_pattern();
    std::vector<ExprPtr> parse_args();

    // Token helpers
    const Token& peek(int ahead = 0) const;
    const Token& advance();
    bool check(Tok k) const { return peek().kind == k; }
    bool match(Tok k);
    const Token& expect(Tok k, const char* context);
    [[noreturn]] void fail(const Token& at, const std::string& msg, std::vector<std::string> notes = {});
    void sync_stmt();
    void sync_item();

    struct DepthGuard {
        Parser& p;
        explicit DepthGuard(Parser& parser);
        ~DepthGuard() { --p.depth_; }
    };

    std::vector<Token> toks_;
    size_t pos_ = 0;
    Diagnostics& diags_;
    int depth_ = 0;
    bool no_struct_lit_ = false;
};

}  // namespace ember

// Static type checking. Runs after name resolution. Assigns a Type to every
// expression and symbol, folds `const` declarations, checks return paths and
// match exhaustiveness. Uses the Error type to stop one mistake cascading
// into many reports.
#pragma once

#include "common/diagnostics.h"
#include "sema/types.h"

namespace ember {

class TypeChecker {
public:
    TypeChecker(Program& prog, SemaResult& sema, Diagnostics& diags);
    void run();

private:
    Type* resolve_type(const TypeExpr* t);
    void declare_fn_signature(FnDecl& fn);
    void check_fn_body(FnDecl& fn);
    void check_block(Block& b);
    void check_stmt(Stmt& s);
    Type* check_expr(Expr& e, Type* expected = nullptr);
    Type* check_call(CallExpr& c);
    Type* check_builtin(CallExpr& c, Builtin b);
    Type* check_module_call(ModuleCallExpr& c);
    Type* check_binary(BinaryExpr& b);
    Type* check_match(MatchExpr& m, Type* expected);
    void check_assign_target(Expr& target);
    void fold_const(ConstDecl& c);
    bool definitely_returns(const Block& b) const;
    bool stmt_returns(const Stmt& s) const;

    // expect: report a mismatch unless `found` is assignable to `want`
    bool expect(Type* want, Type* found, Span span, const std::string& what);
    void error(Span span, std::string msg, std::vector<std::string> notes = {}) {
        diags_.error(DiagKind::Type, span, std::move(msg), std::move(notes));
    }

    Program& prog_;
    SemaResult& sema_;
    TypeContext& types_;
    Diagnostics& diags_;
    Type* current_ret_ = nullptr;
};

}  // namespace ember

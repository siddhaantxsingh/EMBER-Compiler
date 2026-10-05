// Name resolution: binds every identifier to a Symbol, builds per-function
// local lists and closure capture tables, and reports scope errors
// (undefined or duplicate names, break/continue outside loops, assignment to
// immutable bindings). It does not look at types.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "common/diagnostics.h"
#include "sema/types.h"

namespace ember {

// Native functions of the std modules. Shared by the resolver (to recognise
// math.sqrt etc.), the type checker (signatures) and the runtime.
struct NativeSig {
    const char* module;
    const char* name;
    std::vector<TypeKind> params;  // only scalar/String/array-of-String are used
    TypeKind ret;
    bool ret_string_array = false;      // strings.split
    bool param0_string_array = false;   // strings.join
};
const std::vector<NativeSig>& native_table();
int find_native(const std::string& module, const std::string& name);
bool is_std_module(const std::string& name);

class Resolver {
public:
    Resolver(Program& prog, SemaResult& out, Diagnostics& diags);
    void run();

private:
    struct Scope {
        std::unordered_map<std::string, Symbol*> names;
    };

    Symbol* new_symbol(SymbolKind k, const std::string& name, Span span);
    void declare_top_level();
    void declare(Symbol* sym);
    Symbol* lookup(const std::string& name) const;
    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }

    FnInfo* begin_function(FnDecl& fn, const std::string& display_name);
    void resolve_function_body(FnDecl& fn, FnInfo* info);
    void resolve_block(Block& b, bool new_scope = true);
    void resolve_stmt(Stmt& s);
    void resolve_expr(ExprPtr& e);
    void resolve_ident(IdentExpr& id);
    void resolve_place(Expr& target);
    void capture(Symbol* sym);
    Symbol* declare_local(const std::string& name, Span span, bool mutable_);

    Program& prog_;
    SemaResult& out_;
    Diagnostics& diags_;
    std::vector<Scope> scopes_;  // [0] = globals
    std::unordered_map<std::string, StructDecl*> structs_;
    std::unordered_map<std::string, EnumDecl*> enums_;
    std::unordered_map<std::string, bool> imports_;
    FnInfo* current_fn_ = nullptr;
    int loop_depth_ = 0;
    bool in_global_init_ = false;
    int global_init_index_ = -1;
};

}  // namespace ember

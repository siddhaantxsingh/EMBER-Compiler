#include "sema/resolver.h"

namespace ember {

const std::vector<NativeSig>& native_table() {
    using K = TypeKind;
    static const std::vector<NativeSig> table = {
        {"math", "abs", {K::Int}, K::Int},
        {"math", "fabs", {K::Float}, K::Float},
        {"math", "min", {K::Int, K::Int}, K::Int},
        {"math", "max", {K::Int, K::Int}, K::Int},
        {"math", "sqrt", {K::Float}, K::Float},
        {"math", "pow", {K::Float, K::Float}, K::Float},
        {"math", "floor", {K::Float}, K::Float},
        {"math", "ceil", {K::Float}, K::Float},
        {"strings", "substr", {K::String, K::Int, K::Int}, K::String},
        {"strings", "find", {K::String, K::String}, K::Int},
        {"strings", "upper", {K::String}, K::String},
        {"strings", "lower", {K::String}, K::String},
        {"strings", "split", {K::String, K::String}, K::Array, true},
        {"strings", "join", {K::Array, K::String}, K::String, false, true},
        {"strings", "char_code", {K::String, K::Int}, K::Int},
        {"strings", "from_char_code", {K::Int}, K::String},
        {"strings", "repeat", {K::String, K::Int}, K::String},
        {"time", "clock", {}, K::Float},
    };
    return table;
}

int find_native(const std::string& module, const std::string& name) {
    const auto& t = native_table();
    for (size_t i = 0; i < t.size(); ++i)
        if (module == t[i].module && name == t[i].name) return static_cast<int>(i);
    return -1;
}

bool is_std_module(const std::string& name) { return name == "math" || name == "strings" || name == "time"; }

namespace {
struct BuiltinName {
    const char* name;
    Builtin b;
};
const BuiltinName kBuiltins[] = {
    {"print", Builtin::Print}, {"input", Builtin::Input}, {"len", Builtin::Len},
    {"push", Builtin::Push},   {"pop", Builtin::Pop},     {"str", Builtin::Str},
    {"int", Builtin::Int},     {"float", Builtin::Float}, {"parse_int", Builtin::ParseInt},
    {"panic", Builtin::Panic}, {"assert", Builtin::Assert},
};
}  // namespace

Resolver::Resolver(Program& prog, SemaResult& out, Diagnostics& diags) : prog_(prog), out_(out), diags_(diags) {}

Symbol* Resolver::new_symbol(SymbolKind k, const std::string& name, Span span) {
    auto s = std::make_unique<Symbol>();
    s->kind = k;
    s->name = name;
    s->span = span;
    Symbol* p = s.get();
    out_.symbols.push_back(std::move(s));
    return p;
}

void Resolver::declare(Symbol* sym) {
    auto& names = scopes_.back().names;
    auto it = names.find(sym->name);
    if (it != names.end() && it->second->kind != SymbolKind::Builtin) {
        diags_.error(DiagKind::Name, sym->span, "'" + sym->name + "' is already declared in this scope",
                     {"previous declaration at line " + std::to_string(it->second->span.line)});
        return;
    }
    names[sym->name] = sym;
}

Symbol* Resolver::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->names.find(name);
        if (f != it->names.end()) return f->second;
    }
    return nullptr;
}

void Resolver::declare_top_level() {
    scopes_.emplace_back();  // builtins
    for (const auto& b : kBuiltins) {
        Symbol* s = new_symbol(SymbolKind::Builtin, b.name, Span{});
        s->builtin = b.b;
        scopes_.back().names[b.name] = s;
    }
    scopes_.emplace_back();  // globals
    for (auto& item : prog_.items) {
        switch (item.kind) {
            case ItemKind::Struct: {
                auto* d = item.struct_decl.get();
                if (structs_.count(d->name) || enums_.count(d->name)) {
                    diags_.error(DiagKind::Name, d->name_span, "type '" + d->name + "' is already declared");
                    break;
                }
                d->index = static_cast<int>(out_.structs.size());
                out_.structs.push_back(d);
                structs_[d->name] = d;
                break;
            }
            case ItemKind::Enum: {
                auto* d = item.enum_decl.get();
                if (structs_.count(d->name) || enums_.count(d->name)) {
                    diags_.error(DiagKind::Name, d->name_span, "type '" + d->name + "' is already declared");
                    break;
                }
                d->index = static_cast<int>(out_.enums.size());
                out_.enums.push_back(d);
                enums_[d->name] = d;
                break;
            }
            case ItemKind::Fn: {
                Symbol* s = new_symbol(SymbolKind::Function, item.fn->name, item.fn->name_span);
                s->fn = item.fn.get();
                item.fn->sym = s;
                declare(s);
                break;
            }
            case ItemKind::Import: {
                const std::string& m = item.import->module;
                if (!is_std_module(m)) {
                    diags_.error(DiagKind::Module, item.import->span, "unknown module '" + m + "'",
                                 {"available modules: math, strings, time"});
                } else if (imports_.count(m)) {
                    diags_.error(DiagKind::Module, item.import->span, "module '" + m + "' is imported twice");
                } else {
                    imports_[m] = true;
                    out_.imports.push_back(m);
                }
                break;
            }
            case ItemKind::Const: {
                auto* c = item.const_decl.get();
                Symbol* s = new_symbol(SymbolKind::Const, c->name, c->name_span);
                s->const_decl = c;
                c->sym = s;
                declare(s);
                break;
            }
            case ItemKind::Global: {
                auto* g = item.global.get();
                Symbol* s = new_symbol(SymbolKind::Global, g->name, g->name_span);
                s->mutable_ = g->mutable_;
                s->global_index = static_cast<int>(out_.globals.size());
                out_.globals.push_back(s);
                g->sym = s;
                declare(s);
                break;
            }
        }
    }
}

FnInfo* Resolver::begin_function(FnDecl& fn, const std::string& display_name) {
    auto info = std::make_unique<FnInfo>();
    info->decl = &fn;
    info->parent = current_fn_;
    info->display_name = display_name;
    info->index = static_cast<int>(out_.functions.size());
    fn.info = info.get();
    FnInfo* p = info.get();
    out_.functions.push_back(std::move(info));
    return p;
}

Symbol* Resolver::declare_local(const std::string& name, Span span, bool mutable_) {
    Symbol* s = new_symbol(SymbolKind::Local, name, span);
    s->mutable_ = mutable_;
    s->owner = current_fn_;
    current_fn_->locals.push_back(s);
    declare(s);
    return s;
}

void Resolver::run() {
    declare_top_level();

    // A synthetic function holds global initializers.
    auto init = std::make_unique<FnInfo>();
    init->display_name = "<globals>";
    FnInfo* init_info = init.get();
    // Index is assigned after all real functions so that function indices
    // follow source order; the lowering stage handles the init function.
    std::unique_ptr<FnInfo> init_owner = std::move(init);

    // Top-level functions first so that their FnInfos come first.
    for (auto& item : prog_.items) {
        if (item.kind == ItemKind::Fn) begin_function(*item.fn, item.fn->name);
    }
    for (auto& item : prog_.items) {
        if (diags_.full()) break;
        switch (item.kind) {
            case ItemKind::Fn:
                resolve_function_body(*item.fn, item.fn->info);
                if (item.fn->name == "main") out_.main = item.fn->info;
                break;
            case ItemKind::Const: {
                current_fn_ = init_info;
                resolve_expr(item.const_decl->value);
                current_fn_ = nullptr;
                break;
            }
            case ItemKind::Global: {
                auto* g = item.global.get();
                current_fn_ = init_info;
                in_global_init_ = true;
                global_init_index_ = g->sym ? g->sym->global_index : 0;
                resolve_expr(g->init);
                in_global_init_ = false;
                current_fn_ = nullptr;
                break;
            }
            default: break;
        }
    }
    // Globals are visible in every function regardless of declaration
    // order. Functions normally run after all initializers; one called from
    // an initializer may read a global that is not initialized yet, which
    // the runtime reports as 'uninitialized global'.
    init_info->index = static_cast<int>(out_.functions.size());
    out_.functions.push_back(std::move(init_owner));

    if (!out_.main) {
        diags_.error(DiagKind::Name, Span{}, "program has no 'main' function", {"add: fn main() { ... }"});
    }
}

void Resolver::resolve_function_body(FnDecl& fn, FnInfo* info) {
    FnInfo* saved_fn = current_fn_;
    int saved_loop = loop_depth_;
    bool saved_init = in_global_init_;
    current_fn_ = info;
    loop_depth_ = 0;
    in_global_init_ = false;
    push_scope();
    for (auto& p : fn.params) p.sym = declare_local(p.name, p.span, false);
    resolve_block(*fn.body, false);
    pop_scope();
    current_fn_ = saved_fn;
    loop_depth_ = saved_loop;
    in_global_init_ = saved_init;
}

void Resolver::resolve_block(Block& b, bool new_scope) {
    if (new_scope) push_scope();
    // Nested function declarations in a block are bound before the block's
    // statements are resolved, so mutually recursive nested functions work
    // regardless of order (each still captures by reference).
    for (auto& s : b.stmts) {
        if (s->kind == StmtKind::Fn) {
            auto& fs = static_cast<FnStmt&>(*s);
            fs.fn->sym = declare_local(fs.fn->name, fs.fn->name_span, false);
            fs.fn->sym->fn = fs.fn.get();
        }
    }
    for (auto& s : b.stmts) {
        if (diags_.full()) break;
        resolve_stmt(*s);
    }
    if (new_scope) pop_scope();
}

void Resolver::resolve_stmt(Stmt& s) {
    switch (s.kind) {
        case StmtKind::Let: {
            auto& l = static_cast<LetStmt&>(s);
            resolve_expr(l.init);  // initializer cannot see the new name
            l.sym = declare_local(l.name, l.name_span, l.mutable_);
            break;
        }
        case StmtKind::Fn: {
            auto& f = static_cast<FnStmt&>(s);
            std::string name = current_fn_->display_name + "." + f.fn->name;
            FnInfo* info = begin_function(*f.fn, name);
            resolve_function_body(*f.fn, info);
            break;
        }
        case StmtKind::If: {
            auto& i = static_cast<IfStmt&>(s);
            resolve_expr(i.cond);
            resolve_block(*i.then_block);
            if (i.else_stmt) resolve_stmt(*i.else_stmt);
            break;
        }
        case StmtKind::While: {
            auto& w = static_cast<WhileStmt&>(s);
            resolve_expr(w.cond);
            ++loop_depth_;
            resolve_block(*w.body);
            --loop_depth_;
            break;
        }
        case StmtKind::ForRange: {
            auto& f = static_cast<ForRangeStmt&>(s);
            resolve_expr(f.start);
            resolve_expr(f.end);
            push_scope();
            f.sym = declare_local(f.var, f.var_span, false);
            ++loop_depth_;
            resolve_block(*f.body);
            --loop_depth_;
            pop_scope();
            break;
        }
        case StmtKind::ForEach: {
            auto& f = static_cast<ForEachStmt&>(s);
            resolve_expr(f.iterable);
            push_scope();
            f.sym = declare_local(f.var, f.var_span, false);
            ++loop_depth_;
            resolve_block(*f.body);
            --loop_depth_;
            pop_scope();
            break;
        }
        case StmtKind::Break:
        case StmtKind::Continue:
            if (loop_depth_ == 0) {
                diags_.error(DiagKind::Name, s.span,
                             std::string("'") + (s.kind == StmtKind::Break ? "break" : "continue") +
                                 "' outside of a loop");
            }
            break;
        case StmtKind::Return: {
            auto& r = static_cast<ReturnStmt&>(s);
            if (r.value) resolve_expr(r.value);
            break;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<AssignStmt&>(s);
            resolve_expr(a.target);
            resolve_place(*a.target);
            resolve_expr(a.value);
            break;
        }
        case StmtKind::ExprStmt: resolve_expr(static_cast<ExprStmt&>(s).expr); break;
        case StmtKind::BlockStmt: resolve_block(*static_cast<BlockStmt&>(s).block); break;
    }
}

void Resolver::resolve_place(Expr& target) {
    if (target.kind != ExprKind::Ident) return;  // a[i] and s.f are always assignable
    auto& id = static_cast<IdentExpr&>(target);
    Symbol* s = id.sym;
    if (!s) return;
    if (s->kind == SymbolKind::Local || s->kind == SymbolKind::Global) {
        if (!s->mutable_) {
            diags_.error(DiagKind::Name, id.span, "cannot assign to immutable binding '" + id.name + "'",
                         {"declared at line " + std::to_string(s->span.line) + "; use 'var' to make it mutable"});
        }
    } else {
        diags_.error(DiagKind::Name, id.span, "cannot assign to '" + id.name + "'",
                     {"only variables declared with 'var' can be assigned"});
    }
}

void Resolver::capture(Symbol* sym) {
    // Make every function between the current one and the owner capture
    // the symbol, outermost first.
    std::vector<FnInfo*> chain;
    for (FnInfo* f = current_fn_; f && f != sym->owner; f = f->parent) chain.push_back(f);
    if (chain.empty()) return;
    if (!chain.back()->parent || chain.back()->parent != sym->owner) return;  // not reachable; defensive
    sym->captured = true;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        FnInfo* f = *it;
        if (f->capture_index(sym) >= 0) continue;
        Capture c;
        c.sym = sym;
        c.from_parent_local = f->parent == sym->owner;
        c.parent_upvalue_index = c.from_parent_local ? -1 : f->parent->capture_index(sym);
        f->captures.push_back(c);
    }
}

void Resolver::resolve_ident(IdentExpr& id) {
    Symbol* s = lookup(id.name);
    if (!s) {
        std::vector<std::string> notes;
        if (structs_.count(id.name) || enums_.count(id.name))
            notes.push_back("'" + id.name + "' is a type, not a value");
        if (imports_.count(id.name) || is_std_module(id.name))
            notes.push_back("modules can only be used as module.function(...)");
        diags_.error(DiagKind::Name, id.span, "undefined name '" + id.name + "'", notes);
        return;
    }
    if (s->kind == SymbolKind::Builtin) {
        // Only valid as a callee; checked by the caller.
        id.sym = s;
        return;
    }
    if (s->kind == SymbolKind::Global && in_global_init_ && current_fn_ && current_fn_->decl == nullptr &&
        s->global_index >= global_init_index_) {
        diags_.error(DiagKind::Name, id.span,
                     s->global_index == global_init_index_
                         ? "global '" + id.name + "' is used in its own initializer"
                         : "global '" + id.name + "' is used before its declaration",
                     {"globals are initialized in source order"});
    }
    if (s->kind == SymbolKind::Local && s->owner != current_fn_) capture(s);
    id.sym = s;
}

void Resolver::resolve_expr(ExprPtr& ep) {
    Expr& e = *ep;
    switch (e.kind) {
        case ExprKind::IntLit: case ExprKind::FloatLit: case ExprKind::BoolLit:
        case ExprKind::StringLit: case ExprKind::UnitLit:
            break;
        case ExprKind::Ident: {
            auto& id = static_cast<IdentExpr&>(e);
            resolve_ident(id);
            if (id.sym && id.sym->kind == SymbolKind::Builtin) {
                diags_.error(DiagKind::Name, id.span, "builtin '" + id.name + "' can only be called, not used as a value");
            }
            break;
        }
        case ExprKind::Unary: resolve_expr(static_cast<UnaryExpr&>(e).operand); break;
        case ExprKind::Binary: {
            auto& b = static_cast<BinaryExpr&>(e);
            resolve_expr(b.lhs);
            resolve_expr(b.rhs);
            break;
        }
        case ExprKind::Logical: {
            auto& l = static_cast<LogicalExpr&>(e);
            resolve_expr(l.lhs);
            resolve_expr(l.rhs);
            break;
        }
        case ExprKind::Call: {
            auto& c = static_cast<CallExpr&>(e);
            // module.function(args) where `module` is an imported std module
            // that no variable shadows.
            if (c.callee->kind == ExprKind::Field) {
                auto& fe = static_cast<FieldExpr&>(*c.callee);
                if (fe.object->kind == ExprKind::Ident) {
                    auto& obj = static_cast<IdentExpr&>(*fe.object);
                    if (!lookup(obj.name) && (imports_.count(obj.name) || is_std_module(obj.name))) {
                        auto mc = std::make_unique<ModuleCallExpr>(c.span);
                        mc->module = obj.name;
                        mc->function = fe.field;
                        mc->function_span = fe.field_span;
                        mc->args = std::move(c.args);
                        if (!imports_.count(obj.name)) {
                            diags_.error(DiagKind::Module, obj.span, "module '" + obj.name + "' is not imported",
                                         {"add 'import " + obj.name + ";' at the top of the file"});
                        } else {
                            mc->native_id = find_native(mc->module, mc->function);
                            if (mc->native_id < 0)
                                diags_.error(DiagKind::Name, fe.field_span,
                                             "module '" + mc->module + "' has no function '" + mc->function + "'");
                        }
                        for (auto& a : mc->args) resolve_expr(a);
                        ep = std::move(mc);
                        return;
                    }
                }
            }
            if (c.callee->kind == ExprKind::Ident) {
                resolve_ident(static_cast<IdentExpr&>(*c.callee));
            } else {
                resolve_expr(c.callee);
            }
            for (auto& a : c.args) resolve_expr(a);
            break;
        }
        case ExprKind::ModuleCall:
            for (auto& a : static_cast<ModuleCallExpr&>(e).args) resolve_expr(a);
            break;
        case ExprKind::Index: {
            auto& i = static_cast<IndexExpr&>(e);
            resolve_expr(i.object);
            resolve_expr(i.index);
            break;
        }
        case ExprKind::Field: resolve_expr(static_cast<FieldExpr&>(e).object); break;
        case ExprKind::ArrayLit:
            for (auto& x : static_cast<ArrayLit&>(e).elems) resolve_expr(x);
            break;
        case ExprKind::ArrayRepeat: {
            auto& r = static_cast<ArrayRepeat&>(e);
            resolve_expr(r.value);
            resolve_expr(r.count);
            break;
        }
        case ExprKind::StructLit: {
            auto& s = static_cast<StructLit&>(e);
            auto it = structs_.find(s.name);
            if (it == structs_.end()) {
                diags_.error(DiagKind::Name, s.name_span, "unknown struct '" + s.name + "'");
            } else {
                s.decl = it->second;
            }
            for (auto& f : s.fields) resolve_expr(f.value);
            break;
        }
        case ExprKind::EnumCtor: {
            auto& c = static_cast<EnumCtor&>(e);
            auto it = enums_.find(c.enum_name);
            if (it == enums_.end()) {
                diags_.error(DiagKind::Name, c.span, "unknown enum '" + c.enum_name + "'");
            } else {
                c.decl = it->second;
            }
            for (auto& a : c.args) resolve_expr(a);
            break;
        }
        case ExprKind::Lambda: {
            auto& l = static_cast<LambdaExpr&>(e);
            std::string name = "<lambda@" + std::to_string(l.span.line) + ":" + std::to_string(l.span.column) + ">";
            FnInfo* info = begin_function(*l.fn, name);
            int saved_loop = loop_depth_;
            resolve_function_body(*l.fn, info);
            loop_depth_ = saved_loop;
            break;
        }
        case ExprKind::Match: {
            auto& m = static_cast<MatchExpr&>(e);
            resolve_expr(m.scrutinee);
            for (auto& arm : m.arms) {
                push_scope();
                Pattern& p = arm.pattern;
                if (p.kind == PatternKind::Variant) {
                    auto it = enums_.find(p.enum_name);
                    if (it == enums_.end()) {
                        diags_.error(DiagKind::Name, p.span, "unknown enum '" + p.enum_name + "'");
                    } else {
                        p.decl = it->second;
                    }
                    for (size_t i = 0; i < p.bindings.size(); ++i) {
                        if (p.bindings[i] == "_") {
                            p.binding_syms.push_back(nullptr);
                        } else {
                            p.binding_syms.push_back(declare_local(p.bindings[i], p.binding_spans[i], false));
                        }
                    }
                }
                if (arm.expr) resolve_expr(arm.expr);
                if (arm.block) resolve_block(*arm.block);
                pop_scope();
            }
            break;
        }
    }
}

}  // namespace ember

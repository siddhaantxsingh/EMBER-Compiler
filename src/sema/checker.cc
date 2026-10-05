#include "sema/checker.h"

#include <cmath>
#include <functional>
#include <set>

#include "sema/resolver.h"

namespace ember {

namespace {
bool is_numeric(Type* t) { return t->kind == TypeKind::Int || t->kind == TypeKind::Float; }
std::string q(Type* t) { return type_name(t); }
}  // namespace

TypeChecker::TypeChecker(Program& prog, SemaResult& sema, Diagnostics& diags)
    : prog_(prog), sema_(sema), types_(sema.types), diags_(diags) {}

bool TypeChecker::expect(Type* want, Type* found, Span span, const std::string& what) {
    if (!want || !found || want->is_error() || found->is_error() || want == found) return true;
    error(span, "mismatched types in " + what, {"expected: " + q(want), "found: " + q(found)});
    return false;
}

Type* TypeChecker::resolve_type(const TypeExpr* t) {
    if (!t) return types_.unit();
    switch (t->kind) {
        case TypeExprKind::Named: {
            const std::string& n = t->name;
            if (n == "Int") return types_.int_();
            if (n == "Float") return types_.float_();
            if (n == "Bool") return types_.bool_();
            if (n == "String") return types_.string();
            if (n == "Unit") return types_.unit();
            for (auto* s : sema_.structs)
                if (s->name == n) return types_.struct_type(s);
            for (auto* e : sema_.enums)
                if (e->name == n) return types_.enum_type(e);
            error(t->span, "unknown type '" + n + "'");
            return types_.error();
        }
        case TypeExprKind::Array: {
            Type* e = resolve_type(t->elem.get());
            return e->is_error() ? e : types_.array(e);
        }
        case TypeExprKind::Func: {
            std::vector<Type*> ps;
            bool bad = false;
            for (auto& p : t->params) {
                ps.push_back(resolve_type(p.get()));
                bad |= ps.back()->is_error();
            }
            Type* r = resolve_type(t->ret.get());
            if (bad || r->is_error()) return types_.error();
            return types_.func(ps, r);
        }
    }
    return types_.error();
}

void TypeChecker::declare_fn_signature(FnDecl& fn) {
    std::vector<Type*> ps;
    bool bad = false;
    for (auto& p : fn.params) {
        Type* t = resolve_type(p.type.get());
        if (p.sym) p.sym->type = t;
        ps.push_back(t);
        bad |= t->is_error();
    }
    Type* r = resolve_type(fn.ret.get());
    fn.fn_type = (bad || r->is_error()) ? types_.error() : types_.func(ps, r);
    if (fn.sym) fn.sym->type = fn.fn_type;
}

void TypeChecker::run() {
    // 1. Nominal types.
    for (auto* s : sema_.structs) {
        s->type = types_.struct_type(s);
        std::set<std::string> seen;
        for (auto& f : s->fields) {
            if (!seen.insert(f.name).second) error(f.span, "duplicate field '" + f.name + "' in struct '" + s->name + "'");
            f.resolved = resolve_type(f.type.get());
        }
    }
    for (auto* e : sema_.enums) {
        e->type = types_.enum_type(e);
        std::set<std::string> seen;
        for (auto& v : e->variants) {
            if (!seen.insert(v.name).second) error(v.span, "duplicate variant '" + v.name + "' in enum '" + e->name + "'");
            for (auto& p : v.payload) v.resolved.push_back(resolve_type(p.get()));
        }
    }
    // 2. Top-level function signatures.
    for (auto& item : prog_.items)
        if (item.kind == ItemKind::Fn) declare_fn_signature(*item.fn);
    // 3. Constants and globals, in source order. Globals with an annotation
    //    get their type first so that functions can use them.
    for (auto& item : prog_.items) {
        if (item.kind == ItemKind::Global && item.global->type && item.global->sym)
            item.global->sym->type = resolve_type(item.global->type.get());
    }
    for (auto& item : prog_.items) {
        if (diags_.full()) return;
        if (item.kind == ItemKind::Const) {
            fold_const(*item.const_decl);
        } else if (item.kind == ItemKind::Global) {
            auto& g = *item.global;
            Type* want = g.sym ? g.sym->type : nullptr;
            Type* t = check_expr(*g.init, want);
            if (want) {
                expect(want, t, g.init->span, "global initializer");
            } else if (g.sym) {
                g.sym->type = t;
            }
        }
    }
    // 4. Function bodies.
    for (auto& item : prog_.items) {
        if (diags_.full()) return;
        if (item.kind == ItemKind::Fn) check_fn_body(*item.fn);
    }
    if (sema_.main && sema_.main->decl) {
        FnDecl& m = *sema_.main->decl;
        if (!m.params.empty() || (m.fn_type && !m.fn_type->is_error() && m.fn_type->ret != types_.unit()))
            error(m.name_span, "'main' must take no parameters and return Unit");
    }
}

void TypeChecker::check_fn_body(FnDecl& fn) {
    Type* saved = current_ret_;
    current_ret_ = fn.fn_type && !fn.fn_type->is_error() ? fn.fn_type->ret : types_.error();
    check_block(*fn.body);
    if (current_ret_ != types_.unit() && !current_ret_->is_error() && !definitely_returns(*fn.body)) {
        error(fn.ret ? fn.ret->span : fn.span,
              "function '" + (fn.name.empty() ? std::string("<lambda>") : fn.name) +
                  "' may reach the end without returning a value",
              {"declared return type: " + q(current_ret_)});
    }
    current_ret_ = saved;
}

bool TypeChecker::stmt_returns(const Stmt& s) const {
    switch (s.kind) {
        case StmtKind::Return: return true;
        case StmtKind::BlockStmt: return definitely_returns(*static_cast<const BlockStmt&>(s).block);
        case StmtKind::If: {
            auto& i = static_cast<const IfStmt&>(s);
            return i.else_stmt && definitely_returns(*i.then_block) && stmt_returns(*i.else_stmt);
        }
        case StmtKind::While: {
            // `while true { ... }` without a break never falls through.
            auto& w = static_cast<const WhileStmt&>(s);
            if (w.cond->kind != ExprKind::BoolLit || !static_cast<const BoolLit&>(*w.cond).value) return false;
            // Look for a break that targets this loop (not nested loops).
            std::vector<const Block*> work{w.body.get()};
            while (!work.empty()) {
                const Block* b = work.back();
                work.pop_back();
                for (auto& st : b->stmts) {
                    switch (st->kind) {
                        case StmtKind::Break: return false;
                        case StmtKind::BlockStmt: work.push_back(static_cast<const BlockStmt&>(*st).block.get()); break;
                        case StmtKind::If: {
                            const Stmt* cur = st.get();
                            while (cur && cur->kind == StmtKind::If) {
                                auto& i = static_cast<const IfStmt&>(*cur);
                                work.push_back(i.then_block.get());
                                cur = i.else_stmt.get();
                            }
                            if (cur) work.push_back(static_cast<const BlockStmt&>(*cur).block.get());
                            break;
                        }
                        default: break;  // nested loops own their breaks
                    }
                }
            }
            return true;
        }
        case StmtKind::ExprStmt: {
            // panic(...) never returns.
            auto& e = *static_cast<const ExprStmt&>(s).expr;
            if (e.kind == ExprKind::Call) {
                auto& c = static_cast<const CallExpr&>(e);
                if (c.callee->kind == ExprKind::Ident) {
                    auto* sym = static_cast<const IdentExpr&>(*c.callee).sym;
                    if (sym && sym->kind == SymbolKind::Builtin && sym->builtin == Builtin::Panic) return true;
                }
            }
            return false;
        }
        default: return false;
    }
}

bool TypeChecker::definitely_returns(const Block& b) const {
    for (auto& s : b.stmts)
        if (stmt_returns(*s)) return true;
    return false;
}

void TypeChecker::check_block(Block& b) {
    // Nested function signatures first: the resolver bound their names at
    // block entry, so calls may precede the declaration.
    for (auto& s : b.stmts)
        if (s->kind == StmtKind::Fn) declare_fn_signature(*static_cast<FnStmt&>(*s).fn);
    for (auto& s : b.stmts) {
        if (diags_.full()) return;
        check_stmt(*s);
    }
}

void TypeChecker::check_assign_target(Expr& target) {
    // Field and index targets are checked by check_expr; nothing else to do.
    (void)target;
}

void TypeChecker::check_stmt(Stmt& s) {
    switch (s.kind) {
        case StmtKind::Let: {
            auto& l = static_cast<LetStmt&>(s);
            Type* want = l.type ? resolve_type(l.type.get()) : nullptr;
            Type* t = check_expr(*l.init, want);
            if (want) {
                expect(want, t, l.init->span, "variable initializer");
                t = want;
            } else if (t == types_.unit() && !t->is_error()) {
                // Allowed but almost always a mistake.
                error(l.init->span, "variable '" + l.name + "' would have type Unit",
                      {"the initializer produces no value"});
            }
            if (l.sym) l.sym->type = t;
            break;
        }
        case StmtKind::Fn: check_fn_body(*static_cast<FnStmt&>(s).fn); break;
        case StmtKind::If: {
            auto& i = static_cast<IfStmt&>(s);
            expect(types_.bool_(), check_expr(*i.cond, types_.bool_()), i.cond->span, "if condition");
            check_block(*i.then_block);
            if (i.else_stmt) check_stmt(*i.else_stmt);
            break;
        }
        case StmtKind::While: {
            auto& w = static_cast<WhileStmt&>(s);
            expect(types_.bool_(), check_expr(*w.cond, types_.bool_()), w.cond->span, "while condition");
            check_block(*w.body);
            break;
        }
        case StmtKind::ForRange: {
            auto& f = static_cast<ForRangeStmt&>(s);
            expect(types_.int_(), check_expr(*f.start, types_.int_()), f.start->span, "range start");
            expect(types_.int_(), check_expr(*f.end, types_.int_()), f.end->span, "range end");
            if (f.sym) f.sym->type = types_.int_();
            check_block(*f.body);
            break;
        }
        case StmtKind::ForEach: {
            auto& f = static_cast<ForEachStmt&>(s);
            Type* t = check_expr(*f.iterable);
            Type* elem = types_.error();
            if (t->kind == TypeKind::Array) {
                elem = t->elem;
            } else if (!t->is_error()) {
                error(f.iterable->span, "cannot iterate over a value of type " + q(t),
                      {"for x in ... needs an array; use 'for i in a..b' for a range"});
            }
            if (f.sym) f.sym->type = elem;
            check_block(*f.body);
            break;
        }
        case StmtKind::Break:
        case StmtKind::Continue: break;
        case StmtKind::Return: {
            auto& r = static_cast<ReturnStmt&>(s);
            if (!current_ret_) {
                error(s.span, "'return' outside of a function");
                break;
            }
            if (r.value) {
                Type* t = check_expr(*r.value, current_ret_);
                expect(current_ret_, t, r.value->span, "return value");
            } else if (current_ret_ != types_.unit() && !current_ret_->is_error()) {
                error(s.span, "missing return value", {"expected: " + q(current_ret_)});
            }
            break;
        }
        case StmtKind::Assign: {
            auto& a = static_cast<AssignStmt&>(s);
            Type* tt = check_expr(*a.target);
            Type* vt = check_expr(*a.value, tt->is_error() ? nullptr : tt);
            if (a.compound) {
                bool ok = (a.op == BinaryOp::Add && (is_numeric(tt) || tt->kind == TypeKind::String)) ||
                          (a.op == BinaryOp::Mod && tt->kind == TypeKind::Int) ||
                          (a.op != BinaryOp::Add && a.op != BinaryOp::Mod && is_numeric(tt));
                if (!tt->is_error() && !ok) {
                    error(a.span, std::string("operator '") + binary_op_str(a.op) + "=' cannot be applied to " + q(tt));
                    break;
                }
            }
            expect(tt, vt, a.value->span, "assignment");
            break;
        }
        case StmtKind::ExprStmt: check_expr(*static_cast<ExprStmt&>(s).expr); break;
        case StmtKind::BlockStmt: check_block(*static_cast<BlockStmt&>(s).block); break;
    }
}

Type* TypeChecker::check_binary(BinaryExpr& b) {
    Type* l = check_expr(*b.lhs);
    Type* r = check_expr(*b.rhs, l->is_error() ? nullptr : l);
    if (l->is_error() || r->is_error()) return types_.error();
    const char* op = binary_op_str(b.op);
    auto mismatch = [&](const char* what) {
        std::string verb = b.op == BinaryOp::Add ? "add" : b.op == BinaryOp::Sub ? "subtract"
                         : b.op == BinaryOp::Mul ? "multiply" : b.op == BinaryOp::Div ? "divide" : "apply";
        if (l != r) {
            error(b.op_span.valid() ? b.op_span : b.span,
                  std::string("cannot ") + (std::string(verb) == "apply" ? std::string("apply '") + op + "' to" : verb) +
                      " " + q(l) + " and " + q(r),
                  {"expected: " + q(l), "found: " + q(r), what});
        } else {
            error(b.op_span.valid() ? b.op_span : b.span,
                  std::string("operator '") + op + "' cannot be applied to " + q(l), {what});
        }
        return types_.error();
    };
    switch (b.op) {
        case BinaryOp::Add:
            if (l == r && (is_numeric(l) || l->kind == TypeKind::String)) return l;
            return mismatch("'+' works on two Ints, two Floats or two Strings (use str() or float() to convert)");
        case BinaryOp::Sub: case BinaryOp::Mul: case BinaryOp::Div:
            if (l == r && is_numeric(l)) return l;
            return mismatch("arithmetic needs two Ints or two Floats");
        case BinaryOp::Mod:
            if (l == r && l->kind == TypeKind::Int) return l;
            return mismatch("'%' needs two Ints");
        case BinaryOp::Eq: case BinaryOp::Ne:
            if (l == r && (is_numeric(l) || l->kind == TypeKind::Bool || l->kind == TypeKind::String ||
                           l->kind == TypeKind::Unit))
                return types_.bool_();
            return mismatch("'==' compares two values of the same Int, Float, Bool, String or Unit type");
        case BinaryOp::Lt: case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
            if (l == r && (is_numeric(l) || l->kind == TypeKind::String)) return types_.bool_();
            return mismatch("ordering compares two Ints, two Floats or two Strings");
    }
    return types_.error();
}

Type* TypeChecker::check_builtin(CallExpr& c, Builtin b) {
    auto arity = [&](size_t n, const char* name) {
        if (c.args.size() != n) {
            error(c.span, std::string("'") + name + "' takes " + std::to_string(n) + " argument" + (n == 1 ? "" : "s") +
                              " but " + std::to_string(c.args.size()) + " were given");
            for (auto& a : c.args) check_expr(*a);
            return false;
        }
        return true;
    };
    switch (b) {
        case Builtin::Print:
            if (!arity(1, "print")) return types_.unit();
            check_expr(*c.args[0]);
            return types_.unit();
        case Builtin::Str:
            if (!arity(1, "str")) return types_.string();
            check_expr(*c.args[0]);
            return types_.string();
        case Builtin::Input:
            arity(0, "input");
            return types_.string();
        case Builtin::Len: {
            if (!arity(1, "len")) return types_.int_();
            Type* t = check_expr(*c.args[0]);
            if (!t->is_error() && t->kind != TypeKind::Array && t->kind != TypeKind::String)
                error(c.args[0]->span, "'len' needs an array or a String", {"found: " + q(t)});
            return types_.int_();
        }
        case Builtin::Push: {
            if (!arity(2, "push")) return types_.unit();
            Type* t = check_expr(*c.args[0]);
            if (t->is_error()) {
                check_expr(*c.args[1]);
            } else if (t->kind != TypeKind::Array) {
                error(c.args[0]->span, "'push' needs an array as its first argument", {"found: " + q(t)});
                check_expr(*c.args[1]);
            } else {
                expect(t->elem, check_expr(*c.args[1], t->elem), c.args[1]->span, "pushed value");
            }
            return types_.unit();
        }
        case Builtin::Pop: {
            if (!arity(1, "pop")) return types_.error();
            Type* t = check_expr(*c.args[0]);
            if (t->is_error()) return t;
            if (t->kind != TypeKind::Array) {
                error(c.args[0]->span, "'pop' needs an array", {"found: " + q(t)});
                return types_.error();
            }
            return t->elem;
        }
        case Builtin::Int: {
            if (!arity(1, "int")) return types_.int_();
            expect(types_.float_(), check_expr(*c.args[0], types_.float_()), c.args[0]->span, "argument of 'int'");
            return types_.int_();
        }
        case Builtin::Float: {
            if (!arity(1, "float")) return types_.float_();
            expect(types_.int_(), check_expr(*c.args[0], types_.int_()), c.args[0]->span, "argument of 'float'");
            return types_.float_();
        }
        case Builtin::ParseInt: {
            if (!arity(1, "parse_int")) return types_.int_();
            expect(types_.string(), check_expr(*c.args[0], types_.string()), c.args[0]->span, "argument of 'parse_int'");
            return types_.int_();
        }
        case Builtin::Panic: {
            if (!arity(1, "panic")) return types_.unit();
            expect(types_.string(), check_expr(*c.args[0], types_.string()), c.args[0]->span, "argument of 'panic'");
            return types_.unit();
        }
        case Builtin::Assert: {
            if (!arity(2, "assert")) return types_.unit();
            expect(types_.bool_(), check_expr(*c.args[0], types_.bool_()), c.args[0]->span, "assert condition");
            expect(types_.string(), check_expr(*c.args[1], types_.string()), c.args[1]->span, "assert message");
            return types_.unit();
        }
    }
    return types_.error();
}

Type* TypeChecker::check_module_call(ModuleCallExpr& c) {
    if (c.native_id < 0) {
        for (auto& a : c.args) check_expr(*a);
        return types_.error();
    }
    const NativeSig& sig = native_table()[static_cast<size_t>(c.native_id)];
    auto kind_type = [&](TypeKind k, bool string_array) -> Type* {
        switch (k) {
            case TypeKind::Int: return types_.int_();
            case TypeKind::Float: return types_.float_();
            case TypeKind::Bool: return types_.bool_();
            case TypeKind::String: return types_.string();
            case TypeKind::Array: return string_array ? types_.array(types_.string()) : types_.error();
            default: return types_.unit();
        }
    };
    if (c.args.size() != sig.params.size()) {
        error(c.span, c.module + "." + c.function + " takes " + std::to_string(sig.params.size()) +
                          " argument(s) but " + std::to_string(c.args.size()) + " were given");
        for (auto& a : c.args) check_expr(*a);
    } else {
        for (size_t i = 0; i < c.args.size(); ++i) {
            Type* want = kind_type(sig.params[i], i == 0 && sig.param0_string_array);
            expect(want, check_expr(*c.args[i], want), c.args[i]->span,
                   "argument " + std::to_string(i + 1) + " of " + c.module + "." + c.function);
        }
    }
    return kind_type(sig.ret, sig.ret_string_array);
}

Type* TypeChecker::check_call(CallExpr& c) {
    if (c.callee->kind == ExprKind::Ident) {
        auto& id = static_cast<IdentExpr&>(*c.callee);
        if (id.sym && id.sym->kind == SymbolKind::Builtin) {
            Type* t = check_builtin(c, id.sym->builtin);
            id.type = types_.error();  // builtins have no first-class type
            return t;
        }
    }
    Type* ct = check_expr(*c.callee);
    if (ct->is_error()) {
        for (auto& a : c.args) check_expr(*a);
        return types_.error();
    }
    if (ct->kind != TypeKind::Func) {
        error(c.callee->span, "cannot call a value of type " + q(ct));
        for (auto& a : c.args) check_expr(*a);
        return types_.error();
    }
    if (c.args.size() != ct->params.size()) {
        error(c.span, "function takes " + std::to_string(ct->params.size()) + " argument(s) but " +
                          std::to_string(c.args.size()) + " were given",
              {"function type: " + q(ct)});
        for (auto& a : c.args) check_expr(*a);
        return ct->ret;
    }
    for (size_t i = 0; i < c.args.size(); ++i) {
        expect(ct->params[i], check_expr(*c.args[i], ct->params[i]), c.args[i]->span,
               "argument " + std::to_string(i + 1));
    }
    return ct->ret;
}

Type* TypeChecker::check_match(MatchExpr& m, Type* expected) {
    Type* st = check_expr(*m.scrutinee);
    Type* result = nullptr;
    Span result_span;
    bool has_wildcard = false;
    std::set<int> variants_seen;
    std::set<int64_t> ints_seen;
    std::set<std::string> strings_seen;
    std::set<bool> bools_seen;
    for (auto& arm : m.arms) {
        Pattern& p = arm.pattern;
        if (has_wildcard) error(p.span, "unreachable match arm", {"an earlier '_' arm matches everything"});
        bool ok = !st->is_error();
        switch (p.kind) {
            case PatternKind::Wildcard: has_wildcard = true; break;
            case PatternKind::Int:
                if (ok && st->kind != TypeKind::Int) error(p.span, "integer pattern does not match type " + q(st));
                else if (!ints_seen.insert(p.int_value).second) error(p.span, "duplicate pattern");
                break;
            case PatternKind::String:
                if (ok && st->kind != TypeKind::String) error(p.span, "string pattern does not match type " + q(st));
                else if (!strings_seen.insert(p.string_value).second) error(p.span, "duplicate pattern");
                break;
            case PatternKind::Bool:
                if (ok && st->kind != TypeKind::Bool) error(p.span, "boolean pattern does not match type " + q(st));
                else if (!bools_seen.insert(p.bool_value).second) error(p.span, "duplicate pattern");
                break;
            case PatternKind::Variant: {
                if (!p.decl) break;  // already reported
                if (ok && (st->kind != TypeKind::Enum || st->enum_decl != p.decl)) {
                    error(p.span, "pattern of enum '" + p.decl->name + "' does not match type " + q(st));
                    break;
                }
                int vi = -1;
                for (size_t i = 0; i < p.decl->variants.size(); ++i)
                    if (p.decl->variants[i].name == p.variant) vi = static_cast<int>(i);
                if (vi < 0) {
                    error(p.span, "enum '" + p.decl->name + "' has no variant '" + p.variant + "'");
                    break;
                }
                p.variant_index = vi;
                auto& v = p.decl->variants[static_cast<size_t>(vi)];
                if (!variants_seen.insert(vi).second) error(p.span, "unreachable match arm", {"variant already matched"});
                if (p.bindings.size() != v.resolved.size()) {
                    error(p.span, "variant '" + p.decl->name + "::" + v.name + "' has " +
                                      std::to_string(v.resolved.size()) + " field(s) but the pattern binds " +
                                      std::to_string(p.bindings.size()));
                }
                for (size_t i = 0; i < p.binding_syms.size(); ++i) {
                    if (p.binding_syms[i])
                        p.binding_syms[i]->type = i < v.resolved.size() ? v.resolved[i] : types_.error();
                }
                break;
            }
        }
        Type* at;
        if (arm.block) {
            check_block(*arm.block);
            at = types_.unit();
        } else {
            at = check_expr(*arm.expr, result ? result : expected);
        }
        if (!result) {
            result = at;
            result_span = arm.span;
        } else if (!at->is_error() && !result->is_error() && at != result) {
            error(arm.block ? arm.span : arm.expr->span, "match arms have different types",
                  {"first arm: " + q(result), "this arm: " + q(at)});
        }
    }
    // Exhaustiveness
    if (!has_wildcard && !st->is_error()) {
        if (st->kind == TypeKind::Enum) {
            std::vector<std::string> missing;
            for (size_t i = 0; i < st->enum_decl->variants.size(); ++i)
                if (!variants_seen.count(static_cast<int>(i))) missing.push_back(st->enum_decl->variants[i].name);
            if (!missing.empty()) {
                std::string list;
                for (size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + st->enum_decl->name + "::" + missing[i];
                error(m.span, "non-exhaustive match", {"missing: " + list, "add the missing arms or a '_' arm"});
            }
        } else if (st->kind == TypeKind::Bool) {
            if (bools_seen.size() < 2) error(m.span, "non-exhaustive match", {"add a '_' arm"});
        } else {
            error(m.span, "non-exhaustive match", {"matching on " + q(st) + " needs a '_' arm"});
        }
    }
    return result ? result : types_.error();
}

Type* TypeChecker::check_expr(Expr& e, Type* expected) {
    Type* t = types_.error();
    switch (e.kind) {
        case ExprKind::IntLit: t = types_.int_(); break;
        case ExprKind::FloatLit: t = types_.float_(); break;
        case ExprKind::BoolLit: t = types_.bool_(); break;
        case ExprKind::StringLit: t = types_.string(); break;
        case ExprKind::UnitLit: t = types_.unit(); break;
        case ExprKind::Ident: {
            auto& id = static_cast<IdentExpr&>(e);
            if (!id.sym) break;
            if (id.sym->kind == SymbolKind::Const && id.sym->const_decl && !id.sym->const_ready) {
                error(id.span, "constant '" + id.name + "' is used before its definition");
                break;
            }
            if (id.sym->kind == SymbolKind::Global && !id.sym->type) {
                error(id.span, "the type of global '" + id.name + "' is not known yet",
                      {"add a type annotation to the global, or declare it before this use"});
                break;
            }
            t = id.sym->type ? id.sym->type : types_.error();
            break;
        }
        case ExprKind::Unary: {
            auto& u = static_cast<UnaryExpr&>(e);
            Type* ot = check_expr(*u.operand);
            if (ot->is_error()) break;
            if (u.op == UnaryOp::Neg) {
                if (is_numeric(ot)) t = ot;
                else error(u.span, "cannot negate a value of type " + q(ot));
            } else {
                if (ot->kind == TypeKind::Bool) t = ot;
                else error(u.span, "'!' needs a Bool", {"found: " + q(ot)});
            }
            break;
        }
        case ExprKind::Binary: t = check_binary(static_cast<BinaryExpr&>(e)); break;
        case ExprKind::Logical: {
            auto& l = static_cast<LogicalExpr&>(e);
            const char* op = l.op == LogicalOp::And ? "&&" : "||";
            expect(types_.bool_(), check_expr(*l.lhs, types_.bool_()), l.lhs->span, std::string("operand of '") + op + "'");
            expect(types_.bool_(), check_expr(*l.rhs, types_.bool_()), l.rhs->span, std::string("operand of '") + op + "'");
            t = types_.bool_();
            break;
        }
        case ExprKind::Call: t = check_call(static_cast<CallExpr&>(e)); break;
        case ExprKind::ModuleCall: t = check_module_call(static_cast<ModuleCallExpr&>(e)); break;
        case ExprKind::Index: {
            auto& i = static_cast<IndexExpr&>(e);
            Type* ot = check_expr(*i.object);
            expect(types_.int_(), check_expr(*i.index, types_.int_()), i.index->span, "index");
            if (ot->is_error()) break;
            if (ot->kind == TypeKind::Array) t = ot->elem;
            else if (ot->kind == TypeKind::String) t = types_.string();
            else error(i.object->span, "cannot index a value of type " + q(ot));
            break;
        }
        case ExprKind::Field: {
            auto& f = static_cast<FieldExpr&>(e);
            Type* ot = check_expr(*f.object);
            if (ot->is_error()) break;
            if (ot->kind != TypeKind::Struct) {
                error(f.field_span, "type " + q(ot) + " has no field '" + f.field + "'",
                      {"fields exist only on structs"});
                break;
            }
            auto& fields = ot->struct_decl->fields;
            for (size_t k = 0; k < fields.size(); ++k) {
                if (fields[k].name == f.field) {
                    f.field_index = static_cast<int>(k);
                    t = fields[k].resolved;
                }
            }
            if (f.field_index < 0) {
                std::string avail;
                for (size_t k = 0; k < fields.size(); ++k) avail += (k ? ", " : "") + fields[k].name;
                error(f.field_span, "struct '" + ot->struct_decl->name + "' has no field '" + f.field + "'",
                      {"available fields: " + avail});
            }
            break;
        }
        case ExprKind::ArrayLit: {
            auto& a = static_cast<ArrayLit&>(e);
            Type* elem = expected && expected->kind == TypeKind::Array ? expected->elem : nullptr;
            if (a.elems.empty()) {
                if (!elem) {
                    error(a.span, "cannot infer the type of an empty array",
                          {"add a type annotation, e.g. let xs: [Int] = [];"});
                    break;
                }
                t = types_.array(elem);
                break;
            }
            Type* first = check_expr(*a.elems[0], elem);
            Type* et = elem ? elem : first;
            if (elem) expect(elem, first, a.elems[0]->span, "array element");
            for (size_t i = 1; i < a.elems.size(); ++i)
                expect(et, check_expr(*a.elems[i], et), a.elems[i]->span, "array element");
            if (!et->is_error()) t = types_.array(et);
            break;
        }
        case ExprKind::ArrayRepeat: {
            auto& r = static_cast<ArrayRepeat&>(e);
            Type* elem = expected && expected->kind == TypeKind::Array ? expected->elem : nullptr;
            Type* vt = check_expr(*r.value, elem);
            expect(types_.int_(), check_expr(*r.count, types_.int_()), r.count->span, "array length");
            if (!vt->is_error()) t = types_.array(vt);
            break;
        }
        case ExprKind::StructLit: {
            auto& s = static_cast<StructLit&>(e);
            if (!s.decl) {
                for (auto& f : s.fields) check_expr(*f.value);
                break;
            }
            std::set<std::string> given;
            for (auto& fi : s.fields) {
                int idx = -1;
                for (size_t k = 0; k < s.decl->fields.size(); ++k)
                    if (s.decl->fields[k].name == fi.name) idx = static_cast<int>(k);
                if (idx < 0) {
                    error(fi.span, "struct '" + s.name + "' has no field '" + fi.name + "'");
                    check_expr(*fi.value);
                    continue;
                }
                if (!given.insert(fi.name).second) error(fi.span, "field '" + fi.name + "' is given twice");
                Type* ft = s.decl->fields[static_cast<size_t>(idx)].resolved;
                expect(ft, check_expr(*fi.value, ft), fi.value->span, "field '" + fi.name + "'");
            }
            std::string missing;
            for (auto& fd : s.decl->fields)
                if (!given.count(fd.name)) missing += (missing.empty() ? "" : ", ") + fd.name;
            if (!missing.empty()) error(s.span, "missing field(s) in struct literal: " + missing);
            t = s.decl->type;
            break;
        }
        case ExprKind::EnumCtor: {
            auto& c = static_cast<EnumCtor&>(e);
            if (!c.decl) {
                for (auto& a : c.args) check_expr(*a);
                break;
            }
            for (size_t i = 0; i < c.decl->variants.size(); ++i)
                if (c.decl->variants[i].name == c.variant) c.variant_index = static_cast<int>(i);
            if (c.variant_index < 0) {
                error(c.span, "enum '" + c.enum_name + "' has no variant '" + c.variant + "'");
                for (auto& a : c.args) check_expr(*a);
                break;
            }
            auto& v = c.decl->variants[static_cast<size_t>(c.variant_index)];
            if (c.args.size() != v.resolved.size()) {
                error(c.span, "variant '" + c.enum_name + "::" + c.variant + "' takes " +
                                  std::to_string(v.resolved.size()) + " value(s) but " +
                                  std::to_string(c.args.size()) + " were given");
                for (auto& a : c.args) check_expr(*a);
            } else {
                for (size_t i = 0; i < c.args.size(); ++i)
                    expect(v.resolved[i], check_expr(*c.args[i], v.resolved[i]), c.args[i]->span, "variant value");
            }
            t = c.decl->type;
            break;
        }
        case ExprKind::Lambda: {
            auto& l = static_cast<LambdaExpr&>(e);
            declare_fn_signature(*l.fn);
            check_fn_body(*l.fn);
            t = l.fn->fn_type;
            break;
        }
        case ExprKind::Match: t = check_match(static_cast<MatchExpr&>(e), expected); break;
    }
    e.type = t;
    return t;
}

// ------------------------------------------------------------ constants

namespace {
struct CVal {
    TypeKind kind = TypeKind::Error;
    int64_t i = 0;
    double f = 0;
    bool b = false;
    std::string s;
};
}  // namespace

void TypeChecker::fold_const(ConstDecl& c) {
    std::string failure;
    // Evaluate with the runtime's semantics; any failure is a compile error.
    std::function<bool(const Expr&, CVal&)> eval = [&](const Expr& e, CVal& out) -> bool {
        switch (e.kind) {
            case ExprKind::IntLit: out.kind = TypeKind::Int; out.i = static_cast<const IntLit&>(e).value; return true;
            case ExprKind::FloatLit: out.kind = TypeKind::Float; out.f = static_cast<const FloatLit&>(e).value; return true;
            case ExprKind::BoolLit: out.kind = TypeKind::Bool; out.b = static_cast<const BoolLit&>(e).value; return true;
            case ExprKind::StringLit: out.kind = TypeKind::String; out.s = static_cast<const StringLit&>(e).value; return true;
            case ExprKind::Ident: {
                auto* s = static_cast<const IdentExpr&>(e).sym;
                if (!s || s->kind != SymbolKind::Const || !s->const_ready) {
                    failure = "only literals and earlier constants can appear in a constant expression";
                    return false;
                }
                out.kind = s->type->kind;
                out.i = s->const_int; out.f = s->const_float; out.b = s->const_bool; out.s = s->const_string;
                return true;
            }
            case ExprKind::Unary: {
                auto& u = static_cast<const UnaryExpr&>(e);
                if (!eval(*u.operand, out)) return false;
                if (u.op == UnaryOp::Not && out.kind == TypeKind::Bool) { out.b = !out.b; return true; }
                if (u.op == UnaryOp::Neg && out.kind == TypeKind::Int) {
                    if (out.i == INT64_MIN) { failure = "integer overflow"; return false; }
                    out.i = -out.i; return true;
                }
                if (u.op == UnaryOp::Neg && out.kind == TypeKind::Float) { out.f = -out.f; return true; }
                failure = "invalid operand"; return false;
            }
            case ExprKind::Logical: {
                auto& l = static_cast<const LogicalExpr&>(e);
                CVal a, b;
                if (!eval(*l.lhs, a) || !eval(*l.rhs, b)) return false;
                if (a.kind != TypeKind::Bool || b.kind != TypeKind::Bool) { failure = "invalid operand"; return false; }
                out.kind = TypeKind::Bool;
                out.b = l.op == LogicalOp::And ? (a.b && b.b) : (a.b || b.b);
                return true;
            }
            case ExprKind::Binary: {
                auto& bx = static_cast<const BinaryExpr&>(e);
                CVal a, b;
                if (!eval(*bx.lhs, a) || !eval(*bx.rhs, b)) return false;
                if (a.kind != b.kind) { failure = "operands have different types"; return false; }
                if (a.kind == TypeKind::Int) {
                    int64_t r = 0;
                    out.kind = TypeKind::Int;
                    switch (bx.op) {
                        case BinaryOp::Add: if (__builtin_add_overflow(a.i, b.i, &r)) { failure = "integer overflow"; return false; } out.i = r; return true;
                        case BinaryOp::Sub: if (__builtin_sub_overflow(a.i, b.i, &r)) { failure = "integer overflow"; return false; } out.i = r; return true;
                        case BinaryOp::Mul: if (__builtin_mul_overflow(a.i, b.i, &r)) { failure = "integer overflow"; return false; } out.i = r; return true;
                        case BinaryOp::Div: case BinaryOp::Mod:
                            if (b.i == 0) { failure = "division by zero"; return false; }
                            if (a.i == INT64_MIN && b.i == -1) { failure = "integer overflow"; return false; }
                            out.i = bx.op == BinaryOp::Div ? a.i / b.i : a.i % b.i; return true;
                        default: break;
                    }
                    out.kind = TypeKind::Bool;
                    switch (bx.op) {
                        case BinaryOp::Eq: out.b = a.i == b.i; return true;
                        case BinaryOp::Ne: out.b = a.i != b.i; return true;
                        case BinaryOp::Lt: out.b = a.i < b.i; return true;
                        case BinaryOp::Le: out.b = a.i <= b.i; return true;
                        case BinaryOp::Gt: out.b = a.i > b.i; return true;
                        case BinaryOp::Ge: out.b = a.i >= b.i; return true;
                        default: break;
                    }
                }
                if (a.kind == TypeKind::Float) {
                    out.kind = TypeKind::Float;
                    switch (bx.op) {
                        case BinaryOp::Add: out.f = a.f + b.f; return true;
                        case BinaryOp::Sub: out.f = a.f - b.f; return true;
                        case BinaryOp::Mul: out.f = a.f * b.f; return true;
                        case BinaryOp::Div: out.f = a.f / b.f; return true;
                        default: break;
                    }
                    out.kind = TypeKind::Bool;
                    switch (bx.op) {
                        case BinaryOp::Eq: out.b = a.f == b.f; return true;
                        case BinaryOp::Ne: out.b = a.f != b.f; return true;
                        case BinaryOp::Lt: out.b = a.f < b.f; return true;
                        case BinaryOp::Le: out.b = a.f <= b.f; return true;
                        case BinaryOp::Gt: out.b = a.f > b.f; return true;
                        case BinaryOp::Ge: out.b = a.f >= b.f; return true;
                        default: break;
                    }
                }
                if (a.kind == TypeKind::String && bx.op == BinaryOp::Add) {
                    out.kind = TypeKind::String;
                    out.s = a.s + b.s;
                    return true;
                }
                if (a.kind == TypeKind::Bool && (bx.op == BinaryOp::Eq || bx.op == BinaryOp::Ne)) {
                    out.kind = TypeKind::Bool;
                    out.b = bx.op == BinaryOp::Eq ? a.b == b.b : a.b != b.b;
                    return true;
                }
                failure = std::string("operator '") + binary_op_str(bx.op) + "' is not supported in constant expressions";
                return false;
            }
            default:
                failure = "not a constant expression (only literals, operators and other constants)";
                return false;
        }
    };
    Type* t = check_expr(*c.value);
    if (t->is_error()) return;
    if (c.type) expect(resolve_type(c.type.get()), t, c.value->span, "constant");
    CVal v;
    if (!eval(*c.value, v)) {
        error(c.value->span, "cannot evaluate constant '" + c.name + "': " + failure);
        return;
    }
    if (!c.sym) return;
    Symbol* s = c.sym;
    s->type = t;
    s->const_int = v.i;
    s->const_float = v.f;
    s->const_bool = v.b;
    s->const_string = v.s;
    s->const_ready = true;
}

}  // namespace ember

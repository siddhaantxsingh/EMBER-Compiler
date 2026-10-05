#include "tools/refinterp.h"

#include <cmath>
#include <memory>
#include <ostream>
#include <pthread.h>
#include <unordered_map>
#include <vector>

#include "common/numfmt.h"

namespace ember {

namespace {

struct RVal;
using Cell = std::shared_ptr<RVal>;

struct ArrV;
struct StructV;
struct EnumV;
struct FnV;

struct RVal {
    enum class K { Uninit, Unit, Bool, Int, Float, Str, Arr, Struct, Enum, Fn } k = K::Uninit;
    int64_t i = 0;
    double f = 0;
    bool b = false;
    std::shared_ptr<const std::string> s;
    std::shared_ptr<ArrV> a;
    std::shared_ptr<StructV> st;
    std::shared_ptr<EnumV> e;
    std::shared_ptr<FnV> fn;
};

struct ArrV { std::vector<RVal> items; };
struct StructV { StructDecl* decl; std::vector<RVal> fields; };
struct EnumV { EnumDecl* decl; int variant; std::vector<RVal> fields; };

using Env = std::unordered_map<const Symbol*, Cell>;

struct FnV {
    FnDecl* decl;
    Env captured;
};

RVal mk_unit() { RVal v; v.k = RVal::K::Unit; return v; }
RVal mk_bool(bool b) { RVal v; v.k = RVal::K::Bool; v.b = b; return v; }
RVal mk_int(int64_t i) { RVal v; v.k = RVal::K::Int; v.i = i; return v; }
RVal mk_float(double f) { RVal v; v.k = RVal::K::Float; v.f = f; return v; }
RVal mk_str(std::string s) { RVal v; v.k = RVal::K::Str; v.s = std::make_shared<const std::string>(std::move(s)); return v; }

struct RuntimeErr { std::string msg; };
struct LimitErr {};

enum class Flow { Normal, Break, Continue, Return };

// Control flow out of a match-arm block (match is an expression in the AST,
// so statements inside it unwind through eval()).
struct ReturnSignal { RVal v; };
struct BreakSignal {};
struct ContinueSignal {};

class Interp {
public:
    Interp(Program& p, SemaResult& s, std::ostream& out, const RefOptions& o) : prog_(p), sema_(s), out_(out), opts_(o) {}

    // Values are reference counted, so cyclic structures (a closure that
    // captures itself, a struct that contains itself) would leak. Every
    // container is registered weakly and emptied at the end, which breaks
    // all cycles.
    ~Interp() {
        for (auto& w : fns_)
            if (auto f = w.lock()) f->captured.clear();
        for (auto& w : arrs_)
            if (auto a = w.lock()) a->items.clear();
        for (auto& w : structs_)
            if (auto st = w.lock()) st->fields.clear();
        for (auto& w : enums_)
            if (auto e = w.lock()) e->fields.clear();
        for (auto& g : globals_) *g = RVal{};
    }

    void run() {
        globals_.resize(sema_.globals.size());
        for (auto& g : globals_) g = std::make_shared<RVal>();
        Env env;
        for (auto& item : prog_.items) {
            if (item.kind != ItemKind::Global) continue;
            RVal v = eval(*item.global->init, env);
            *globals_[static_cast<size_t>(item.global->sym->global_index)] = v;
        }
        FnDecl* main = sema_.main->decl;
        call_fn(*main, Env{}, {});
    }

private:
    void step() {
        if (++steps_ > opts_.max_steps) throw LimitErr{};
    }
    [[noreturn]] void err(const std::string& m) { throw RuntimeErr{m}; }

    // ---------------------------------------------------------- formatting
    void format(std::string& out, const RVal& v, int depth) {
        if (depth > 32) {
            out += "...";
            return;
        }
        switch (v.k) {
            case RVal::K::Uninit: out += "<uninitialized>"; break;
            case RVal::K::Unit: out += "()"; break;
            case RVal::K::Bool: out += v.b ? "true" : "false"; break;
            case RVal::K::Int: out += std::to_string(v.i); break;
            case RVal::K::Float: out += format_float(v.f); break;
            case RVal::K::Str: out += *v.s; break;
            case RVal::K::Arr:
                out += "[";
                for (size_t i = 0; i < v.a->items.size(); ++i) {
                    if (i) out += ", ";
                    format(out, v.a->items[i], depth + 1);
                }
                out += "]";
                break;
            case RVal::K::Struct:
                out += v.st->decl->name;
                if (v.st->fields.empty()) {
                    out += " {}";
                    break;
                }
                out += " { ";
                for (size_t i = 0; i < v.st->fields.size(); ++i) {
                    if (i) out += ", ";
                    out += v.st->decl->fields[i].name + ": ";
                    format(out, v.st->fields[i], depth + 1);
                }
                out += " }";
                break;
            case RVal::K::Enum: {
                out += v.e->decl->name + "::" + v.e->decl->variants[static_cast<size_t>(v.e->variant)].name;
                if (!v.e->fields.empty()) {
                    out += "(";
                    for (size_t i = 0; i < v.e->fields.size(); ++i) {
                        if (i) out += ", ";
                        format(out, v.e->fields[i], depth + 1);
                    }
                    out += ")";
                }
                break;
            }
            case RVal::K::Fn: out += "<fn " + v.fn->decl->info->display_name + ">"; break;
        }
    }
    std::string fmt(const RVal& v) {
        std::string s;
        format(s, v, 0);
        if (s.size() > kMaxString)
            err("value too large: formatted value exceeds the limit of " + std::to_string(kMaxString) + " bytes");
        return s;
    }

    static constexpr size_t kMaxString = 64u << 20;
    static constexpr size_t kMaxArray = 16u << 20;
    void check_string(size_t n) {
        if (n > kMaxString)
            err("value too large: string of " + std::to_string(n) + " bytes exceeds the limit of " +
                std::to_string(kMaxString));
    }
    void check_array(size_t n) {
        if (n > kMaxArray)
            err("value too large: array of " + std::to_string(n) + " elements exceeds the limit of " +
                std::to_string(kMaxArray));
    }

    // ---------------------------------------------------------- variables
    Cell& cell(const Symbol* s, Env& env) {
        auto it = env.find(s);
        if (it == env.end()) err("internal: unbound " + s->name);
        return it->second;
    }

    RVal load(Symbol* s, Env& env) {
        switch (s->kind) {
            case SymbolKind::Local: {
                RVal v = *cell(s, env);
                if (v.k == RVal::K::Uninit)
                    err("uninitialized variable: '" + s->name + "' was used before its declaration ran");
                return v;
            }
            case SymbolKind::Global: {
                RVal v = *globals_[static_cast<size_t>(s->global_index)];
                if (v.k == RVal::K::Uninit)
                    err("uninitialized global: '" + s->name + "' was read before its initializer ran");
                return v;
            }
            case SymbolKind::Function: {
                RVal v;
                v.k = RVal::K::Fn;
                v.fn = std::make_shared<FnV>(FnV{s->fn, Env{}});
                return v;
            }
            case SymbolKind::Const:
                switch (s->type->kind) {
                    case TypeKind::Int: return mk_int(s->const_int);
                    case TypeKind::Float: return mk_float(s->const_float);
                    case TypeKind::Bool: return mk_bool(s->const_bool);
                    case TypeKind::String: return mk_str(s->const_string);
                    default: break;
                }
                break;
            default: break;
        }
        err("internal: cannot load " + s->name);
    }

    void store(Symbol* s, const RVal& v, Env& env) {
        if (s->kind == SymbolKind::Global) *globals_[static_cast<size_t>(s->global_index)] = v;
        else *cell(s, env) = v;
    }

    // ---------------------------------------------------------- calls
    RVal call(FnDecl& fn, Env env, std::vector<RVal> args) {
        if (++depth_ > opts_.max_depth) {
            --depth_;
            err("stack overflow");
        }
        for (size_t i = 0; i < fn.params.size(); ++i) env[fn.params[i].sym] = std::make_shared<RVal>(args[i]);
        RVal ret = mk_unit();
        Flow f = exec_block(*fn.body, env, ret);
        --depth_;
        if (f == Flow::Return) return ret;
        return mk_unit();
    }

    RVal make_closure(FnDecl& fn, Env& env) {
        RVal v;
        v.k = RVal::K::Fn;
        v.fn = std::make_shared<FnV>(FnV{&fn, env});
        fns_.push_back(v.fn);
        return v;
    }

    // ---------------------------------------------------------- statements
    Flow exec_block(Block& b, Env& env, RVal& ret) {
        // Fresh variables for this execution of the block: nested functions
        // and lets are bound at entry (lets start uninitialized).
        for (auto& s : b.stmts) {
            if (s->kind == StmtKind::Let) env[static_cast<LetStmt&>(*s).sym] = std::make_shared<RVal>();
            if (s->kind == StmtKind::Fn) env[static_cast<FnStmt&>(*s).fn->sym] = std::make_shared<RVal>();
        }
        for (auto& s : b.stmts) {
            if (s->kind != StmtKind::Fn) continue;
            auto& fs = static_cast<FnStmt&>(*s);
            *env[fs.fn->sym] = make_closure(*fs.fn, env);
        }
        // Closures above copied the environment before the other nested
        // functions were bound; rebind so they all see each other's cells.
        for (auto& s : b.stmts) {
            if (s->kind != StmtKind::Fn) continue;
            auto& fs = static_cast<FnStmt&>(*s);
            env[fs.fn->sym]->fn->captured = env;
        }
        for (auto& s : b.stmts) {
            Flow f = exec(*s, env, ret);
            if (f != Flow::Normal) return f;
        }
        return Flow::Normal;
    }

    Flow exec(Stmt& s, Env& env, RVal& ret) {
        step();
        switch (s.kind) {
            case StmtKind::Let: {
                auto& l = static_cast<LetStmt&>(s);
                RVal v = eval(*l.init, env);
                *env[l.sym] = v;
                return Flow::Normal;
            }
            case StmtKind::Fn: return Flow::Normal;
            case StmtKind::If: {
                auto& i = static_cast<IfStmt&>(s);
                if (eval(*i.cond, env).b) return exec_block(*i.then_block, env, ret);
                if (i.else_stmt) return exec(*i.else_stmt, env, ret);
                return Flow::Normal;
            }
            case StmtKind::While: {
                auto& w = static_cast<WhileStmt&>(s);
                while (eval(*w.cond, env).b) {
                    Flow f = loop_body(*w.body, env, ret);
                    if (f == Flow::Break) break;
                    if (f == Flow::Return) return f;
                }
                return Flow::Normal;
            }
            case StmtKind::ForRange: {
                auto& f = static_cast<ForRangeStmt&>(s);
                int64_t i = eval(*f.start, env).i;
                int64_t end = eval(*f.end, env).i;
                while (i < end) {
                    step();
                    env[f.sym] = std::make_shared<RVal>(mk_int(i));
                    Flow fl = loop_body(*f.body, env, ret);
                    if (fl == Flow::Break) break;
                    if (fl == Flow::Return) return fl;
                    if (i == INT64_MAX) err("integer overflow");
                    ++i;
                }
                return Flow::Normal;
            }
            case StmtKind::ForEach: {
                auto& f = static_cast<ForEachStmt&>(s);
                RVal arr = eval(*f.iterable, env);
                for (size_t i = 0; i < arr.a->items.size(); ++i) {
                    step();
                    env[f.sym] = std::make_shared<RVal>(arr.a->items[i]);
                    Flow fl = loop_body(*f.body, env, ret);
                    if (fl == Flow::Break) break;
                    if (fl == Flow::Return) return fl;
                }
                return Flow::Normal;
            }
            case StmtKind::Break: return Flow::Break;
            case StmtKind::Continue: return Flow::Continue;
            case StmtKind::Return: {
                auto& r = static_cast<ReturnStmt&>(s);
                ret = r.value ? eval(*r.value, env) : mk_unit();
                return Flow::Return;
            }
            case StmtKind::Assign: assign(static_cast<AssignStmt&>(s), env); return Flow::Normal;
            case StmtKind::ExprStmt: eval(*static_cast<ExprStmt&>(s).expr, env); return Flow::Normal;
            case StmtKind::BlockStmt: return exec_block(*static_cast<BlockStmt&>(s).block, env, ret);
        }
        return Flow::Normal;
    }

    // A loop body; break/continue inside a match-arm block arrive as signals.
    Flow loop_body(Block& b, Env& env, RVal& ret) {
        try {
            return exec_block(b, env, ret);
        } catch (BreakSignal&) {
            return Flow::Break;
        } catch (ContinueSignal&) {
            return Flow::Continue;
        }
    }

    void assign(AssignStmt& a, Env& env) {
        Expr& t = *a.target;
        switch (t.kind) {
            case ExprKind::Ident: {
                Symbol* s = static_cast<IdentExpr&>(t).sym;
                RVal v;
                if (a.compound) {
                    RVal old = load(s, env);
                    RVal rhs = eval(*a.value, env);
                    v = binop(a.op, old, rhs);
                } else {
                    v = eval(*a.value, env);
                }
                store(s, v, env);
                return;
            }
            case ExprKind::Index: {
                auto& ix = static_cast<IndexExpr&>(t);
                RVal arr = eval(*ix.object, env);
                int64_t i = eval(*ix.index, env).i;
                RVal v;
                if (a.compound) {
                    RVal old = index(arr, i);
                    RVal rhs = eval(*a.value, env);
                    v = binop(a.op, old, rhs);
                } else {
                    v = eval(*a.value, env);
                }
                check_index(i, arr.a->items.size());
                arr.a->items[static_cast<size_t>(i)] = v;
                return;
            }
            case ExprKind::Field: {
                auto& fe = static_cast<FieldExpr&>(t);
                RVal obj = eval(*fe.object, env);
                RVal v;
                if (a.compound) {
                    RVal old = obj.st->fields[static_cast<size_t>(fe.field_index)];
                    RVal rhs = eval(*a.value, env);
                    v = binop(a.op, old, rhs);
                } else {
                    v = eval(*a.value, env);
                }
                obj.st->fields[static_cast<size_t>(fe.field_index)] = v;
                return;
            }
            default: err("internal: bad assignment target");
        }
    }

    void check_index(int64_t i, size_t len) {
        if (i < 0 || static_cast<uint64_t>(i) >= len)
            err("index out of bounds: index " + std::to_string(i) + " for length " + std::to_string(len));
    }

    RVal index(const RVal& arr, int64_t i) {
        check_index(i, arr.a->items.size());
        return arr.a->items[static_cast<size_t>(i)];
    }

    // ---------------------------------------------------------- expressions
    RVal binop(BinaryOp op, const RVal& l, const RVal& r) {
        auto cmpres = [&](int c) {
            switch (op) {
                case BinaryOp::Eq: return c == 0;
                case BinaryOp::Ne: return c != 0;
                case BinaryOp::Lt: return c < 0;
                case BinaryOp::Le: return c <= 0;
                case BinaryOp::Gt: return c > 0;
                default: return c >= 0;
            }
        };
        switch (l.k) {
            case RVal::K::Int: {
                int64_t a = l.i, b = r.i, o = 0;
                switch (op) {
                    case BinaryOp::Add: if (__builtin_add_overflow(a, b, &o)) err("integer overflow"); return mk_int(o);
                    case BinaryOp::Sub: if (__builtin_sub_overflow(a, b, &o)) err("integer overflow"); return mk_int(o);
                    case BinaryOp::Mul: if (__builtin_mul_overflow(a, b, &o)) err("integer overflow"); return mk_int(o);
                    case BinaryOp::Div:
                        if (b == 0) err("division by zero");
                        if (a == INT64_MIN && b == -1) err("integer overflow");
                        return mk_int(a / b);
                    case BinaryOp::Mod:
                        if (b == 0) err("division by zero");
                        return mk_int(b == -1 ? 0 : a % b);
                    default: return mk_bool(cmpres(a < b ? -1 : a > b ? 1 : 0));
                }
            }
            case RVal::K::Float: {
                double a = l.f, b = r.f;
                switch (op) {
                    case BinaryOp::Add: return mk_float(a + b);
                    case BinaryOp::Sub: return mk_float(a - b);
                    case BinaryOp::Mul: return mk_float(a * b);
                    case BinaryOp::Div: return mk_float(a / b);
                    case BinaryOp::Eq: return mk_bool(a == b);
                    case BinaryOp::Ne: return mk_bool(a != b);
                    case BinaryOp::Lt: return mk_bool(a < b);
                    case BinaryOp::Le: return mk_bool(a <= b);
                    case BinaryOp::Gt: return mk_bool(a > b);
                    case BinaryOp::Ge: return mk_bool(a >= b);
                    default: break;
                }
                break;
            }
            case RVal::K::Str:
                if (op == BinaryOp::Add) {
                    check_string(l.s->size() + r.s->size());
                    return mk_str(*l.s + *r.s);
                }
                return mk_bool(cmpres(l.s->compare(*r.s)));
            case RVal::K::Bool: return mk_bool(op == BinaryOp::Eq ? l.b == r.b : l.b != r.b);
            case RVal::K::Unit: return mk_bool(op == BinaryOp::Eq);
            default: break;
        }
        err("internal: bad binary operation");
    }

    RVal eval(Expr& e, Env& env) {
        step();
        switch (e.kind) {
            case ExprKind::IntLit: return mk_int(static_cast<IntLit&>(e).value);
            case ExprKind::FloatLit: return mk_float(static_cast<FloatLit&>(e).value);
            case ExprKind::BoolLit: return mk_bool(static_cast<BoolLit&>(e).value);
            case ExprKind::StringLit: return mk_str(static_cast<StringLit&>(e).value);
            case ExprKind::UnitLit: return mk_unit();
            case ExprKind::Ident: return load(static_cast<IdentExpr&>(e).sym, env);
            case ExprKind::Unary: {
                auto& u = static_cast<UnaryExpr&>(e);
                RVal v = eval(*u.operand, env);
                if (u.op == UnaryOp::Not) return mk_bool(!v.b);
                if (v.k == RVal::K::Int) {
                    if (v.i == INT64_MIN) err("integer overflow");
                    return mk_int(-v.i);
                }
                return mk_float(-v.f);
            }
            case ExprKind::Binary: {
                auto& b = static_cast<BinaryExpr&>(e);
                RVal l = eval(*b.lhs, env);
                RVal r = eval(*b.rhs, env);
                return binop(b.op, l, r);
            }
            case ExprKind::Logical: {
                auto& l = static_cast<LogicalExpr&>(e);
                bool a = eval(*l.lhs, env).b;
                if (l.op == LogicalOp::And) return mk_bool(a ? eval(*l.rhs, env).b : false);
                return mk_bool(a ? true : eval(*l.rhs, env).b);
            }
            case ExprKind::Call: return eval_call(static_cast<CallExpr&>(e), env);
            case ExprKind::ModuleCall: {
                auto& mc = static_cast<ModuleCallExpr&>(e);
                std::vector<RVal> args;
                for (auto& a : mc.args) args.push_back(eval(*a, env));
                return module_call(mc.module + "." + mc.function, args);
            }
            case ExprKind::Index: {
                auto& ix = static_cast<IndexExpr&>(e);
                RVal o = eval(*ix.object, env);
                int64_t i = eval(*ix.index, env).i;
                if (o.k == RVal::K::Str) {
                    check_index(i, o.s->size());
                    return mk_str(std::string(1, (*o.s)[static_cast<size_t>(i)]));
                }
                return index(o, i);
            }
            case ExprKind::Field: {
                auto& fe = static_cast<FieldExpr&>(e);
                RVal o = eval(*fe.object, env);
                return o.st->fields[static_cast<size_t>(fe.field_index)];
            }
            case ExprKind::ArrayLit: {
                auto& a = static_cast<ArrayLit&>(e);
                RVal v;
                v.k = RVal::K::Arr;
                v.a = std::make_shared<ArrV>();
                arrs_.push_back(v.a);
                for (auto& x : a.elems) v.a->items.push_back(eval(*x, env));
                return v;
            }
            case ExprKind::ArrayRepeat: {
                auto& r = static_cast<ArrayRepeat&>(e);
                RVal x = eval(*r.value, env);
                int64_t n = eval(*r.count, env).i;
                if (n < 0) err("invalid argument: negative array length " + std::to_string(n));
                if (n > (16 << 20))
                    err("value too large: array of " + std::to_string(n) + " elements exceeds the limit of 16777216");
                RVal v;
                v.k = RVal::K::Arr;
                v.a = std::make_shared<ArrV>();
                arrs_.push_back(v.a);
                v.a->items.assign(static_cast<size_t>(n), x);
                return v;
            }
            case ExprKind::StructLit: {
                auto& s = static_cast<StructLit&>(e);
                RVal v;
                v.k = RVal::K::Struct;
                v.st = std::make_shared<StructV>();
                structs_.push_back(v.st);
                v.st->decl = s.decl;
                v.st->fields.resize(s.decl->fields.size());
                for (auto& fi : s.fields) {
                    RVal x = eval(*fi.value, env);
                    for (size_t k = 0; k < s.decl->fields.size(); ++k)
                        if (s.decl->fields[k].name == fi.name) v.st->fields[k] = x;
                }
                return v;
            }
            case ExprKind::EnumCtor: {
                auto& c = static_cast<EnumCtor&>(e);
                RVal v;
                v.k = RVal::K::Enum;
                v.e = std::make_shared<EnumV>();
                enums_.push_back(v.e);
                v.e->decl = c.decl;
                v.e->variant = c.variant_index;
                for (auto& a : c.args) v.e->fields.push_back(eval(*a, env));
                return v;
            }
            case ExprKind::Lambda: return make_closure(*static_cast<LambdaExpr&>(e).fn, env);
            case ExprKind::Match: return eval_match(static_cast<MatchExpr&>(e), env);
        }
        err("internal: unknown expression");
    }

    RVal eval_match(MatchExpr& m, Env& env) {
        RVal s = eval(*m.scrutinee, env);
        for (auto& arm : m.arms) {
            Pattern& p = arm.pattern;
            bool hit = false;
            switch (p.kind) {
                case PatternKind::Wildcard: hit = true; break;
                case PatternKind::Int: hit = s.i == p.int_value; break;
                case PatternKind::Bool: hit = s.b == p.bool_value; break;
                case PatternKind::String: hit = *s.s == p.string_value; break;
                case PatternKind::Variant: hit = s.e->variant == p.variant_index; break;
            }
            if (!hit) continue;
            if (p.kind == PatternKind::Variant)
                for (size_t i = 0; i < p.binding_syms.size(); ++i)
                    if (p.binding_syms[i]) env[p.binding_syms[i]] = std::make_shared<RVal>(s.e->fields[i]);
            if (arm.block) {
                RVal ret = mk_unit();
                Flow f = exec_block(*arm.block, env, ret);
                if (f == Flow::Return) throw ReturnSignal{ret};
                if (f == Flow::Break) throw BreakSignal{};
                if (f == Flow::Continue) throw ContinueSignal{};
                return mk_unit();
            }
            return eval(*arm.expr, env);
        }
        err("internal: no match arm");
    }

    RVal eval_call(CallExpr& c, Env& env) {
        if (c.callee->kind == ExprKind::Ident) {
            Symbol* s = static_cast<IdentExpr&>(*c.callee).sym;
            if (s->kind == SymbolKind::Builtin) return builtin(c, s->builtin, env);
            if (s->kind == SymbolKind::Function) {
                std::vector<RVal> args;
                for (auto& a : c.args) args.push_back(eval(*a, env));
                return call_fn(*s->fn, Env{}, std::move(args));
            }
        }
        RVal f = eval(*c.callee, env);
        std::vector<RVal> args;
        for (auto& a : c.args) args.push_back(eval(*a, env));
        return call_fn(*f.fn->decl, f.fn->captured, std::move(args));
    }

    RVal call_fn(FnDecl& fn, Env env, std::vector<RVal> args) {
        try {
            return call(fn, std::move(env), std::move(args));
        } catch (ReturnSignal& r) {
            --depth_;
            return r.v;
        }
    }

    RVal builtin(CallExpr& c, Builtin b, Env& env) {
        std::vector<RVal> a;
        for (auto& x : c.args) a.push_back(eval(*x, env));
        switch (b) {
            case Builtin::Print: out_ << fmt(a[0]) << "\n"; return mk_unit();
            case Builtin::Str: return mk_str(fmt(a[0]));
            case Builtin::Input: return mk_str("");
            case Builtin::Len:
                return mk_int(a[0].k == RVal::K::Str ? static_cast<int64_t>(a[0].s->size())
                                                     : static_cast<int64_t>(a[0].a->items.size()));
            case Builtin::Push:
                if (a[0].a->items.size() + 1 > (16u << 20))
                    err("value too large: array of " + std::to_string(a[0].a->items.size() + 1) +
                        " elements exceeds the limit of 16777216");
                a[0].a->items.push_back(a[1]);
                return mk_unit();
            case Builtin::Pop: {
                if (a[0].a->items.empty()) err("empty array: pop from an empty array");
                RVal v = a[0].a->items.back();
                a[0].a->items.pop_back();
                return v;
            }
            case Builtin::Int: {
                double t = std::trunc(a[0].f);
                if (!(t >= -9223372036854775808.0 && t < 9223372036854775808.0))
                    err("invalid conversion: " + fmt(a[0]) + " does not fit in Int");
                return mk_int(static_cast<int64_t>(t));
            }
            case Builtin::Float: return mk_float(static_cast<double>(a[0].i));
            case Builtin::ParseInt: {
                auto v = parse_int(*a[0].s);
                if (!v) {
                    std::string shown = a[0].s->size() > 40 ? a[0].s->substr(0, 40) + "..." : *a[0].s;
                    err("invalid conversion: \"" + shown + "\" is not an integer");
                }
                return mk_int(*v);
            }
            case Builtin::Panic: err("panic: " + *a[0].s);
            case Builtin::Assert:
                if (!a[0].b) err("assertion failed: " + *a[1].s);
                return mk_unit();
        }
        err("internal: unknown builtin");
    }

    RVal module_call(const std::string& name, std::vector<RVal>& a) {
        if (name == "math.abs") {
            if (a[0].i == INT64_MIN) err("integer overflow");
            return mk_int(a[0].i < 0 ? -a[0].i : a[0].i);
        }
        if (name == "math.fabs") return mk_float(std::fabs(a[0].f));
        if (name == "math.min") return mk_int(a[0].i < a[1].i ? a[0].i : a[1].i);
        if (name == "math.max") return mk_int(a[0].i > a[1].i ? a[0].i : a[1].i);
        if (name == "math.sqrt") return mk_float(std::sqrt(a[0].f));
        if (name == "math.pow") return mk_float(std::pow(a[0].f, a[1].f));
        if (name == "math.floor") return mk_float(std::floor(a[0].f));
        if (name == "math.ceil") return mk_float(std::ceil(a[0].f));
        if (name == "strings.substr") {
            const std::string& s = *a[0].s;
            int64_t st = a[1].i, len = a[2].i;
            if (st < 0 || len < 0 || st > static_cast<int64_t>(s.size()) || len > static_cast<int64_t>(s.size()) - st)
                err("index out of bounds: substr(" + std::to_string(st) + ", " + std::to_string(len) +
                    ") of a string of length " + std::to_string(s.size()));
            return mk_str(s.substr(static_cast<size_t>(st), static_cast<size_t>(len)));
        }
        if (name == "strings.find") {
            size_t p = a[0].s->find(*a[1].s);
            return mk_int(p == std::string::npos ? -1 : static_cast<int64_t>(p));
        }
        if (name == "strings.upper" || name == "strings.lower") {
            std::string s = *a[0].s;
            bool up = name == "strings.upper";
            for (char& ch : s) {
                if (up && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
                if (!up && ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
            }
            return mk_str(s);
        }
        if (name == "strings.split") {
            RVal v;
            v.k = RVal::K::Arr;
            v.a = std::make_shared<ArrV>();
            arrs_.push_back(v.a);
            const std::string& s = *a[0].s;
            const std::string& sep = *a[1].s;
            if (sep.empty()) {
                for (char ch : s) v.a->items.push_back(mk_str(std::string(1, ch)));
            } else {
                size_t pos = 0;
                for (;;) {
                    size_t q = s.find(sep, pos);
                    if (q == std::string::npos) {
                        v.a->items.push_back(mk_str(s.substr(pos)));
                        break;
                    }
                    v.a->items.push_back(mk_str(s.substr(pos, q - pos)));
                    pos = q + sep.size();
                }
            }
            check_array(v.a->items.size());
            return v;
        }
        if (name == "strings.join") {
            std::string out;
            for (size_t i = 0; i < a[0].a->items.size(); ++i) {
                if (i) out += *a[1].s;
                out += *a[0].a->items[i].s;
                check_string(out.size());
            }
            return mk_str(out);
        }
        if (name == "strings.char_code") {
            check_index(a[1].i, a[0].s->size());
            return mk_int(static_cast<unsigned char>((*a[0].s)[static_cast<size_t>(a[1].i)]));
        }
        if (name == "strings.from_char_code") {
            if (a[0].i < 0 || a[0].i > 255)
                err("invalid argument: character code " + std::to_string(a[0].i) + " is not in 0..255");
            return mk_str(std::string(1, static_cast<char>(a[0].i)));
        }
        if (name == "strings.repeat") {
            int64_t n = a[1].i;
            if (n < 0) err("invalid argument: negative repeat count " + std::to_string(n));
            if (!a[0].s->empty() && static_cast<uint64_t>(n) > kMaxString / a[0].s->size())
                err("value too large: repeated string would exceed the limit of " + std::to_string(kMaxString) + " bytes");
            std::string out;
            for (int64_t i = 0; i < n; ++i) out += *a[0].s;
            return mk_str(out);
        }
        if (name == "time.clock") return mk_float(0.0);
        err("internal: unknown module function " + name);
    }

    Program& prog_;
    SemaResult& sema_;
    std::ostream& out_;
    RefOptions opts_;
    std::vector<Cell> globals_;
    std::vector<std::weak_ptr<FnV>> fns_;
    std::vector<std::weak_ptr<ArrV>> arrs_;
    std::vector<std::weak_ptr<StructV>> structs_;
    std::vector<std::weak_ptr<EnumV>> enums_;
    uint64_t steps_ = 0;
    uint32_t depth_ = 0;
};

}  // namespace

RefResult run_reference(Program& prog, SemaResult& sema, std::ostream& out, const RefOptions& opts) {
    RefResult r;
    Interp in(prog, sema, out, opts);
    try {
        in.run();
    } catch (const RuntimeErr& e) {
        r.ok = false;
        r.error = e.msg;
    } catch (const LimitErr&) {
        r.ok = false;
        r.limit_hit = true;
        r.error = "step limit";
    }
    return r;
}

namespace {
struct ThreadArgs {
    Program* prog;
    SemaResult* sema;
    std::ostream* out;
    RefOptions opts;
    RefResult result;
};
void* thread_main(void* p) {
    auto* a = static_cast<ThreadArgs*>(p);
    a->result = run_reference(*a->prog, *a->sema, *a->out, a->opts);
    return nullptr;
}
}  // namespace

RefResult run_reference_big_stack(Program& prog, SemaResult& sema, std::ostream& out, const RefOptions& opts) {
    ThreadArgs args{&prog, &sema, &out, opts, {}};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024ul * 1024 * 1024);  // reserved, not committed
    pthread_t t;
    if (pthread_create(&t, &attr, thread_main, &args) != 0) {
        pthread_attr_destroy(&attr);
        return run_reference(prog, sema, out, opts);
    }
    pthread_join(t, nullptr);
    pthread_attr_destroy(&attr);
    return args.result;
}

}  // namespace ember

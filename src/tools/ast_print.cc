#include "tools/ast_print.h"

#include <sstream>

#include "common/numfmt.h"
#include "sema/types.h"

namespace ember {

namespace {

class Printer {
public:
    explicit Printer(bool loc) : loc_(loc) {}
    std::ostringstream o;

    void program(const Program& p) {
        for (const auto& it : p.items) item(it);
    }

private:
    void head(int d, const std::string& s, const Span& sp, const Type* t = nullptr) {
        o << std::string(static_cast<size_t>(d) * 2, ' ') << s;
        if (t) o << " [" << type_name(t) << "]";
        if (loc_ && sp.valid()) o << "  @" << sp.line << ":" << sp.column;
        o << "\n";
    }

    std::string type_expr(const TypeExpr* t) {
        if (!t) return "()";
        switch (t->kind) {
            case TypeExprKind::Named: return t->name;
            case TypeExprKind::Array: return "[" + type_expr(t->elem.get()) + "]";
            case TypeExprKind::Func: {
                std::string s = "fn(";
                for (size_t i = 0; i < t->params.size(); ++i) s += (i ? ", " : "") + type_expr(t->params[i].get());
                return s + ") -> " + type_expr(t->ret.get());
            }
        }
        return "?";
    }

    void fn(int d, const FnDecl& f, const char* what) {
        std::string s = std::string(what) + " " + (f.name.empty() ? "<lambda>" : f.name) + "(";
        for (size_t i = 0; i < f.params.size(); ++i)
            s += (i ? ", " : "") + f.params[i].name + ": " + type_expr(f.params[i].type.get());
        s += ") -> " + type_expr(f.ret.get());
        head(d, s, f.span);
        block(d + 1, *f.body);
    }

    void item(const Item& it) {
        switch (it.kind) {
            case ItemKind::Fn: fn(0, *it.fn, "Fn"); break;
            case ItemKind::Struct: {
                std::string s = "Struct " + it.struct_decl->name + " {";
                for (size_t i = 0; i < it.struct_decl->fields.size(); ++i)
                    s += (i ? ", " : " ") + it.struct_decl->fields[i].name + ": " + type_expr(it.struct_decl->fields[i].type.get());
                head(0, s + " }", it.struct_decl->span);
                break;
            }
            case ItemKind::Enum: {
                std::string s = "Enum " + it.enum_decl->name + " {";
                for (size_t i = 0; i < it.enum_decl->variants.size(); ++i) {
                    const auto& v = it.enum_decl->variants[i];
                    s += (i ? ", " : " ") + v.name;
                    if (!v.payload.empty()) {
                        s += "(";
                        for (size_t k = 0; k < v.payload.size(); ++k) s += (k ? ", " : "") + type_expr(v.payload[k].get());
                        s += ")";
                    }
                }
                head(0, s + " }", it.enum_decl->span);
                break;
            }
            case ItemKind::Const:
                head(0, "Const " + it.const_decl->name + ": " + type_expr(it.const_decl->type.get()), it.const_decl->span);
                expr(1, *it.const_decl->value);
                break;
            case ItemKind::Global:
                head(0, std::string(it.global->mutable_ ? "Var " : "Let ") + it.global->name +
                            (it.global->type ? ": " + type_expr(it.global->type.get()) : ""),
                     it.global->span);
                expr(1, *it.global->init);
                break;
            case ItemKind::Import: head(0, "Import " + it.import->module, it.import->span); break;
        }
    }

    void block(int d, const Block& b) {
        head(d, "Block", b.span);
        for (const auto& s : b.stmts) stmt(d + 1, *s);
    }

    void stmt(int d, const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                auto& l = static_cast<const LetStmt&>(s);
                head(d, std::string(l.mutable_ ? "Var " : "Let ") + l.name + (l.type ? ": " + type_expr(l.type.get()) : ""), s.span);
                expr(d + 1, *l.init);
                break;
            }
            case StmtKind::Fn: fn(d, *static_cast<const FnStmt&>(s).fn, "Fn"); break;
            case StmtKind::If: {
                auto& i = static_cast<const IfStmt&>(s);
                head(d, "If", s.span);
                expr(d + 1, *i.cond);
                block(d + 1, *i.then_block);
                if (i.else_stmt) {
                    head(d + 1, "Else", i.else_stmt->span);
                    stmt(d + 2, *i.else_stmt);
                }
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<const WhileStmt&>(s);
                head(d, "While", s.span);
                expr(d + 1, *w.cond);
                block(d + 1, *w.body);
                break;
            }
            case StmtKind::ForRange: {
                auto& f = static_cast<const ForRangeStmt&>(s);
                head(d, "ForRange " + f.var, s.span);
                expr(d + 1, *f.start);
                expr(d + 1, *f.end);
                block(d + 1, *f.body);
                break;
            }
            case StmtKind::ForEach: {
                auto& f = static_cast<const ForEachStmt&>(s);
                head(d, "ForEach " + f.var, s.span);
                expr(d + 1, *f.iterable);
                block(d + 1, *f.body);
                break;
            }
            case StmtKind::Break: head(d, "Break", s.span); break;
            case StmtKind::Continue: head(d, "Continue", s.span); break;
            case StmtKind::Return: {
                auto& r = static_cast<const ReturnStmt&>(s);
                head(d, "Return", s.span);
                if (r.value) expr(d + 1, *r.value);
                break;
            }
            case StmtKind::Assign: {
                auto& a = static_cast<const AssignStmt&>(s);
                head(d, a.compound ? std::string("Assign ") + binary_op_str(a.op) + "=" : "Assign", s.span);
                expr(d + 1, *a.target);
                expr(d + 1, *a.value);
                break;
            }
            case StmtKind::ExprStmt:
                head(d, "ExprStmt", s.span);
                expr(d + 1, *static_cast<const ExprStmt&>(s).expr);
                break;
            case StmtKind::BlockStmt: block(d, *static_cast<const BlockStmt&>(s).block); break;
        }
    }

    void expr(int d, const Expr& e) {
        const Type* t = e.type;
        switch (e.kind) {
            case ExprKind::IntLit: head(d, "Int " + std::to_string(static_cast<const IntLit&>(e).value), e.span, t); break;
            case ExprKind::FloatLit: head(d, "Float " + format_float(static_cast<const FloatLit&>(e).value), e.span, t); break;
            case ExprKind::BoolLit: head(d, static_cast<const BoolLit&>(e).value ? "Bool true" : "Bool false", e.span, t); break;
            case ExprKind::StringLit: {
                std::string v = static_cast<const StringLit&>(e).value;
                std::string q;
                for (char c : v) {
                    if (c == '\n') q += "\\n";
                    else if (c == '"') q += "\\\"";
                    else q += c;
                }
                head(d, "String \"" + q + "\"", e.span, t);
                break;
            }
            case ExprKind::UnitLit: head(d, "Unit", e.span, t); break;
            case ExprKind::Ident: head(d, "Ident " + static_cast<const IdentExpr&>(e).name, e.span, t); break;
            case ExprKind::Unary: {
                auto& u = static_cast<const UnaryExpr&>(e);
                head(d, u.op == UnaryOp::Neg ? "Neg" : "Not", e.span, t);
                expr(d + 1, *u.operand);
                break;
            }
            case ExprKind::Binary: {
                auto& b = static_cast<const BinaryExpr&>(e);
                head(d, std::string("Binary ") + binary_op_str(b.op), e.span, t);
                expr(d + 1, *b.lhs);
                expr(d + 1, *b.rhs);
                break;
            }
            case ExprKind::Logical: {
                auto& l = static_cast<const LogicalExpr&>(e);
                head(d, l.op == LogicalOp::And ? "And" : "Or", e.span, t);
                expr(d + 1, *l.lhs);
                expr(d + 1, *l.rhs);
                break;
            }
            case ExprKind::Call: {
                auto& c = static_cast<const CallExpr&>(e);
                head(d, "Call", e.span, t);
                expr(d + 1, *c.callee);
                for (const auto& a : c.args) expr(d + 1, *a);
                break;
            }
            case ExprKind::ModuleCall: {
                auto& c = static_cast<const ModuleCallExpr&>(e);
                head(d, "ModuleCall " + c.module + "." + c.function, e.span, t);
                for (const auto& a : c.args) expr(d + 1, *a);
                break;
            }
            case ExprKind::Index: {
                auto& i = static_cast<const IndexExpr&>(e);
                head(d, "Index", e.span, t);
                expr(d + 1, *i.object);
                expr(d + 1, *i.index);
                break;
            }
            case ExprKind::Field: {
                auto& f = static_cast<const FieldExpr&>(e);
                head(d, "Field ." + f.field, e.span, t);
                expr(d + 1, *f.object);
                break;
            }
            case ExprKind::ArrayLit: {
                head(d, "Array", e.span, t);
                for (const auto& x : static_cast<const ArrayLit&>(e).elems) expr(d + 1, *x);
                break;
            }
            case ExprKind::ArrayRepeat: {
                auto& r = static_cast<const ArrayRepeat&>(e);
                head(d, "ArrayRepeat", e.span, t);
                expr(d + 1, *r.value);
                expr(d + 1, *r.count);
                break;
            }
            case ExprKind::StructLit: {
                auto& s = static_cast<const StructLit&>(e);
                head(d, "StructLit " + s.name, e.span, t);
                for (const auto& f : s.fields) {
                    head(d + 1, "." + f.name, f.span);
                    expr(d + 2, *f.value);
                }
                break;
            }
            case ExprKind::EnumCtor: {
                auto& c = static_cast<const EnumCtor&>(e);
                head(d, "EnumCtor " + c.enum_name + "::" + c.variant, e.span, t);
                for (const auto& a : c.args) expr(d + 1, *a);
                break;
            }
            case ExprKind::Lambda: fn(d, *static_cast<const LambdaExpr&>(e).fn, "Lambda"); break;
            case ExprKind::Match: {
                auto& m = static_cast<const MatchExpr&>(e);
                head(d, "Match", e.span, t);
                expr(d + 1, *m.scrutinee);
                for (const auto& arm : m.arms) {
                    head(d + 1, "Arm " + pattern(arm.pattern), arm.span);
                    if (arm.block) block(d + 2, *arm.block);
                    else expr(d + 2, *arm.expr);
                }
                break;
            }
        }
    }

    static std::string pattern(const Pattern& p) {
        switch (p.kind) {
            case PatternKind::Wildcard: return "_";
            case PatternKind::Int: return std::to_string(p.int_value);
            case PatternKind::Bool: return p.bool_value ? "true" : "false";
            case PatternKind::String: return "\"" + p.string_value + "\"";
            case PatternKind::Variant: {
                std::string s = p.enum_name + "::" + p.variant;
                if (!p.bindings.empty()) {
                    s += "(";
                    for (size_t i = 0; i < p.bindings.size(); ++i) s += (i ? ", " : "") + p.bindings[i];
                    s += ")";
                }
                return s;
            }
        }
        return "?";
    }

    bool loc_;
};

}  // namespace

std::string print_ast(const Program& prog, bool with_locations) {
    Printer p(with_locations);
    p.program(prog);
    return p.o.str();
}

}  // namespace ember

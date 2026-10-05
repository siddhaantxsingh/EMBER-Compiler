#include "tools/formatter.h"

#include <cstdio>
#include <sstream>

#include "common/diagnostics.h"
#include "common/numfmt.h"
#include "common/source.h"
#include "front/lexer.h"
#include "front/parser.h"

namespace ember {

namespace {

// Binding power of an expression as an operand (higher binds tighter).
enum Prec { kLowest = 0, kOr = 1, kAnd = 2, kEquality = 3, kCompare = 4, kTerm = 5, kFactor = 6, kUnary = 7, kPostfix = 8 };

int binop_prec(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq:
        case BinaryOp::Ne: return kEquality;
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge: return kCompare;
        case BinaryOp::Add:
        case BinaryOp::Sub: return kTerm;
        default: return kFactor;
    }
}

int expr_prec(const Expr& e) {
    switch (e.kind) {
        case ExprKind::Binary: return binop_prec(static_cast<const BinaryExpr&>(e).op);
        case ExprKind::Logical: return static_cast<const LogicalExpr&>(e).op == LogicalOp::And ? kAnd : kOr;
        case ExprKind::Unary: return kUnary;
        default: return kPostfix;
    }
}

std::string quote_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\0': out += "\\0"; break;
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            default:
                if (c < 32 || c == 127) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\x%02X", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

// Float literal that re-lexes to the same value ("1e+300" -> "1.0e+300").
std::string float_literal(double d) {
    std::string s = format_float(d);
    size_t e = s.find('e');
    if (e != std::string::npos && s.find('.') == std::string::npos) s.insert(e, ".0");
    return s;
}

// Whether a struct literal appears where `if`/`while`/`for`/`match` heads
// would misparse it (so the head must be parenthesized).
bool contains_struct_lit(const Expr& e) {
    switch (e.kind) {
        case ExprKind::StructLit: return true;
        case ExprKind::Unary: return contains_struct_lit(*static_cast<const UnaryExpr&>(e).operand);
        case ExprKind::Binary: {
            auto& b = static_cast<const BinaryExpr&>(e);
            return contains_struct_lit(*b.lhs) || contains_struct_lit(*b.rhs);
        }
        case ExprKind::Logical: {
            auto& b = static_cast<const LogicalExpr&>(e);
            return contains_struct_lit(*b.lhs) || contains_struct_lit(*b.rhs);
        }
        case ExprKind::Call: return contains_struct_lit(*static_cast<const CallExpr&>(e).callee);
        case ExprKind::Index: return contains_struct_lit(*static_cast<const IndexExpr&>(e).object);
        case ExprKind::Field: return contains_struct_lit(*static_cast<const FieldExpr&>(e).object);
        case ExprKind::Match: return contains_struct_lit(*static_cast<const MatchExpr&>(e).scrutinee);
        default: return false;  // inside (), [], call arguments: allowed
    }
}

class Formatter {
public:
    Formatter(const SourceFile& src, const std::vector<Comment>& comments) : src_(src), comments_(comments) {}

    std::string run(const Program& p) {
        uint32_t prev_end_line = 0;
        for (size_t i = 0; i < p.items.size(); ++i) {
            const Item& it = p.items[i];
            Span sp = item_span(it);
            flush_comments_before(sp.start, 0, prev_end_line);
            if (i > 0) {
                bool blank = has_blank_line_before(sp) || (is_block_item(it) || is_block_item(p.items[i - 1]));
                if (blank && !just_wrote_blank()) out_ += "\n";
            }
            item(it);
            prev_end_line = src_.line_of(sp.end > 0 ? sp.end - 1 : 0);
            trailing_comment(prev_end_line, i + 1 < p.items.size() ? item_span(p.items[i + 1]).start : UINT32_MAX);
            out_ += "\n";
        }
        flush_comments_before(UINT32_MAX, 0, prev_end_line);
        // Exactly one trailing newline.
        while (out_.size() >= 2 && out_[out_.size() - 1] == '\n' && out_[out_.size() - 2] == '\n') out_.pop_back();
        return out_;
    }

private:
    static bool is_block_item(const Item& it) {
        return it.kind == ItemKind::Fn || it.kind == ItemKind::Struct || it.kind == ItemKind::Enum;
    }

    static Span item_span(const Item& it) {
        switch (it.kind) {
            case ItemKind::Fn: return it.fn->span;
            case ItemKind::Struct: return it.struct_decl->span;
            case ItemKind::Enum: return it.enum_decl->span;
            case ItemKind::Const: return it.const_decl->span;
            case ItemKind::Global: return it.global->span;
            case ItemKind::Import: return it.import->span;
        }
        return Span{};
    }

    bool just_wrote_blank() const { return out_.size() >= 2 && out_.back() == '\n' && out_[out_.size() - 2] == '\n'; }

    // True if the source has an empty line right before the node (ignoring
    // comment lines that belong to it).
    bool has_blank_line_before(const Span& sp) const {
        uint32_t line = sp.line;
        // Walk upwards over comment-only lines.
        while (line > 1) {
            std::string_view text = src_.line_text(line - 1);
            size_t k = text.find_first_not_of(" \t\r");
            if (k == std::string_view::npos) return true;
            if (text.substr(k, 2) == "//") {
                --line;
                continue;
            }
            return false;
        }
        return false;
    }

    std::string ind(int level) const { return std::string(static_cast<size_t>(level) * 4, ' '); }

    void flush_comments_before(uint32_t offset, int level, uint32_t prev_end_line) {
        while (next_comment_ < comments_.size() && comments_[next_comment_].span.start < offset) {
            const Comment& c = comments_[next_comment_++];
            uint32_t cl = c.span.line;
            if (prev_end_line != 0 && cl > prev_end_line + 1) {
                std::string_view above = src_.line_text(cl - 1);
                if (above.find_first_not_of(" \t\r") == std::string_view::npos && !just_wrote_blank() && !out_.empty())
                    out_ += "\n";
            }
            out_ += ind(level) + strip(c.text) + "\n";
            prev_end_line = src_.line_of(c.span.end > 0 ? c.span.end - 1 : 0);
        }
    }

    static std::string strip(const std::string& s) {
        size_t e = s.find_last_not_of(" \t\r\n");
        return e == std::string::npos ? std::string() : s.substr(0, e + 1);
    }

    // An end-of-line comment on `line`, before `limit`, is appended.
    void trailing_comment(uint32_t line, uint32_t limit) {
        if (next_comment_ < comments_.size()) {
            const Comment& c = comments_[next_comment_];
            if (c.span.line == line && c.span.start < limit && c.text.rfind("//", 0) == 0) {
                out_ += " " + strip(c.text);
                ++next_comment_;
            }
        }
    }

    // ------------------------------------------------------------ items
    void item(const Item& it) {
        switch (it.kind) {
            case ItemKind::Fn: out_ += fn_decl(*it.fn, 0); break;
            case ItemKind::Struct: {
                const auto& s = *it.struct_decl;
                std::vector<std::string> fields;
                for (const auto& f : s.fields) fields.push_back(f.name + ": " + type(f.type.get()));
                out_ += "struct " + s.name + " " + braced_list(fields, 0);
                break;
            }
            case ItemKind::Enum: {
                const auto& e = *it.enum_decl;
                std::vector<std::string> vs;
                for (const auto& v : e.variants) {
                    std::string s = v.name;
                    if (!v.payload.empty()) {
                        s += "(";
                        for (size_t i = 0; i < v.payload.size(); ++i) s += (i ? ", " : "") + type(v.payload[i].get());
                        s += ")";
                    }
                    vs.push_back(s);
                }
                out_ += "enum " + e.name + " " + braced_list(vs, 0);
                break;
            }
            case ItemKind::Const: {
                const auto& c = *it.const_decl;
                out_ += "const " + c.name + (c.type ? ": " + type(c.type.get()) : "") + " = " + expr(*c.value, 0) + ";";
                break;
            }
            case ItemKind::Global: out_ += let(*it.global, 0); break;
            case ItemKind::Import: out_ += "import " + it.import->module + ";"; break;
        }
    }

    std::string braced_list(const std::vector<std::string>& parts, int level) {
        std::string one = "{";
        for (size_t i = 0; i < parts.size(); ++i) one += (i ? ", " : " ") + parts[i];
        one += parts.empty() ? "}" : " }";
        if (one.size() + static_cast<size_t>(level) * 4 <= 80) return one;
        std::string s = "{\n";
        for (const auto& p : parts) s += ind(level + 1) + p + ",\n";
        return s + ind(level) + "}";
    }

    std::string type(const TypeExpr* t) {
        if (!t) return "()";
        switch (t->kind) {
            case TypeExprKind::Named: return t->name;
            case TypeExprKind::Array: return "[" + type(t->elem.get()) + "]";
            case TypeExprKind::Func: {
                std::string s = "fn(";
                for (size_t i = 0; i < t->params.size(); ++i) s += (i ? ", " : "") + type(t->params[i].get());
                s += ")";
                if (t->ret) s += " -> " + type(t->ret.get());
                return s;
            }
        }
        return "?";
    }

    std::string signature(const FnDecl& f) {
        std::string s = "fn" + (f.name.empty() ? std::string() : " " + f.name) + "(";
        for (size_t i = 0; i < f.params.size(); ++i) s += (i ? ", " : "") + f.params[i].name + ": " + type(f.params[i].type.get());
        s += ")";
        if (f.ret) s += " -> " + type(f.ret.get());
        return s;
    }

    std::string fn_decl(const FnDecl& f, int level) { return signature(f) + " " + block(*f.body, level); }

    std::string let(const LetStmt& l, int level) {
        return std::string(l.mutable_ ? "var " : "let ") + l.name + (l.type ? ": " + type(l.type.get()) : "") + " = " +
               expr(*l.init, level) + ";";
    }

    // ------------------------------------------------------------ statements
    // A block, starting at "{" (no leading indentation) and ending at "}".
    std::string block(const Block& b, int level) {
        std::string saved;
        saved.swap(out_);
        out_ += "{\n";
        uint32_t prev_end_line = b.span.line;
        for (size_t i = 0; i < b.stmts.size(); ++i) {
            const Stmt& s = *b.stmts[i];
            flush_comments_before(s.span.start, level + 1, prev_end_line);
            if (i > 0 && has_blank_line_before(s.span) && !just_wrote_blank()) out_ += "\n";
            out_ += ind(level + 1);
            std::string text = stmt(s, level + 1);
            out_ += text;
            prev_end_line = src_.line_of(s.span.end > 0 ? s.span.end - 1 : 0);
            trailing_comment(prev_end_line, i + 1 < b.stmts.size() ? b.stmts[i + 1]->span.start : b.span.end);
            out_ += "\n";
        }
        flush_comments_before(b.span.end > 0 ? b.span.end - 1 : 0, level + 1, prev_end_line);
        out_ += ind(level) + "}";
        std::string result;
        result.swap(out_);
        out_.swap(saved);
        return result;
    }

    std::string head(const Expr& e, int level) {
        std::string s = expr(e, level);
        return contains_struct_lit(e) ? "(" + s + ")" : s;
    }

    std::string stmt(const Stmt& s, int level) {
        switch (s.kind) {
            case StmtKind::Let: return let(static_cast<const LetStmt&>(s), level);
            case StmtKind::Fn: return fn_decl(*static_cast<const FnStmt&>(s).fn, level);
            case StmtKind::If: {
                auto& i = static_cast<const IfStmt&>(s);
                std::string r = "if " + head(*i.cond, level) + " " + block(*i.then_block, level);
                if (i.else_stmt) {
                    r += " else ";
                    if (i.else_stmt->kind == StmtKind::If) r += stmt(*i.else_stmt, level);
                    else r += block(*static_cast<const BlockStmt&>(*i.else_stmt).block, level);
                }
                return r;
            }
            case StmtKind::While: {
                auto& w = static_cast<const WhileStmt&>(s);
                return "while " + head(*w.cond, level) + " " + block(*w.body, level);
            }
            case StmtKind::ForRange: {
                auto& f = static_cast<const ForRangeStmt&>(s);
                return "for " + f.var + " in " + head(*f.start, level) + ".." + head(*f.end, level) + " " + block(*f.body, level);
            }
            case StmtKind::ForEach: {
                auto& f = static_cast<const ForEachStmt&>(s);
                return "for " + f.var + " in " + head(*f.iterable, level) + " " + block(*f.body, level);
            }
            case StmtKind::Break: return "break;";
            case StmtKind::Continue: return "continue;";
            case StmtKind::Return: {
                auto& r = static_cast<const ReturnStmt&>(s);
                return r.value ? "return " + expr(*r.value, level) + ";" : "return;";
            }
            case StmtKind::Assign: {
                auto& a = static_cast<const AssignStmt&>(s);
                std::string op = a.compound ? std::string(" ") + binary_op_str(a.op) + "= " : " = ";
                return expr(*a.target, level) + op + expr(*a.value, level) + ";";
            }
            case StmtKind::ExprStmt: {
                const Expr& e = *static_cast<const ExprStmt&>(s).expr;
                if (e.kind == ExprKind::Match) return expr(e, level);
                return expr(e, level) + ";";
            }
            case StmtKind::BlockStmt: return block(*static_cast<const BlockStmt&>(s).block, level);
        }
        return "";
    }

    // ------------------------------------------------------------ expressions
    std::string operand(const Expr& e, int min_prec, int level) {
        std::string s = expr(e, level);
        return expr_prec(e) < min_prec ? "(" + s + ")" : s;
    }

    std::string args(const std::vector<ExprPtr>& v, int level) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + expr(*v[i], level);
        return s;
    }

    std::string expr(const Expr& e, int level) {
        switch (e.kind) {
            case ExprKind::IntLit: return std::to_string(static_cast<const IntLit&>(e).value);
            case ExprKind::FloatLit: return float_literal(static_cast<const FloatLit&>(e).value);
            case ExprKind::BoolLit: return static_cast<const BoolLit&>(e).value ? "true" : "false";
            case ExprKind::StringLit: return quote_string(static_cast<const StringLit&>(e).value);
            case ExprKind::UnitLit: return "()";
            case ExprKind::Ident: return static_cast<const IdentExpr&>(e).name;
            case ExprKind::Unary: {
                auto& u = static_cast<const UnaryExpr&>(e);
                std::string inner = operand(*u.operand, kUnary, level);
                // Avoid "--x" turning into a different token sequence if the
                // lexer ever grows a "--" operator: keep a space.
                std::string op = u.op == UnaryOp::Neg ? "-" : "!";
                if (u.op == UnaryOp::Neg && !inner.empty() && inner[0] == '-') op += " ";
                return op + inner;
            }
            case ExprKind::Binary: {
                auto& b = static_cast<const BinaryExpr&>(e);
                int p = binop_prec(b.op);
                return operand(*b.lhs, p, level) + " " + binary_op_str(b.op) + " " + operand(*b.rhs, p + 1, level);
            }
            case ExprKind::Logical: {
                auto& l = static_cast<const LogicalExpr&>(e);
                int p = l.op == LogicalOp::And ? kAnd : kOr;
                return operand(*l.lhs, p, level) + (l.op == LogicalOp::And ? " && " : " || ") + operand(*l.rhs, p + 1, level);
            }
            case ExprKind::Call: {
                auto& c = static_cast<const CallExpr&>(e);
                return operand(*c.callee, kPostfix, level) + "(" + args(c.args, level) + ")";
            }
            case ExprKind::ModuleCall: {
                auto& c = static_cast<const ModuleCallExpr&>(e);
                return c.module + "." + c.function + "(" + args(c.args, level) + ")";
            }
            case ExprKind::Index: {
                auto& i = static_cast<const IndexExpr&>(e);
                return operand(*i.object, kPostfix, level) + "[" + expr(*i.index, level) + "]";
            }
            case ExprKind::Field: {
                auto& f = static_cast<const FieldExpr&>(e);
                return operand(*f.object, kPostfix, level) + "." + f.field;
            }
            case ExprKind::ArrayLit: return "[" + args(static_cast<const ArrayLit&>(e).elems, level) + "]";
            case ExprKind::ArrayRepeat: {
                auto& r = static_cast<const ArrayRepeat&>(e);
                return "[" + expr(*r.value, level) + "; " + expr(*r.count, level) + "]";
            }
            case ExprKind::StructLit: {
                auto& s = static_cast<const StructLit&>(e);
                if (s.fields.empty()) return s.name + " {}";
                std::string r = s.name + " { ";
                for (size_t i = 0; i < s.fields.size(); ++i)
                    r += (i ? ", " : "") + s.fields[i].name + ": " + expr(*s.fields[i].value, level);
                return r + " }";
            }
            case ExprKind::EnumCtor: {
                auto& c = static_cast<const EnumCtor&>(e);
                std::string r = c.enum_name + "::" + c.variant;
                if (c.has_parens || !c.args.empty()) r += "(" + args(c.args, level) + ")";
                return r;
            }
            case ExprKind::Lambda: {
                const FnDecl& f = *static_cast<const LambdaExpr&>(e).fn;
                // Short single-return bodies stay on one line.
                if (f.body->stmts.size() == 1 && f.body->stmts[0]->kind == StmtKind::Return && !has_comment_inside(f.body->span)) {
                    std::string inner = stmt(*f.body->stmts[0], level + 1);
                    std::string one = signature(f) + " { " + inner + " }";
                    if (one.find('\n') == std::string::npos && one.size() <= 72) return one;
                }
                return signature(f) + " " + block(*f.body, level);
            }
            case ExprKind::Match: {
                auto& m = static_cast<const MatchExpr&>(e);
                std::string r = "match " + head(*m.scrutinee, level) + " {\n";
                for (const auto& arm : m.arms) {
                    flush_into(r, arm.span.start, level + 1);
                    r += ind(level + 1) + pattern(arm.pattern) + " => ";
                    if (arm.block) r += block(*arm.block, level + 1);
                    else r += expr(*arm.expr, level + 1);
                    r += ",\n";
                }
                flush_into(r, m.span.end > 0 ? m.span.end - 1 : 0, level + 1);
                return r + ind(level) + "}";
            }
        }
        return "";
    }

    bool has_comment_inside(const Span& sp) const {
        for (size_t i = next_comment_; i < comments_.size(); ++i) {
            if (comments_[i].span.start >= sp.end) break;
            if (comments_[i].span.start >= sp.start) return true;
        }
        return false;
    }

    // Comments before a match arm, written into the expression text.
    void flush_into(std::string& r, uint32_t offset, int level) {
        while (next_comment_ < comments_.size() && comments_[next_comment_].span.start < offset) {
            r += ind(level) + strip(comments_[next_comment_].text) + "\n";
            ++next_comment_;
        }
    }

    static std::string pattern(const Pattern& p) {
        switch (p.kind) {
            case PatternKind::Wildcard: return "_";
            case PatternKind::Int: return std::to_string(p.int_value);
            case PatternKind::Bool: return p.bool_value ? "true" : "false";
            case PatternKind::String: return quote_string(p.string_value);
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
        return "_";
    }

    const SourceFile& src_;
    const std::vector<Comment>& comments_;
    size_t next_comment_ = 0;
    std::string out_;
};

}  // namespace

FormatResult format_source(const std::string& name, const std::string& text) {
    FormatResult r;
    SourceFile src(name, text);
    Diagnostics diags;
    Lexer lexer(src, diags);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens), diags);
    Program prog = parser.parse_program();
    if (diags.has_errors()) {
        r.diagnostics = diags.render(src);
        return r;
    }
    Formatter f(src, lexer.comments());
    r.output = f.run(prog) + "\n";
    // Collapse the possible double newline at the end.
    while (r.output.size() >= 2 && r.output[r.output.size() - 1] == '\n' && r.output[r.output.size() - 2] == '\n')
        r.output.pop_back();
    r.ok = true;
    return r;
}

}  // namespace ember

#include "front/parser.h"

#include <algorithm>

namespace ember {

const char* binary_op_str(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Eq: return "==";
        case BinaryOp::Ne: return "!=";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
    }
    return "?";
}

namespace {

// Binding powers for binary operators (higher binds tighter).
enum Prec { kNone = 0, kOr = 1, kAnd = 2, kEquality = 3, kCompare = 4, kTerm = 5, kFactor = 6 };

int infix_prec(Tok t) {
    switch (t) {
        case Tok::OrOr: return kOr;
        case Tok::AndAnd: return kAnd;
        case Tok::EqEq: case Tok::BangEq: return kEquality;
        case Tok::Lt: case Tok::LtEq: case Tok::Gt: case Tok::GtEq: return kCompare;
        case Tok::Plus: case Tok::Minus: return kTerm;
        case Tok::Star: case Tok::Slash: case Tok::Percent: return kFactor;
        default: return kNone;
    }
}

BinaryOp to_binop(Tok t) {
    switch (t) {
        case Tok::Plus: return BinaryOp::Add;
        case Tok::Minus: return BinaryOp::Sub;
        case Tok::Star: return BinaryOp::Mul;
        case Tok::Slash: return BinaryOp::Div;
        case Tok::Percent: return BinaryOp::Mod;
        case Tok::EqEq: return BinaryOp::Eq;
        case Tok::BangEq: return BinaryOp::Ne;
        case Tok::Lt: return BinaryOp::Lt;
        case Tok::LtEq: return BinaryOp::Le;
        case Tok::Gt: return BinaryOp::Gt;
        default: return BinaryOp::Ge;
    }
}

bool is_compound_assign(Tok t, BinaryOp& op) {
    switch (t) {
        case Tok::PlusEq: op = BinaryOp::Add; return true;
        case Tok::MinusEq: op = BinaryOp::Sub; return true;
        case Tok::StarEq: op = BinaryOp::Mul; return true;
        case Tok::SlashEq: op = BinaryOp::Div; return true;
        case Tok::PercentEq: op = BinaryOp::Mod; return true;
        default: return false;
    }
}

bool is_place(const Expr& e) {
    return e.kind == ExprKind::Ident || e.kind == ExprKind::Index || e.kind == ExprKind::Field;
}

}  // namespace

Parser::DepthGuard::DepthGuard(Parser& parser) : p(parser) {
    if (++p.depth_ > kMaxDepth) {
        p.fail(p.peek(), "nesting is too deep (limit " + std::to_string(kMaxDepth) + ")");
    }
}

Parser::Parser(std::vector<Token> tokens, Diagnostics& diags) : diags_(diags) {
    // Error tokens were already reported by the lexer; dropping them lets
    // the parser carry on and find real syntax errors.
    toks_.reserve(tokens.size());
    for (auto& t : tokens)
        if (t.kind != Tok::Error) toks_.push_back(std::move(t));
    if (toks_.empty() || toks_.back().kind != Tok::Eof) toks_.push_back(Token{});
}

const Token& Parser::peek(int ahead) const {
    size_t i = std::min(pos_ + static_cast<size_t>(ahead), toks_.size() - 1);
    return toks_[i];
}

const Token& Parser::advance() {
    const Token& t = toks_[pos_];
    if (pos_ + 1 < toks_.size()) ++pos_;
    return t;
}

bool Parser::match(Tok k) {
    if (!check(k)) return false;
    advance();
    return true;
}

void Parser::fail(const Token& at, const std::string& msg, std::vector<std::string> notes) {
    diags_.error(DiagKind::Syntax, at.span, msg, std::move(notes));
    throw SyntaxError{};
}

const Token& Parser::expect(Tok k, const char* context) {
    if (check(k)) return advance();
    std::string msg = std::string("expected ") + tok_desc(k) + " " + context;
    fail(peek(), msg, {std::string("expected: ") + tok_desc(k), std::string("found: ") + tok_desc(peek().kind)});
}

// Skip to just after a ';' or to the start of something that begins a
// statement, or a closing brace.
void Parser::sync_stmt() {
    while (!check(Tok::Eof)) {
        if (match(Tok::Semicolon)) return;
        switch (peek().kind) {
            case Tok::RBrace: case Tok::KwLet: case Tok::KwVar: case Tok::KwFn: case Tok::KwIf:
            case Tok::KwWhile: case Tok::KwFor: case Tok::KwReturn: case Tok::KwBreak:
            case Tok::KwContinue: case Tok::KwStruct: case Tok::KwEnum: case Tok::KwConst:
                return;
            default: advance();
        }
    }
}

void Parser::sync_item() {
    int brace = 0;
    while (!check(Tok::Eof)) {
        Tok k = peek().kind;
        if (brace == 0 && (k == Tok::KwFn || k == Tok::KwStruct || k == Tok::KwEnum || k == Tok::KwConst ||
                           k == Tok::KwLet || k == Tok::KwVar || k == Tok::KwImport))
            return;
        if (k == Tok::LBrace) ++brace;
        if (k == Tok::RBrace) {
            if (brace > 0) --brace;
        }
        advance();
    }
}

// ------------------------------------------------------------ items

Program Parser::parse_program() {
    Program prog;
    while (!check(Tok::Eof) && !diags_.full()) {
        size_t before = pos_;
        try {
            parse_item(prog);
        } catch (const SyntaxError&) {
            if (pos_ == before) advance();
            sync_item();
        }
    }
    return prog;
}

void Parser::parse_item(Program& prog) {
    Item item;
    switch (peek().kind) {
        case Tok::KwFn:
            item.kind = ItemKind::Fn;
            item.fn = parse_fn(false);
            break;
        case Tok::KwStruct:
            item.kind = ItemKind::Struct;
            item.struct_decl = parse_struct();
            break;
        case Tok::KwEnum:
            item.kind = ItemKind::Enum;
            item.enum_decl = parse_enum();
            break;
        case Tok::KwConst:
            item.kind = ItemKind::Const;
            item.const_decl = parse_const();
            break;
        case Tok::KwLet:
        case Tok::KwVar:
            item.kind = ItemKind::Global;
            item.global = parse_let();
            break;
        case Tok::KwImport: {
            item.kind = ItemKind::Import;
            auto imp = std::make_unique<ImportDecl>();
            Span s = advance().span;
            const Token& name = expect(Tok::Ident, "after 'import'");
            imp->module = name.text;
            imp->span = Span::merge(s, name.span);
            expect(Tok::Semicolon, "after import");
            item.import = std::move(imp);
            break;
        }
        default:
            fail(peek(), std::string("expected an item (fn, struct, enum, const, let, var, import), found ") +
                             tok_desc(peek().kind));
    }
    prog.items.push_back(std::move(item));
}

std::unique_ptr<FnDecl> Parser::parse_fn(bool lambda) {
    DepthGuard g(*this);
    auto fn = std::make_unique<FnDecl>();
    Span start = expect(Tok::KwFn, "").span;
    if (!lambda) {
        const Token& name = expect(Tok::Ident, "for function name");
        fn->name = name.text;
        fn->name_span = name.span;
    }
    expect(Tok::LParen, "to start the parameter list");
    while (!check(Tok::RParen)) {
        Param p;
        const Token& pn = expect(Tok::Ident, "for parameter name");
        p.name = pn.text;
        p.span = pn.span;
        expect(Tok::Colon, "after parameter name (parameters need a type)");
        p.type = parse_type();
        fn->params.push_back(std::move(p));
        if (!match(Tok::Comma)) break;
    }
    expect(Tok::RParen, "to close the parameter list");
    if (match(Tok::Arrow)) fn->ret = parse_type();
    bool saved = no_struct_lit_;
    no_struct_lit_ = false;
    fn->body = parse_block();
    no_struct_lit_ = saved;
    fn->span = Span::merge(start, fn->body->span);
    return fn;
}

std::unique_ptr<StructDecl> Parser::parse_struct() {
    auto s = std::make_unique<StructDecl>();
    Span start = advance().span;
    const Token& name = expect(Tok::Ident, "for struct name");
    s->name = name.text;
    s->name_span = name.span;
    expect(Tok::LBrace, "to start the struct body");
    while (!check(Tok::RBrace)) {
        FieldDecl f;
        const Token& fn = expect(Tok::Ident, "for field name");
        f.name = fn.text;
        f.span = fn.span;
        expect(Tok::Colon, "after field name");
        f.type = parse_type();
        s->fields.push_back(std::move(f));
        if (!match(Tok::Comma)) break;
    }
    Span end = expect(Tok::RBrace, "to close the struct body").span;
    s->span = Span::merge(start, end);
    return s;
}

std::unique_ptr<EnumDecl> Parser::parse_enum() {
    auto e = std::make_unique<EnumDecl>();
    Span start = advance().span;
    const Token& name = expect(Tok::Ident, "for enum name");
    e->name = name.text;
    e->name_span = name.span;
    expect(Tok::LBrace, "to start the enum body");
    while (!check(Tok::RBrace)) {
        VariantDecl v;
        const Token& vn = expect(Tok::Ident, "for variant name");
        v.name = vn.text;
        v.span = vn.span;
        if (match(Tok::LParen)) {
            while (!check(Tok::RParen)) {
                v.payload.push_back(parse_type());
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RParen, "to close the variant payload");
        }
        e->variants.push_back(std::move(v));
        if (!match(Tok::Comma)) break;
    }
    Span end = expect(Tok::RBrace, "to close the enum body").span;
    e->span = Span::merge(start, end);
    if (e->variants.empty()) diags_.error(DiagKind::Syntax, e->name_span, "enum must have at least one variant");
    return e;
}

std::unique_ptr<ConstDecl> Parser::parse_const() {
    auto c = std::make_unique<ConstDecl>();
    Span start = advance().span;
    const Token& name = expect(Tok::Ident, "for constant name");
    c->name = name.text;
    c->name_span = name.span;
    if (match(Tok::Colon)) c->type = parse_type();
    expect(Tok::Eq, "in constant declaration");
    c->value = parse_expr();
    Span end = expect(Tok::Semicolon, "after constant declaration").span;
    c->span = Span::merge(start, end);
    return c;
}

std::unique_ptr<LetStmt> Parser::parse_let() {
    Span start = peek().span;
    auto let = std::make_unique<LetStmt>(start);
    let->mutable_ = advance().kind == Tok::KwVar;
    const Token& name = expect(Tok::Ident, "for variable name");
    let->name = name.text;
    let->name_span = name.span;
    if (match(Tok::Colon)) let->type = parse_type();
    expect(Tok::Eq, "(variables must be initialized)");
    let->init = parse_expr();
    Span end = expect(Tok::Semicolon, "after variable declaration").span;
    let->span = Span::merge(start, end);
    return let;
}

std::unique_ptr<TypeExpr> Parser::parse_type() {
    DepthGuard g(*this);
    auto t = std::make_unique<TypeExpr>();
    t->span = peek().span;
    if (match(Tok::LBracket)) {
        t->kind = TypeExprKind::Array;
        t->elem = parse_type();
        t->span = Span::merge(t->span, expect(Tok::RBracket, "to close the array type").span);
    } else if (match(Tok::KwFn)) {
        t->kind = TypeExprKind::Func;
        expect(Tok::LParen, "in function type");
        while (!check(Tok::RParen)) {
            t->params.push_back(parse_type());
            if (!match(Tok::Comma)) break;
        }
        t->span = Span::merge(t->span, expect(Tok::RParen, "to close the function type").span);
        if (match(Tok::Arrow)) {
            t->ret = parse_type();
            t->span = Span::merge(t->span, t->ret->span);
        }
    } else if (check(Tok::Ident)) {
        t->kind = TypeExprKind::Named;
        t->name = advance().text;
    } else {
        fail(peek(), std::string("expected a type, found ") + tok_desc(peek().kind));
    }
    return t;
}

// ------------------------------------------------------------ statements

std::unique_ptr<Block> Parser::parse_block() {
    DepthGuard g(*this);
    auto b = std::make_unique<Block>();
    Span start = expect(Tok::LBrace, "to start a block").span;
    bool saved = no_struct_lit_;
    no_struct_lit_ = false;
    while (!check(Tok::RBrace) && !check(Tok::Eof) && !diags_.full()) {
        size_t before = pos_;
        try {
            b->stmts.push_back(parse_stmt());
        } catch (const SyntaxError&) {
            if (pos_ == before) advance();
            sync_stmt();
        }
    }
    no_struct_lit_ = saved;
    Span end = expect(Tok::RBrace, "to close the block").span;
    b->span = Span::merge(start, end);
    return b;
}

StmtPtr Parser::parse_stmt() {
    DepthGuard g(*this);
    Span start = peek().span;
    switch (peek().kind) {
        case Tok::KwLet:
        case Tok::KwVar: return parse_let();
        case Tok::KwFn: {
            if (peek(1).kind == Tok::Ident) {
                auto fn = parse_fn(false);
                Span s = fn->span;
                return std::make_unique<FnStmt>(s, std::move(fn));
            }
            break;  // a lambda in expression-statement position
        }
        case Tok::KwIf: return parse_if();
        case Tok::KwWhile: {
            advance();
            auto w = std::make_unique<WhileStmt>(start);
            bool saved = no_struct_lit_;
            no_struct_lit_ = true;
            w->cond = parse_expr();
            no_struct_lit_ = saved;
            w->body = parse_block();
            w->span = Span::merge(start, w->body->span);
            return w;
        }
        case Tok::KwFor: return parse_for();
        case Tok::KwBreak: {
            advance();
            Span end = expect(Tok::Semicolon, "after 'break'").span;
            return std::make_unique<BreakStmt>(Span::merge(start, end));
        }
        case Tok::KwContinue: {
            advance();
            Span end = expect(Tok::Semicolon, "after 'continue'").span;
            return std::make_unique<ContinueStmt>(Span::merge(start, end));
        }
        case Tok::KwReturn: {
            advance();
            auto r = std::make_unique<ReturnStmt>(start);
            if (!check(Tok::Semicolon)) r->value = parse_expr();
            Span end = expect(Tok::Semicolon, "after return").span;
            r->span = Span::merge(start, end);
            return r;
        }
        case Tok::LBrace: {
            auto b = parse_block();
            Span s = b->span;
            return std::make_unique<BlockStmt>(s, std::move(b));
        }
        case Tok::KwStruct: case Tok::KwEnum: case Tok::KwConst: case Tok::KwImport:
            fail(peek(), std::string(tok_desc(peek().kind)) + " declarations are only allowed at the top level");
        default: break;
    }

    ExprPtr e = parse_expr();
    BinaryOp op = BinaryOp::Add;
    if (check(Tok::Eq) || is_compound_assign(peek().kind, op)) {
        const Token& eq = advance();
        if (!is_place(*e)) {
            diags_.error(DiagKind::Syntax, e->span, "invalid assignment target",
                         {"only variables, array elements (a[i]) and fields (s.f) can be assigned"});
            throw SyntaxError{};
        }
        auto a = std::make_unique<AssignStmt>(start);
        a->compound = eq.kind != Tok::Eq;
        a->op = op;
        a->target = std::move(e);
        a->value = parse_expr();
        Span end = expect(Tok::Semicolon, "after assignment").span;
        a->span = Span::merge(start, end);
        return a;
    }
    // A `match` used as a statement may omit the semicolon (like `if`).
    Span end = e->span;
    if (e->kind == ExprKind::Match && !check(Tok::Semicolon)) {
        // nothing to consume
    } else {
        end = expect(Tok::Semicolon, "after expression").span;
    }
    if (e->kind != ExprKind::Call && e->kind != ExprKind::Match) {
        diags_.error(DiagKind::Syntax, e->span, "expression statement has no effect",
                     {"only calls and match expressions can be used as statements"});
    }
    Span s = Span::merge(start, end);
    return std::make_unique<ExprStmt>(s, std::move(e));
}

StmtPtr Parser::parse_if() {
    DepthGuard g(*this);
    Span start = advance().span;
    auto s = std::make_unique<IfStmt>(start);
    bool saved = no_struct_lit_;
    no_struct_lit_ = true;
    s->cond = parse_expr();
    no_struct_lit_ = saved;
    s->then_block = parse_block();
    s->span = Span::merge(start, s->then_block->span);
    if (match(Tok::KwElse)) {
        if (check(Tok::KwIf)) {
            s->else_stmt = parse_if();
        } else {
            auto b = parse_block();
            Span bs = b->span;
            s->else_stmt = std::make_unique<BlockStmt>(bs, std::move(b));
        }
        s->span = Span::merge(s->span, s->else_stmt->span);
    }
    return s;
}

StmtPtr Parser::parse_for() {
    Span start = advance().span;
    const Token& var = expect(Tok::Ident, "for loop variable");
    std::string name = var.text;
    Span var_span = var.span;
    expect(Tok::KwIn, "after loop variable");
    bool saved = no_struct_lit_;
    no_struct_lit_ = true;
    ExprPtr first = parse_expr();
    if (match(Tok::DotDot)) {
        auto f = std::make_unique<ForRangeStmt>(start);
        f->var = name;
        f->var_span = var_span;
        f->start = std::move(first);
        f->end = parse_expr();
        no_struct_lit_ = saved;
        f->body = parse_block();
        f->span = Span::merge(start, f->body->span);
        return f;
    }
    no_struct_lit_ = saved;
    auto f = std::make_unique<ForEachStmt>(start);
    f->var = name;
    f->var_span = var_span;
    f->iterable = std::move(first);
    f->body = parse_block();
    f->span = Span::merge(start, f->body->span);
    return f;
}

// ------------------------------------------------------------ expressions

ExprPtr Parser::parse_expression_only() {
    try {
        ExprPtr e = parse_expr();
        if (!check(Tok::Eof)) fail(peek(), std::string("unexpected ") + tok_desc(peek().kind) + " after expression");
        return e;
    } catch (const SyntaxError&) {
        return nullptr;
    }
}

ExprPtr Parser::parse_expr(int min_prec) {
    DepthGuard g(*this);
    ExprPtr lhs = parse_unary();
    for (;;) {
        Tok t = peek().kind;
        int prec = infix_prec(t);
        if (prec == kNone || prec <= min_prec) break;
        const Token& op = advance();
        Span op_span = op.span;
        ExprPtr rhs = parse_expr(prec);  // left-associative: rhs binds tighter
        Span s = Span::merge(lhs->span, rhs->span);
        if (t == Tok::AndAnd || t == Tok::OrOr) {
            lhs = std::make_unique<LogicalExpr>(s, t == Tok::AndAnd ? LogicalOp::And : LogicalOp::Or, std::move(lhs),
                                                std::move(rhs));
        } else {
            auto b = std::make_unique<BinaryExpr>(s, to_binop(t), std::move(lhs), std::move(rhs));
            b->op_span = op_span;
            lhs = std::move(b);
        }
    }
    return lhs;
}

ExprPtr Parser::parse_unary() {
    DepthGuard g(*this);
    if (check(Tok::Minus) || check(Tok::Bang)) {
        const Token& op = advance();
        Span s = op.span;
        UnaryOp uop = op.kind == Tok::Minus ? UnaryOp::Neg : UnaryOp::Not;
        ExprPtr operand = parse_unary();
        Span full = Span::merge(s, operand->span);
        return std::make_unique<UnaryExpr>(full, uop, std::move(operand));
    }
    return parse_postfix(parse_primary());
}

std::vector<ExprPtr> Parser::parse_args() {
    std::vector<ExprPtr> args;
    bool saved = no_struct_lit_;
    no_struct_lit_ = false;
    while (!check(Tok::RParen)) {
        args.push_back(parse_expr());
        if (!match(Tok::Comma)) break;
    }
    no_struct_lit_ = saved;
    return args;
}

ExprPtr Parser::parse_postfix(ExprPtr e) {
    for (;;) {
        DepthGuard g(*this);
        if (match(Tok::LParen)) {
            auto call = std::make_unique<CallExpr>(e->span, std::move(e));
            call->args = parse_args();
            call->span = Span::merge(call->span, expect(Tok::RParen, "to close the argument list").span);
            e = std::move(call);
        } else if (match(Tok::LBracket)) {
            bool saved = no_struct_lit_;
            no_struct_lit_ = false;
            ExprPtr idx = parse_expr();
            no_struct_lit_ = saved;
            Span end = expect(Tok::RBracket, "to close the index").span;
            Span s = Span::merge(e->span, end);
            e = std::make_unique<IndexExpr>(s, std::move(e), std::move(idx));
        } else if (match(Tok::Dot)) {
            const Token& f = expect(Tok::Ident, "for field name after '.'");
            Span s = Span::merge(e->span, f.span);
            auto fe = std::make_unique<FieldExpr>(s, std::move(e), f.text);
            fe->field_span = f.span;
            e = std::move(fe);
        } else {
            return e;
        }
    }
}

ExprPtr Parser::parse_primary() {
    DepthGuard g(*this);
    const Token& t = peek();
    Span s = t.span;
    switch (t.kind) {
        case Tok::Int: advance(); return std::make_unique<IntLit>(s, t.int_value);
        case Tok::Float: advance(); return std::make_unique<FloatLit>(s, t.float_value);
        case Tok::String: {
            std::string v = t.text;
            advance();
            return std::make_unique<StringLit>(s, std::move(v));
        }
        case Tok::KwTrue: advance(); return std::make_unique<BoolLit>(s, true);
        case Tok::KwFalse: advance(); return std::make_unique<BoolLit>(s, false);
        case Tok::LParen: {
            advance();
            if (check(Tok::RParen)) {
                Span end = advance().span;
                return std::make_unique<UnitLit>(Span::merge(s, end));
            }
            bool saved = no_struct_lit_;
            no_struct_lit_ = false;
            ExprPtr inner = parse_expr();
            no_struct_lit_ = saved;
            Span end = expect(Tok::RParen, "to close the parenthesized expression").span;
            inner->span = Span::merge(s, end);
            return inner;
        }
        case Tok::LBracket: {
            advance();
            bool saved = no_struct_lit_;
            no_struct_lit_ = false;
            if (match(Tok::RBracket)) {
                no_struct_lit_ = saved;
                return std::make_unique<ArrayLit>(Span::merge(s, toks_[pos_ - 1].span));
            }
            ExprPtr first = parse_expr();
            if (match(Tok::Semicolon)) {
                ExprPtr count = parse_expr();
                no_struct_lit_ = saved;
                Span end = expect(Tok::RBracket, "to close the array").span;
                return std::make_unique<ArrayRepeat>(Span::merge(s, end), std::move(first), std::move(count));
            }
            auto arr = std::make_unique<ArrayLit>(s);
            arr->elems.push_back(std::move(first));
            while (match(Tok::Comma)) {
                if (check(Tok::RBracket)) break;
                arr->elems.push_back(parse_expr());
            }
            no_struct_lit_ = saved;
            arr->span = Span::merge(s, expect(Tok::RBracket, "to close the array").span);
            return arr;
        }
        case Tok::KwFn: {
            auto lam = std::make_unique<LambdaExpr>(s);
            lam->fn = parse_fn(true);
            lam->span = lam->fn->span;
            return lam;
        }
        case Tok::KwMatch: return parse_match();
        case Tok::Ident: {
            std::string name = t.text;
            advance();
            if (match(Tok::ColonColon)) {
                const Token& v = expect(Tok::Ident, "for enum variant name after '::'");
                auto ctor = std::make_unique<EnumCtor>(Span::merge(s, v.span), name, v.text);
                if (match(Tok::LParen)) {
                    ctor->has_parens = true;
                    ctor->args = parse_args();
                    ctor->span = Span::merge(ctor->span, expect(Tok::RParen, "to close the variant arguments").span);
                }
                return ctor;
            }
            // Struct literal: `Name {` followed by `}` or `field :`.
            if (!no_struct_lit_ && check(Tok::LBrace) &&
                (peek(1).kind == Tok::RBrace || (peek(1).kind == Tok::Ident && peek(2).kind == Tok::Colon))) {
                advance();
                auto lit = std::make_unique<StructLit>(s, name);
                lit->name_span = s;
                while (!check(Tok::RBrace)) {
                    FieldInit fi;
                    const Token& fname = expect(Tok::Ident, "for field name in struct literal");
                    fi.name = fname.text;
                    fi.span = fname.span;
                    expect(Tok::Colon, "after field name in struct literal");
                    fi.value = parse_expr();
                    lit->fields.push_back(std::move(fi));
                    if (!match(Tok::Comma)) break;
                }
                lit->span = Span::merge(s, expect(Tok::RBrace, "to close the struct literal").span);
                return lit;
            }
            return std::make_unique<IdentExpr>(s, name);
        }
        default:
            fail(t, std::string("expected an expression, found ") + tok_desc(t.kind));
    }
}

ExprPtr Parser::parse_match() {
    Span start = advance().span;
    bool saved = no_struct_lit_;
    no_struct_lit_ = true;
    ExprPtr scrut = parse_expr();
    no_struct_lit_ = false;
    auto m = std::make_unique<MatchExpr>(start, std::move(scrut));
    expect(Tok::LBrace, "to start the match arms");
    while (!check(Tok::RBrace)) {
        MatchArm arm;
        arm.pattern = parse_pattern();
        expect(Tok::FatArrow, "after match pattern");
        if (check(Tok::LBrace)) {
            arm.block = parse_block();
            arm.span = Span::merge(arm.pattern.span, arm.block->span);
            m->arms.push_back(std::move(arm));
            match(Tok::Comma);  // optional after a block arm
            continue;
        }
        arm.expr = parse_expr();
        arm.span = Span::merge(arm.pattern.span, arm.expr->span);
        m->arms.push_back(std::move(arm));
        if (!match(Tok::Comma)) break;
    }
    no_struct_lit_ = saved;
    m->span = Span::merge(start, expect(Tok::RBrace, "to close the match").span);
    if (m->arms.empty()) diags_.error(DiagKind::Syntax, m->span, "match must have at least one arm");
    return m;
}

Pattern Parser::parse_pattern() {
    Pattern p;
    const Token& t = peek();
    p.span = t.span;
    switch (t.kind) {
        case Tok::Underscore: advance(); p.kind = PatternKind::Wildcard; return p;
        case Tok::Int: advance(); p.kind = PatternKind::Int; p.int_value = t.int_value; return p;
        case Tok::Minus: {
            advance();
            const Token& n = expect(Tok::Int, "after '-' in pattern");
            p.kind = PatternKind::Int;
            p.int_value = -n.int_value;  // n.int_value >= 0, so no overflow
            p.span = Span::merge(p.span, n.span);
            return p;
        }
        case Tok::String: advance(); p.kind = PatternKind::String; p.string_value = t.text; return p;
        case Tok::KwTrue: advance(); p.kind = PatternKind::Bool; p.bool_value = true; return p;
        case Tok::KwFalse: advance(); p.kind = PatternKind::Bool; p.bool_value = false; return p;
        case Tok::Ident: {
            p.kind = PatternKind::Variant;
            p.enum_name = advance().text;
            expect(Tok::ColonColon, "in variant pattern (write Enum::Variant)");
            const Token& v = expect(Tok::Ident, "for variant name in pattern");
            p.variant = v.text;
            p.span = Span::merge(p.span, v.span);
            if (match(Tok::LParen)) {
                while (!check(Tok::RParen)) {
                    if (check(Tok::Underscore)) {
                        p.binding_spans.push_back(advance().span);
                        p.bindings.push_back("_");
                    } else {
                        const Token& b = expect(Tok::Ident, "for pattern binding");
                        p.binding_spans.push_back(b.span);
                        p.bindings.push_back(b.text);
                    }
                    if (!match(Tok::Comma)) break;
                }
                p.span = Span::merge(p.span, expect(Tok::RParen, "to close the pattern").span);
            }
            return p;
        }
        default:
            fail(t, std::string("expected a pattern, found ") + tok_desc(t.kind));
    }
}

}  // namespace ember

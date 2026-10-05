// Abstract syntax tree. Nodes own their children through unique_ptr. The
// parser fills in syntax only; name resolution and type checking fill in the
// annotation fields (sym, type, ...), which later stages read.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/source.h"

namespace ember {

struct Type;
struct Symbol;
struct FnInfo;
struct StructDecl;
struct EnumDecl;

// ------------------------------------------------------------ type syntax

enum class TypeExprKind { Named, Array, Func };

struct TypeExpr {
    TypeExprKind kind;
    Span span;
    std::string name;                               // Named
    std::unique_ptr<TypeExpr> elem;                 // Array
    std::vector<std::unique_ptr<TypeExpr>> params;  // Func
    std::unique_ptr<TypeExpr> ret;                  // Func (null = Unit)
};

// ------------------------------------------------------------ expressions

enum class ExprKind {
    IntLit, FloatLit, BoolLit, StringLit, UnitLit,
    Ident, Unary, Binary, Logical, Call, Index, Field,
    ArrayLit, ArrayRepeat, StructLit, EnumCtor, Lambda, Match, ModuleCall
};

enum class UnaryOp { Neg, Not };
enum class BinaryOp { Add, Sub, Mul, Div, Mod, Eq, Ne, Lt, Le, Gt, Ge };
enum class LogicalOp { And, Or };

const char* binary_op_str(BinaryOp op);

struct Block;
struct Pattern;

struct Expr {
    explicit Expr(ExprKind k, Span s) : kind(k), span(s) {}
    virtual ~Expr() = default;
    ExprKind kind;
    Span span;
    Type* type = nullptr;  // set by the type checker
};

using ExprPtr = std::unique_ptr<Expr>;

struct IntLit : Expr { IntLit(Span s, int64_t v) : Expr(ExprKind::IntLit, s), value(v) {} int64_t value; };
struct FloatLit : Expr { FloatLit(Span s, double v) : Expr(ExprKind::FloatLit, s), value(v) {} double value; };
struct BoolLit : Expr { BoolLit(Span s, bool v) : Expr(ExprKind::BoolLit, s), value(v) {} bool value; };
struct StringLit : Expr { StringLit(Span s, std::string v) : Expr(ExprKind::StringLit, s), value(std::move(v)) {} std::string value; };
struct UnitLit : Expr { explicit UnitLit(Span s) : Expr(ExprKind::UnitLit, s) {} };

struct IdentExpr : Expr {
    IdentExpr(Span s, std::string n) : Expr(ExprKind::Ident, s), name(std::move(n)) {}
    std::string name;
    Symbol* sym = nullptr;  // set by the resolver
};

struct UnaryExpr : Expr {
    UnaryExpr(Span s, UnaryOp o, ExprPtr e) : Expr(ExprKind::Unary, s), op(o), operand(std::move(e)) {}
    UnaryOp op;
    ExprPtr operand;
};

struct BinaryExpr : Expr {
    BinaryExpr(Span s, BinaryOp o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::Binary, s), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
    BinaryOp op;
    Span op_span;
    ExprPtr lhs, rhs;
};

struct LogicalExpr : Expr {
    LogicalExpr(Span s, LogicalOp o, ExprPtr l, ExprPtr r)
        : Expr(ExprKind::Logical, s), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
    LogicalOp op;
    ExprPtr lhs, rhs;
};

// Calls of builtin functions (print, len, ...) are recognised during name
// resolution: callee is an IdentExpr whose symbol has kind Builtin.
struct CallExpr : Expr {
    CallExpr(Span s, ExprPtr c) : Expr(ExprKind::Call, s), callee(std::move(c)) {}
    ExprPtr callee;
    std::vector<ExprPtr> args;
};

// `math.sqrt(x)`: a call into an imported std module.
struct ModuleCallExpr : Expr {
    ModuleCallExpr(Span s) : Expr(ExprKind::ModuleCall, s) {}
    std::string module, function;
    Span function_span;
    std::vector<ExprPtr> args;
    int native_id = -1;  // set by the resolver
};

struct IndexExpr : Expr {
    IndexExpr(Span s, ExprPtr o, ExprPtr i) : Expr(ExprKind::Index, s), object(std::move(o)), index(std::move(i)) {}
    ExprPtr object, index;
};

struct FieldExpr : Expr {
    FieldExpr(Span s, ExprPtr o, std::string f) : Expr(ExprKind::Field, s), object(std::move(o)), field(std::move(f)) {}
    ExprPtr object;
    std::string field;
    Span field_span;
    int field_index = -1;  // set by the type checker
};

struct ArrayLit : Expr {
    explicit ArrayLit(Span s) : Expr(ExprKind::ArrayLit, s) {}
    std::vector<ExprPtr> elems;
};

struct ArrayRepeat : Expr {
    ArrayRepeat(Span s, ExprPtr v, ExprPtr c) : Expr(ExprKind::ArrayRepeat, s), value(std::move(v)), count(std::move(c)) {}
    ExprPtr value, count;
};

struct FieldInit {
    std::string name;
    Span span;
    ExprPtr value;
};

struct StructLit : Expr {
    StructLit(Span s, std::string n) : Expr(ExprKind::StructLit, s), name(std::move(n)) {}
    std::string name;
    Span name_span;
    std::vector<FieldInit> fields;  // source order
    StructDecl* decl = nullptr;      // set by the resolver
};

struct EnumCtor : Expr {
    EnumCtor(Span s, std::string e, std::string v) : Expr(ExprKind::EnumCtor, s), enum_name(std::move(e)), variant(std::move(v)) {}
    std::string enum_name, variant;
    std::vector<ExprPtr> args;
    bool has_parens = false;
    EnumDecl* decl = nullptr;  // set by the resolver
    int variant_index = -1;    // set by the type checker
};

struct Param {
    std::string name;
    Span span;
    std::unique_ptr<TypeExpr> type;
    Symbol* sym = nullptr;
};

struct FnDecl;

struct LambdaExpr : Expr {
    LambdaExpr(Span s) : Expr(ExprKind::Lambda, s) {}
    std::unique_ptr<FnDecl> fn;
};

enum class PatternKind { Wildcard, Int, String, Bool, Variant };

struct Pattern {
    PatternKind kind = PatternKind::Wildcard;
    Span span;
    int64_t int_value = 0;
    std::string string_value;
    bool bool_value = false;
    std::string enum_name, variant;
    std::vector<std::string> bindings;  // "_" for ignored positions
    std::vector<Span> binding_spans;
    std::vector<Symbol*> binding_syms;  // set by the resolver (null for "_")
    EnumDecl* decl = nullptr;
    int variant_index = -1;
};

struct MatchArm {
    Pattern pattern;
    ExprPtr expr;                  // either expr ...
    std::unique_ptr<Block> block;  // ... or block
    Span span;
};

struct MatchExpr : Expr {
    MatchExpr(Span s, ExprPtr e) : Expr(ExprKind::Match, s), scrutinee(std::move(e)) {}
    ExprPtr scrutinee;
    std::vector<MatchArm> arms;
};

// ------------------------------------------------------------ statements

enum class StmtKind { Let, Fn, If, While, ForRange, ForEach, Break, Continue, Return, Assign, ExprStmt, BlockStmt };

struct Stmt {
    Stmt(StmtKind k, Span s) : kind(k), span(s) {}
    virtual ~Stmt() = default;
    StmtKind kind;
    Span span;
};

using StmtPtr = std::unique_ptr<Stmt>;

struct Block {
    Span span;
    std::vector<StmtPtr> stmts;
};

struct LetStmt : Stmt {
    LetStmt(Span s) : Stmt(StmtKind::Let, s) {}
    bool mutable_ = false;
    std::string name;
    Span name_span;
    std::unique_ptr<TypeExpr> type;  // optional
    ExprPtr init;
    Symbol* sym = nullptr;
};

struct FnDecl {
    std::string name;  // empty for lambdas
    Span span, name_span;
    std::vector<Param> params;
    std::unique_ptr<TypeExpr> ret;  // null = Unit
    std::unique_ptr<Block> body;
    Symbol* sym = nullptr;    // the binding of the name (null for lambdas)
    FnInfo* info = nullptr;   // set by the resolver
    Type* fn_type = nullptr;  // set by the type checker
};

struct FnStmt : Stmt {
    FnStmt(Span s, std::unique_ptr<FnDecl> f) : Stmt(StmtKind::Fn, s), fn(std::move(f)) {}
    std::unique_ptr<FnDecl> fn;
};

struct IfStmt : Stmt {
    IfStmt(Span s) : Stmt(StmtKind::If, s) {}
    ExprPtr cond;
    std::unique_ptr<Block> then_block;
    StmtPtr else_stmt;  // IfStmt or BlockStmt, or null
};

struct WhileStmt : Stmt {
    WhileStmt(Span s) : Stmt(StmtKind::While, s) {}
    ExprPtr cond;
    std::unique_ptr<Block> body;
};

struct ForRangeStmt : Stmt {
    ForRangeStmt(Span s) : Stmt(StmtKind::ForRange, s) {}
    std::string var;
    Span var_span;
    ExprPtr start, end;
    std::unique_ptr<Block> body;
    Symbol* sym = nullptr;
};

struct ForEachStmt : Stmt {
    ForEachStmt(Span s) : Stmt(StmtKind::ForEach, s) {}
    std::string var;
    Span var_span;
    ExprPtr iterable;
    std::unique_ptr<Block> body;
    Symbol* sym = nullptr;
};

struct BreakStmt : Stmt { explicit BreakStmt(Span s) : Stmt(StmtKind::Break, s) {} };
struct ContinueStmt : Stmt { explicit ContinueStmt(Span s) : Stmt(StmtKind::Continue, s) {} };

struct ReturnStmt : Stmt {
    ReturnStmt(Span s) : Stmt(StmtKind::Return, s) {}
    ExprPtr value;  // optional
};

// `target = value` or `target OP= value`; op is set for compound forms.
struct AssignStmt : Stmt {
    AssignStmt(Span s) : Stmt(StmtKind::Assign, s) {}
    ExprPtr target, value;
    bool compound = false;
    BinaryOp op = BinaryOp::Add;
};

struct ExprStmt : Stmt {
    ExprStmt(Span s, ExprPtr e) : Stmt(StmtKind::ExprStmt, s), expr(std::move(e)) {}
    ExprPtr expr;
};

struct BlockStmt : Stmt {
    BlockStmt(Span s, std::unique_ptr<Block> b) : Stmt(StmtKind::BlockStmt, s), block(std::move(b)) {}
    std::unique_ptr<Block> block;
};

// ------------------------------------------------------------ declarations

struct FieldDecl {
    std::string name;
    Span span;
    std::unique_ptr<TypeExpr> type;
    Type* resolved = nullptr;
};

struct StructDecl {
    std::string name;
    Span span, name_span;
    std::vector<FieldDecl> fields;
    int index = -1;  // position in the module's struct table
    Type* type = nullptr;
};

struct VariantDecl {
    std::string name;
    Span span;
    std::vector<std::unique_ptr<TypeExpr>> payload;
    std::vector<Type*> resolved;
};

struct EnumDecl {
    std::string name;
    Span span, name_span;
    std::vector<VariantDecl> variants;
    int index = -1;
    Type* type = nullptr;
};

struct ConstDecl {
    std::string name;
    Span span, name_span;
    std::unique_ptr<TypeExpr> type;
    ExprPtr value;
    Symbol* sym = nullptr;
};

struct ImportDecl {
    std::string module;
    Span span;
};

// Top-level items, kept in source order (global initialization order).
enum class ItemKind { Fn, Struct, Enum, Const, Global, Import };

struct Item {
    ItemKind kind;
    std::unique_ptr<FnDecl> fn;
    std::unique_ptr<StructDecl> struct_decl;
    std::unique_ptr<EnumDecl> enum_decl;
    std::unique_ptr<ConstDecl> const_decl;
    std::unique_ptr<LetStmt> global;
    std::unique_ptr<ImportDecl> import;
};

struct Program {
    std::vector<Item> items;
};

}  // namespace ember

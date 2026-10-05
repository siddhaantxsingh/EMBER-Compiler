// Semantic types and symbols. Types are interned by TypeContext, so two
// structurally equal types are the same pointer and equality is pointer
// comparison.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "front/ast.h"

namespace ember {

enum class TypeKind { Error, Int, Float, Bool, String, Unit, Array, Func, Struct, Enum };

struct Type {
    explicit Type(TypeKind k) : kind(k) {}
    TypeKind kind;
    Type* elem = nullptr;           // Array
    std::vector<Type*> params;      // Func
    Type* ret = nullptr;            // Func
    StructDecl* struct_decl = nullptr;
    EnumDecl* enum_decl = nullptr;

    bool is_error() const { return kind == TypeKind::Error; }
    bool is_ref() const {
        return kind == TypeKind::String || kind == TypeKind::Array || kind == TypeKind::Func ||
               kind == TypeKind::Struct || kind == TypeKind::Enum;
    }
};

std::string type_name(const Type* t);

class TypeContext {
public:
    TypeContext();
    Type* error() { return &error_; }
    Type* int_() { return &int_t_; }
    Type* float_() { return &float_t_; }
    Type* bool_() { return &bool_t_; }
    Type* string() { return &string_t_; }
    Type* unit() { return &unit_t_; }
    Type* array(Type* elem);
    Type* func(const std::vector<Type*>& params, Type* ret);
    Type* struct_type(StructDecl* d);
    Type* enum_type(EnumDecl* d);

private:
    Type error_{TypeKind::Error}, int_t_{TypeKind::Int}, float_t_{TypeKind::Float}, bool_t_{TypeKind::Bool},
        string_t_{TypeKind::String}, unit_t_{TypeKind::Unit};
    std::map<Type*, std::unique_ptr<Type>> arrays_;
    std::map<std::pair<std::vector<Type*>, Type*>, std::unique_ptr<Type>> funcs_;
    std::map<void*, std::unique_ptr<Type>> nominal_;
};

// ------------------------------------------------------------ symbols

enum class SymbolKind { Local, Global, Function, Const, Builtin };

enum class Builtin {
    Print, Input, Len, Push, Pop, Str, Int, Float, ParseInt, Panic, Assert
};

struct Symbol {
    SymbolKind kind = SymbolKind::Local;
    std::string name;
    Span span;
    Type* type = nullptr;
    bool mutable_ = false;
    bool captured = false;     // a closure refers to it (locals only)
    FnInfo* owner = nullptr;   // function that declares it (locals)
    FnDecl* fn = nullptr;      // Function symbols (top-level or nested)
    Builtin builtin = Builtin::Print;
    ConstDecl* const_decl = nullptr;
    int global_index = -1;     // Global symbols: slot in the globals table
    // Const symbols: folded value (Int/Float/Bool/String only)
    int64_t const_int = 0;
    double const_float = 0;
    bool const_bool = false;
    std::string const_string;
    bool const_ready = false;
};

// One per function (top-level, nested, or lambda). The resolver records the
// variables it captures, in first-use order, so lowering can build the
// upvalue table.
struct Capture {
    Symbol* sym;
    // Where the closure finds it at creation time: a local of the directly
    // enclosing function (is_local) or an upvalue of that function (index).
    bool from_parent_local;
    int parent_upvalue_index;  // when !from_parent_local
};

struct FnInfo {
    FnDecl* decl = nullptr;
    FnInfo* parent = nullptr;  // enclosing function (null for top-level)
    std::string display_name;  // "main", "counter.inc", "<lambda@12:9>"
    std::vector<Symbol*> locals;  // params first, then other locals in declaration order
    std::vector<Capture> captures;
    int index = -1;            // function index in the module
    bool is_top_level() const { return parent == nullptr; }
    int capture_index(Symbol* s) const {
        for (size_t i = 0; i < captures.size(); ++i)
            if (captures[i].sym == s) return static_cast<int>(i);
        return -1;
    }
};

// Everything semantic analysis produces, owned in one place so that AST
// annotations (raw pointers) stay valid for the later stages.
struct SemaResult {
    TypeContext types;
    std::vector<std::unique_ptr<Symbol>> symbols;
    std::vector<std::unique_ptr<FnInfo>> functions;  // index == FnInfo::index
    std::vector<Symbol*> globals;                    // index == global_index
    std::vector<StructDecl*> structs;
    std::vector<EnumDecl*> enums;
    std::vector<std::string> imports;
    FnInfo* main = nullptr;
    // Statement/expression annotations not stored in nodes:
    std::map<const Stmt*, bool> unused;  // reserved
};

}  // namespace ember

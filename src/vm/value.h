// Runtime values and heap objects.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/kind.h"

namespace ember::vm {

// The first five tags coincide with Kind so verified code can rely on the
// static kind; Uninit marks a global or captured variable that has not been
// assigned yet (reading it through LOAD_GLOBAL/LOAD_UPVAL traps).
enum class Tag : uint8_t { Unit = 0, Bool = 1, Int = 2, Float = 3, Ref = 4, Uninit = 5 };

struct Obj;

// 16 bytes, trivially copyable and trivially default-constructible (so the
// preallocated value stack is not touched until used).
struct Value {
    Tag tag;
    union {
        int64_t i;
        double f;
        bool b;
        Obj* o;
    };

    static Value unit() { Value v; v.tag = Tag::Unit; v.i = 0; return v; }
    static Value uninit() { Value v; v.tag = Tag::Uninit; v.i = 0; return v; }
    static Value boolean(bool x) { Value v; v.tag = Tag::Bool; v.i = 0; v.b = x; return v; }
    static Value integer(int64_t x) { Value v; v.tag = Tag::Int; v.i = x; return v; }
    static Value flt(double x) { Value v; v.tag = Tag::Float; v.f = x; return v; }
    static Value ref(Obj* x) { Value v; v.tag = Tag::Ref; v.o = x; return v; }
    // Initial value of a local slot of the given kind.
    static Value default_for(Kind k) {
        switch (k) {
            case Kind::Bool: return boolean(false);
            case Kind::Int: return integer(0);
            case Kind::Float: return flt(0.0);
            case Kind::Ref: return uninit();
            default: return unit();
        }
    }
};

static_assert(sizeof(Value) == 16);

enum class ObjType : uint8_t { String, Array, Struct, Enum, Closure, Upvalue };

struct Obj {
    ObjType type;
    bool marked = false;
    size_t bytes = 0;     // accounted size (header + payload)
    Obj* next = nullptr;  // all-objects list
    explicit Obj(ObjType t) : type(t) {}
};

struct StringObj : Obj {
    std::string s;
    StringObj() : Obj(ObjType::String) {}
};

struct ArrayObj : Obj {
    Kind elem;
    std::vector<Value> items;
    explicit ArrayObj(Kind k) : Obj(ObjType::Array), elem(k) {}
};

struct StructObj : Obj {
    uint16_t sid;
    std::vector<Value> fields;
    explicit StructObj(uint16_t id) : Obj(ObjType::Struct), sid(id) {}
};

struct EnumObj : Obj {
    uint16_t eid;
    uint16_t variant;
    std::vector<Value> fields;
    EnumObj(uint16_t e, uint16_t v) : Obj(ObjType::Enum), eid(e), variant(v) {}
};

struct UpvalueObj : Obj {
    Value* loc;           // points into the stack while open, at `closed` after
    Value closed;
    UpvalueObj* next_open = nullptr;
    explicit UpvalueObj(Value* l) : Obj(ObjType::Upvalue), loc(l) { closed = Value::unit(); }
};

struct ClosureObj : Obj {
    uint32_t fn;
    std::vector<UpvalueObj*> upvalues;
    explicit ClosureObj(uint32_t f) : Obj(ObjType::Closure), fn(f) {}
};

}  // namespace ember::vm

// Ember IR: a control-flow graph of basic blocks per function.
//
// * Temporaries ("temps") are typed by Kind and defined exactly once
//   (SSA-like); a temp's definition dominates all its uses.
// * Locals are mutable frame slots (parameters first). Reading a variable is
//   an explicit LoadLocal into a temp, so evaluation order is explicit.
// * Every block ends with exactly one terminator (Jump, Branch, Return,
//   Unreachable); no instruction follows it.
//
// Lowering (AST -> IR) is in lower.cc, structural validation in validate.cc,
// optimizations in opt.cc, printing in print.cc. Code generation consumes the
// validated IR and never looks at the AST.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/kind.h"
#include "common/source.h"

namespace ember::ir {

#define EMBER_IR_OPS(X)                                                                  \
    /* constants */                                                                      \
    X(ConstInt) X(ConstFloat) X(ConstBool) X(ConstString) X(ConstUnit)                   \
    /* variables */                                                                      \
    X(LoadLocal) X(StoreLocal) X(LoadUpval) X(StoreUpval) X(LoadGlobal) X(StoreGlobal)   \
    X(ClearLocal) /* reset a slot to 'uninitialized' (captured before its let ran) */ \
    /* arithmetic (Int traps on overflow / division by zero) */                         \
    X(IAdd) X(ISub) X(IMul) X(IDiv) X(IMod) X(INeg)                                      \
    X(FAdd) X(FSub) X(FMul) X(FDiv) X(FNeg)                                              \
    X(Not) X(I2F) X(F2I)                                                                 \
    /* comparisons; imm = CmpOp */                                                      \
    X(ICmp) X(FCmp) X(BCmp) X(SCmp)                                                      \
    /* strings */                                                                        \
    X(SConcat) X(SLen) X(SIndex)                                                         \
    /* calls; imm = function index / signature index / native index */                  \
    X(Call) X(CallIndirect) X(CallNative)                                                \
    /* closures; imm = function index */                                                \
    X(MakeClosure) X(CloseUpvalues)                                                      \
    /* structs; imm = struct index, imm2 = field index */                               \
    X(MakeStruct) X(GetField) X(SetField)                                                \
    /* arrays; elem = element kind */                                                   \
    X(MakeArray) X(ArrayRepeat) X(ArrayGet) X(ArraySet) X(ArrayLen) X(ArrayPush) X(ArrayPop) \
    /* enums; imm = enum index, imm2 = variant, imm3 = field */                         \
    X(MakeEnum) X(EnumTag) X(EnumField)                                                  \
    /* terminators */                                                                    \
    X(Jump) X(Branch) X(Return) X(Unreachable)

enum class Op : uint8_t {
#define X(n) n,
    EMBER_IR_OPS(X)
#undef X
};

const char* op_name(Op op);
bool is_terminator(Op op);
// Pure: no side effects and no traps, so it can be removed if unused.
bool is_pure(Op op);

enum class CmpOp : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };
const char* cmp_name(CmpOp c);

using TempId = int32_t;
using BlockId = int32_t;
constexpr TempId kNoTemp = -1;

struct Instr {
    Op op;
    TempId dst = kNoTemp;
    std::vector<TempId> args;
    int64_t imm = 0;   // integer constant, index, CmpOp, local slot ...
    int64_t imm2 = 0;
    int64_t imm3 = 0;
    double fimm = 0;   // ConstFloat
    Kind elem = Kind::Unit;  // array element kind
    BlockId target = -1;     // Jump / Branch(true)
    BlockId target2 = -1;    // Branch(false)
    Span loc;                // source location for debug info
};

struct Block {
    BlockId id = 0;
    std::vector<Instr> instrs;
    bool terminated() const { return !instrs.empty() && is_terminator(instrs.back().op); }
};

// How a closure obtains each captured variable when it is created.
struct CaptureDesc {
    bool from_local;   // true: local slot of the creating frame; false: its upvalue
    uint32_t index;
};

struct LocalInfo {
    std::string name;  // "" for hidden locals
    Kind kind;
    // Source range where the variable is in scope (for the debugger).
    uint32_t decl_line = 0;
};

struct Function {
    std::string name;
    uint32_t num_params = 0;
    std::vector<Kind> param_kinds;
    Kind ret_kind = Kind::Unit;
    std::vector<LocalInfo> locals;      // params first
    std::vector<Kind> upvalue_kinds;
    std::vector<std::string> upvalue_names;
    std::vector<CaptureDesc> captures;  // same length as upvalue_kinds
    std::vector<Kind> temp_kinds;       // index = TempId
    std::vector<Block> blocks;          // block 0 is the entry
    Span loc;

    TempId new_temp(Kind k) {
        temp_kinds.push_back(k);
        return static_cast<TempId>(temp_kinds.size() - 1);
    }
    BlockId new_block() {
        Block b;
        b.id = static_cast<BlockId>(blocks.size());
        blocks.push_back(std::move(b));
        return b.id;
    }
};

struct StructInfo {
    std::string name;
    std::vector<std::string> field_names;
    std::vector<Kind> field_kinds;
};

struct VariantInfo {
    std::string name;
    std::vector<Kind> field_kinds;
};

struct EnumInfo {
    std::string name;
    std::vector<VariantInfo> variants;
};

struct Signature {
    std::vector<Kind> params;
    Kind ret;
    bool operator==(const Signature& o) const { return params == o.params && ret == o.ret; }
};

struct GlobalInfo {
    std::string name;
    Kind kind;
};

struct Module {
    std::vector<Function> functions;
    std::vector<StructInfo> structs;
    std::vector<EnumInfo> enums;
    std::vector<Signature> signatures;
    std::vector<GlobalInfo> globals;
    std::vector<std::string> strings;  // string constant pool
    uint32_t init_function = 0;        // runs global initializers
    uint32_t main_function = 0;

    uint32_t intern_string(const std::string& s);
    uint32_t intern_signature(const Signature& s);
};

std::string print_module(const Module& m);
std::string print_function(const Module& m, const Function& f);

}  // namespace ember::ir

// Ember bytecode instruction set.
//
// Each instruction is a one-byte opcode followed by fixed-size little-endian
// operands described by a format string:
//   b = u8, h = u16, w = u32, q = i64, d = f64 (IEEE 754 bits)
// The table below is the single source of truth for the encoder, the
// decoder/verifier, the disassembler and the VM. Opcode numbers are part of
// the file format: never reorder, only append (and bump the minor version).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ember::bc {

#define EMBER_OPCODES(X)                          \
    X(NOP, "")                                    \
    X(POP, "")                                    \
    X(PUSH_INT, "q")                              \
    X(PUSH_FLOAT, "d")                            \
    X(PUSH_TRUE, "")                              \
    X(PUSH_FALSE, "")                             \
    X(PUSH_UNIT, "")                              \
    X(PUSH_STR, "w")       /* string pool index */ \
    X(LOAD_LOCAL, "h")                            \
    X(STORE_LOCAL, "h")                           \
    X(CLEAR_LOCAL, "h")                           \
    X(LOAD_UPVAL, "h")                            \
    X(STORE_UPVAL, "h")                           \
    X(LOAD_GLOBAL, "h")                           \
    X(STORE_GLOBAL, "h")                          \
    X(IADD, "")                                   \
    X(ISUB, "")                                   \
    X(IMUL, "")                                   \
    X(IDIV, "")                                   \
    X(IMOD, "")                                   \
    X(INEG, "")                                   \
    X(FADD, "")                                   \
    X(FSUB, "")                                   \
    X(FMUL, "")                                   \
    X(FDIV, "")                                   \
    X(FNEG, "")                                   \
    X(NOT, "")                                    \
    X(I2F, "")                                    \
    X(F2I, "")                                    \
    X(IEQ, "") X(INE, "") X(ILT, "") X(ILE, "") X(IGT, "") X(IGE, "") \
    X(FEQ, "") X(FNE, "") X(FLT, "") X(FLE, "") X(FGT, "") X(FGE, "") \
    X(BEQ, "") X(BNE, "")                         \
    X(SEQ, "") X(SNE, "") X(SLT, "") X(SLE, "") X(SGT, "") X(SGE, "") \
    X(SCONCAT, "")                                \
    X(SLEN, "")                                   \
    X(SINDEX, "")                                 \
    X(CALL, "w")           /* function index */   \
    X(CALL_INDIRECT, "h")  /* signature index */  \
    X(CALL_NATIVE, "h")    /* native id */        \
    X(RETURN, "")                                 \
    X(CLOSURE, "w")        /* function index */   \
    X(CLOSE_UPVALUES, "h") /* lowest slot */      \
    X(MAKE_STRUCT, "h")                           \
    X(GET_FIELD, "hh")                            \
    X(SET_FIELD, "hh")                            \
    X(MAKE_ARRAY, "bw")    /* elem kind, count */ \
    X(ARRAY_REPEAT, "b")                          \
    X(ARRAY_GET, "b")                             \
    X(ARRAY_SET, "b")                             \
    X(ARRAY_LEN, "")                              \
    X(ARRAY_PUSH, "b")                            \
    X(ARRAY_POP, "b")                             \
    X(MAKE_ENUM, "hh")     /* enum, variant */    \
    X(ENUM_TAG, "h")                              \
    X(ENUM_FIELD, "hhh")   /* enum, variant, field */ \
    X(JUMP, "w")           /* absolute pc */      \
    X(JUMP_IF_FALSE, "w")                         \
    X(JUMP_IF_TRUE, "w")                          \
    X(UNREACHABLE, "")                            \
    /* superinstructions (format 1.1), chosen from opcode-pair profiles */ \
    X(LOAD_LOCAL2, "hh")   /* push local a, then local b */ \
    X(STORE_LOAD, "hh")    /* pop into local a, then push local b */ \
    X(IJEQ, "w") X(IJNE, "w") X(IJLT, "w") X(IJLE, "w") X(IJGT, "w") X(IJGE, "w") /* pop 2 ints, jump if true */

enum class Opcode : uint8_t {
#define X(n, f) n,
    EMBER_OPCODES(X)
#undef X
        COUNT_
};

constexpr size_t kOpcodeCount = static_cast<size_t>(Opcode::COUNT_);

struct OpcodeInfo {
    const char* name;
    const char* format;
    uint8_t size;  // total encoded size including the opcode byte
};

const OpcodeInfo& opcode_info(Opcode op);
inline const OpcodeInfo* opcode_info_checked(uint8_t byte) {
    return byte < kOpcodeCount ? &opcode_info(static_cast<Opcode>(byte)) : nullptr;
}

}  // namespace ember::bc

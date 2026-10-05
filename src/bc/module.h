// In-memory form of a compiled Ember program. Produced by the code generator
// or by decoding a .embc file; consumed by the verifier, disassembler and VM.
// This header (and src/bc/) has no dependency on the compiler.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/kind.h"

namespace ember::bc {

struct StructDef {
    std::string name;
    std::vector<std::string> field_names;
    std::vector<Kind> field_kinds;
};

struct VariantDef {
    std::string name;
    std::vector<Kind> field_kinds;
};

struct EnumDef {
    std::string name;
    std::vector<VariantDef> variants;
};

struct Signature {
    std::vector<Kind> params;
    Kind ret = Kind::Unit;
    bool operator==(const Signature& o) const { return params == o.params && ret == o.ret; }
};

struct GlobalDef {
    std::string name;
    Kind kind = Kind::Unit;
};

struct UpvalueDef {
    Kind kind = Kind::Unit;
    bool from_local = true;  // capture a local slot of the creating frame, or its upvalue
    uint16_t index = 0;
    std::string name;        // debug info
};

struct LineEntry {
    uint32_t pc;
    uint32_t line;
    uint32_t column;
};

struct Function {
    std::string name;
    uint16_t num_params = 0;
    Kind ret_kind = Kind::Unit;
    std::vector<Kind> local_kinds;  // params first; size = number of frame slots
    std::vector<UpvalueDef> upvalues;
    uint32_t max_stack = 0;         // operand stack slots needed above the locals
    std::vector<uint8_t> code;
    // Debug info (optional in the file format).
    uint32_t decl_line = 0;
    std::vector<LineEntry> lines;   // sorted by pc; entry applies until the next
    std::vector<std::string> local_names;  // "" for compiler temporaries

    Signature signature() const {
        Signature s;
        s.params.assign(local_kinds.begin(), local_kinds.begin() + num_params);
        s.ret = ret_kind;
        return s;
    }
    // Source position of the instruction at pc (0 if unknown).
    LineEntry location(uint32_t pc) const {
        LineEntry best{0, 0, 0};
        for (const auto& e : lines) {
            if (e.pc > pc) break;
            best = e;
        }
        return best;
    }
};

struct Module {
    std::vector<std::string> strings;
    std::vector<StructDef> structs;
    std::vector<EnumDef> enums;
    std::vector<Signature> signatures;
    std::vector<GlobalDef> globals;
    std::vector<Function> functions;
    uint32_t init_function = 0;
    uint32_t main_function = 0;
    std::string source_name;  // debug info
    bool has_debug_info = true;
};

// Unaligned little-endian operand access (all supported hosts are
// little-endian; this is checked at build time in format.cc).
template <typename T>
inline T read_operand(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

}  // namespace ember::bc

#include "bc/verify.h"

#include <optional>

#include "bc/opcodes.h"
#include "common/natives.h"

namespace ember::bc {

namespace {

constexpr uint32_t kMaxDeclaredStack = 1u << 20;
constexpr size_t kMaxErrorsPerModule = 20;

struct Fail {
    std::string msg;
};

class FunctionVerifier {
public:
    FunctionVerifier(const Module& m, const Function& f) : m_(m), f_(f) {}

    void run() {
        if (f_.code.empty()) throw Fail{"empty code"};
        if (f_.max_stack > kMaxDeclaredStack) throw Fail{"declared max_stack is too large"};
        if (f_.local_kinds.size() < f_.num_params) throw Fail{"fewer locals than parameters"};
        // Pass 1: instruction boundaries.
        starts_.assign(f_.code.size(), false);
        for (size_t pc = 0; pc < f_.code.size();) {
            const OpcodeInfo* info = opcode_info_checked(f_.code[pc]);
            if (!info) throw Fail{"invalid opcode " + std::to_string(f_.code[pc]) + " at pc " + std::to_string(pc)};
            if (pc + info->size > f_.code.size()) throw Fail{"truncated instruction at pc " + std::to_string(pc)};
            starts_[pc] = true;
            pc += info->size;
        }
        // Pass 2: abstract interpretation.
        states_.assign(f_.code.size(), std::nullopt);
        merge(0, {});
        while (!work_.empty()) {
            uint32_t pc = work_.back();
            work_.pop_back();
            step(pc);
        }
    }

private:
    using Stack = std::vector<Kind>;

    [[noreturn]] void fail(uint32_t pc, const std::string& msg) {
        throw Fail{"pc " + std::to_string(pc) + " (" + opcode_info(static_cast<Opcode>(f_.code[pc])).name + "): " + msg};
    }

    void merge(uint32_t pc, const Stack& s) {
        if (pc >= f_.code.size() || !starts_[pc]) throw Fail{"jump to pc " + std::to_string(pc) + " is not an instruction"};
        if (s.size() > f_.max_stack)
            throw Fail{"operand stack depth " + std::to_string(s.size()) + " exceeds max_stack " +
                       std::to_string(f_.max_stack) + " at pc " + std::to_string(pc)};
        auto& st = states_[pc];
        if (!st) {
            st = s;
            work_.push_back(pc);
            return;
        }
        if (*st != s) throw Fail{"inconsistent operand stack at pc " + std::to_string(pc) + " (paths disagree)"};
    }

    void pop(uint32_t pc, Stack& s, Kind want) {
        if (s.empty()) fail(pc, "operand stack underflow");
        Kind k = s.back();
        s.pop_back();
        if (want != Kind::Any && k != want)
            fail(pc, std::string("expected ") + kind_name(want) + " operand, found " + kind_name(k));
    }

    static bool valid_kind(uint8_t k) { return k <= static_cast<uint8_t>(Kind::Ref); }

    void step(uint32_t pc) {
        Stack s = *states_[pc];
        const uint8_t* p = f_.code.data() + pc;
        Opcode op = static_cast<Opcode>(p[0]);
        const OpcodeInfo& info = opcode_info(op);
        uint32_t next = pc + info.size;
        auto u8 = [&](size_t off) { return p[1 + off]; };
        auto u16 = [&](size_t off) { return read_operand<uint16_t>(p + 1 + off); };
        auto u32 = [&](size_t off) { return read_operand<uint32_t>(p + 1 + off); };
        auto elem = [&]() {
            uint8_t k = u8(0);
            if (!valid_kind(k)) fail(pc, "invalid element kind");
            return static_cast<Kind>(k);
        };
        auto binop = [&](Kind in, Kind out) {
            pop(pc, s, in);
            pop(pc, s, in);
            s.push_back(out);
        };
        bool falls_through = true;
        using K = Kind;
        switch (op) {
            case Opcode::NOP: break;
            case Opcode::POP: pop(pc, s, K::Any); break;
            case Opcode::PUSH_INT: s.push_back(K::Int); break;
            case Opcode::PUSH_FLOAT: s.push_back(K::Float); break;
            case Opcode::PUSH_TRUE:
            case Opcode::PUSH_FALSE: s.push_back(K::Bool); break;
            case Opcode::PUSH_UNIT: s.push_back(K::Unit); break;
            case Opcode::PUSH_STR:
                if (u32(0) >= m_.strings.size()) fail(pc, "string index out of range");
                s.push_back(K::Ref);
                break;
            case Opcode::LOAD_LOCAL:
                if (u16(0) >= f_.local_kinds.size()) fail(pc, "local slot out of range");
                s.push_back(f_.local_kinds[u16(0)]);
                break;
            case Opcode::STORE_LOCAL:
                if (u16(0) >= f_.local_kinds.size()) fail(pc, "local slot out of range");
                pop(pc, s, f_.local_kinds[u16(0)]);
                break;
            case Opcode::CLEAR_LOCAL:
            case Opcode::CLOSE_UPVALUES:
                if (u16(0) >= f_.local_kinds.size()) fail(pc, "local slot out of range");
                break;
            case Opcode::LOAD_UPVAL:
                if (u16(0) >= f_.upvalues.size()) fail(pc, "upvalue index out of range");
                s.push_back(f_.upvalues[u16(0)].kind);
                break;
            case Opcode::STORE_UPVAL:
                if (u16(0) >= f_.upvalues.size()) fail(pc, "upvalue index out of range");
                pop(pc, s, f_.upvalues[u16(0)].kind);
                break;
            case Opcode::LOAD_GLOBAL:
                if (u16(0) >= m_.globals.size()) fail(pc, "global index out of range");
                s.push_back(m_.globals[u16(0)].kind);
                break;
            case Opcode::STORE_GLOBAL:
                if (u16(0) >= m_.globals.size()) fail(pc, "global index out of range");
                pop(pc, s, m_.globals[u16(0)].kind);
                break;
            case Opcode::IADD:
            case Opcode::ISUB:
            case Opcode::IMUL:
            case Opcode::IDIV:
            case Opcode::IMOD: binop(K::Int, K::Int); break;
            case Opcode::INEG: pop(pc, s, K::Int); s.push_back(K::Int); break;
            case Opcode::FADD:
            case Opcode::FSUB:
            case Opcode::FMUL:
            case Opcode::FDIV: binop(K::Float, K::Float); break;
            case Opcode::FNEG: pop(pc, s, K::Float); s.push_back(K::Float); break;
            case Opcode::NOT: pop(pc, s, K::Bool); s.push_back(K::Bool); break;
            case Opcode::I2F: pop(pc, s, K::Int); s.push_back(K::Float); break;
            case Opcode::F2I: pop(pc, s, K::Float); s.push_back(K::Int); break;
            case Opcode::IEQ: case Opcode::INE: case Opcode::ILT:
            case Opcode::ILE: case Opcode::IGT: case Opcode::IGE: binop(K::Int, K::Bool); break;
            case Opcode::FEQ: case Opcode::FNE: case Opcode::FLT:
            case Opcode::FLE: case Opcode::FGT: case Opcode::FGE: binop(K::Float, K::Bool); break;
            case Opcode::BEQ: case Opcode::BNE: binop(K::Bool, K::Bool); break;
            case Opcode::SEQ: case Opcode::SNE: case Opcode::SLT:
            case Opcode::SLE: case Opcode::SGT: case Opcode::SGE: binop(K::Ref, K::Bool); break;
            case Opcode::SCONCAT: binop(K::Ref, K::Ref); break;
            case Opcode::SLEN: pop(pc, s, K::Ref); s.push_back(K::Int); break;
            case Opcode::SINDEX:
                pop(pc, s, K::Int);
                pop(pc, s, K::Ref);
                s.push_back(K::Ref);
                break;
            case Opcode::CALL: {
                uint32_t fi = u32(0);
                if (fi >= m_.functions.size()) fail(pc, "function index out of range");
                const Function& callee = m_.functions[fi];
                if (!callee.upvalues.empty()) fail(pc, "direct call to a function that needs upvalues");
                for (size_t i = callee.num_params; i-- > 0;) pop(pc, s, callee.local_kinds[i]);
                s.push_back(callee.ret_kind);
                break;
            }
            case Opcode::CALL_INDIRECT: {
                uint16_t si = u16(0);
                if (si >= m_.signatures.size()) fail(pc, "signature index out of range");
                const Signature& sig = m_.signatures[si];
                for (size_t i = sig.params.size(); i-- > 0;) pop(pc, s, sig.params[i]);
                pop(pc, s, K::Ref);
                s.push_back(sig.ret);
                break;
            }
            case Opcode::CALL_NATIVE: {
                uint16_t id = u16(0);
                if (id >= natives().size()) fail(pc, "native function id out of range");
                const NativeDesc& d = natives()[id];
                for (size_t i = d.params.size(); i-- > 0;) pop(pc, s, d.params[i]);
                s.push_back(d.ret);
                break;
            }
            case Opcode::RETURN:
                pop(pc, s, f_.ret_kind);
                if (!s.empty()) fail(pc, "operand stack not empty at return");
                falls_through = false;
                break;
            case Opcode::CLOSURE: {
                uint32_t fi = u32(0);
                if (fi >= m_.functions.size()) fail(pc, "function index out of range");
                const Function& child = m_.functions[fi];
                for (const auto& u : child.upvalues) {
                    if (u.from_local) {
                        if (u.index >= f_.local_kinds.size()) fail(pc, "capture of a missing local slot");
                        if (f_.local_kinds[u.index] != u.kind) fail(pc, "captured local has the wrong kind");
                    } else {
                        if (u.index >= f_.upvalues.size()) fail(pc, "capture of a missing upvalue");
                        if (f_.upvalues[u.index].kind != u.kind) fail(pc, "captured upvalue has the wrong kind");
                    }
                }
                s.push_back(K::Ref);
                break;
            }
            case Opcode::MAKE_STRUCT: {
                uint16_t si = u16(0);
                if (si >= m_.structs.size()) fail(pc, "struct index out of range");
                const auto& fk = m_.structs[si].field_kinds;
                for (size_t i = fk.size(); i-- > 0;) pop(pc, s, fk[i]);
                s.push_back(K::Ref);
                break;
            }
            case Opcode::GET_FIELD:
            case Opcode::SET_FIELD: {
                uint16_t si = u16(0), fi = u16(2);
                if (si >= m_.structs.size()) fail(pc, "struct index out of range");
                if (fi >= m_.structs[si].field_kinds.size()) fail(pc, "field index out of range");
                Kind fk = m_.structs[si].field_kinds[fi];
                if (op == Opcode::SET_FIELD) {
                    pop(pc, s, fk);
                    pop(pc, s, K::Ref);
                } else {
                    pop(pc, s, K::Ref);
                    s.push_back(fk);
                }
                break;
            }
            case Opcode::MAKE_ARRAY: {
                Kind ek = elem();
                uint32_t n = u32(1);
                if (n > s.size()) fail(pc, "operand stack underflow");
                for (uint32_t i = 0; i < n; ++i) pop(pc, s, ek);
                s.push_back(K::Ref);
                break;
            }
            case Opcode::ARRAY_REPEAT: {
                Kind ek = elem();
                pop(pc, s, K::Int);
                pop(pc, s, ek);
                s.push_back(K::Ref);
                break;
            }
            case Opcode::ARRAY_GET: {
                Kind ek = elem();
                pop(pc, s, K::Int);
                pop(pc, s, K::Ref);
                s.push_back(ek);
                break;
            }
            case Opcode::ARRAY_SET: {
                Kind ek = elem();
                pop(pc, s, ek);
                pop(pc, s, K::Int);
                pop(pc, s, K::Ref);
                break;
            }
            case Opcode::ARRAY_LEN: pop(pc, s, K::Ref); s.push_back(K::Int); break;
            case Opcode::ARRAY_PUSH: {
                Kind ek = elem();
                pop(pc, s, ek);
                pop(pc, s, K::Ref);
                s.push_back(K::Unit);
                break;
            }
            case Opcode::ARRAY_POP: {
                Kind ek = elem();
                pop(pc, s, K::Ref);
                s.push_back(ek);
                break;
            }
            case Opcode::MAKE_ENUM: {
                uint16_t ei = u16(0), vi = u16(2);
                if (ei >= m_.enums.size()) fail(pc, "enum index out of range");
                if (vi >= m_.enums[ei].variants.size()) fail(pc, "variant index out of range");
                const auto& fk = m_.enums[ei].variants[vi].field_kinds;
                for (size_t i = fk.size(); i-- > 0;) pop(pc, s, fk[i]);
                s.push_back(K::Ref);
                break;
            }
            case Opcode::ENUM_TAG:
                if (u16(0) >= m_.enums.size()) fail(pc, "enum index out of range");
                pop(pc, s, K::Ref);
                s.push_back(K::Int);
                break;
            case Opcode::ENUM_FIELD: {
                uint16_t ei = u16(0), vi = u16(2), fi = u16(4);
                if (ei >= m_.enums.size()) fail(pc, "enum index out of range");
                if (vi >= m_.enums[ei].variants.size()) fail(pc, "variant index out of range");
                const auto& fk = m_.enums[ei].variants[vi].field_kinds;
                if (fi >= fk.size()) fail(pc, "field index out of range");
                pop(pc, s, K::Ref);
                s.push_back(fk[fi]);
                break;
            }
            case Opcode::JUMP:
                merge(u32(0), s);
                falls_through = false;
                break;
            case Opcode::JUMP_IF_FALSE:
            case Opcode::JUMP_IF_TRUE:
                pop(pc, s, K::Bool);
                merge(u32(0), s);
                break;
            case Opcode::UNREACHABLE: falls_through = false; break;
            case Opcode::LOAD_LOCAL2:
                if (u16(0) >= f_.local_kinds.size() || u16(2) >= f_.local_kinds.size()) fail(pc, "local slot out of range");
                s.push_back(f_.local_kinds[u16(0)]);
                s.push_back(f_.local_kinds[u16(2)]);
                break;
            case Opcode::STORE_LOAD:
                if (u16(0) >= f_.local_kinds.size() || u16(2) >= f_.local_kinds.size()) fail(pc, "local slot out of range");
                pop(pc, s, f_.local_kinds[u16(0)]);
                s.push_back(f_.local_kinds[u16(2)]);
                break;
            case Opcode::IJEQ: case Opcode::IJNE: case Opcode::IJLT:
            case Opcode::IJLE: case Opcode::IJGT: case Opcode::IJGE:
                pop(pc, s, K::Int);
                pop(pc, s, K::Int);
                merge(u32(0), s);
                break;
            case Opcode::COUNT_: fail(pc, "invalid opcode");
        }
        if (falls_through) {
            if (next >= f_.code.size()) fail(pc, "execution can fall off the end of the function");
            merge(next, s);
        }
    }

    const Module& m_;
    const Function& f_;
    std::vector<bool> starts_;
    std::vector<std::optional<Stack>> states_;
    std::vector<uint32_t> work_;
};

}  // namespace

VerifyResult verify(const Module& m) {
    VerifyResult r;
    auto err = [&](const std::string& s) {
        if (r.errors.size() < kMaxErrorsPerModule) r.errors.push_back(s);
    };
    for (size_t i = 0; i < m.structs.size(); ++i)
        if (m.structs[i].field_names.size() != m.structs[i].field_kinds.size()) err("struct " + std::to_string(i) + ": metadata mismatch");
    if (m.functions.empty()) {
        err("module has no functions");
        return r;
    }
    if (m.functions.size() > (1u << 24)) err("too many functions");
    if (m.init_function >= m.functions.size()) err("init function index out of range");
    if (m.main_function >= m.functions.size()) err("main function index out of range");
    if (!r.ok()) return r;
    for (uint32_t idx : {m.init_function, m.main_function}) {
        const Function& f = m.functions[idx];
        if (f.num_params != 0 || f.ret_kind != Kind::Unit || !f.upvalues.empty())
            err("function '" + f.name + "' cannot be an entry point (needs no parameters, no upvalues, Unit result)");
    }
    for (size_t i = 0; i < m.functions.size(); ++i) {
        const Function& f = m.functions[i];
        if (f.local_names.size() != f.local_kinds.size()) err("function '" + f.name + "': local name table mismatch");
        try {
            FunctionVerifier(m, f).run();
        } catch (const Fail& e) {
            err("function " + std::to_string(i) + " '" + f.name + "': " + e.msg);
        }
    }
    return r;
}

}  // namespace ember::bc

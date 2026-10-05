#include "codegen/codegen.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "bc/opcodes.h"
#include "common/natives.h"

namespace ember::codegen {

using bc::Opcode;
using ir::Op;
using ir::TempId;

namespace {

bool is_const_op(Op op) {
    return op == Op::ConstInt || op == Op::ConstFloat || op == Op::ConstBool || op == Op::ConstString ||
           op == Op::ConstUnit;
}

class FnGen {
public:
    FnGen(const ir::Function& f, bc::Function& out, CodegenOptions opts) : f_(f), out_(out), opts_(opts) {}

    void run() {
        out_.name = f_.name;
        out_.num_params = static_cast<uint16_t>(f_.num_params);
        out_.ret_kind = f_.ret_kind;
        out_.decl_line = f_.loc.line;
        for (const auto& l : f_.locals) {
            out_.local_kinds.push_back(l.kind);
            out_.local_names.push_back(l.name);
        }
        for (size_t i = 0; i < f_.upvalue_kinds.size(); ++i) {
            bc::UpvalueDef u;
            u.kind = f_.upvalue_kinds[i];
            u.from_local = f_.captures[i].from_local;
            u.index = static_cast<uint16_t>(f_.captures[i].index);
            u.name = f_.upvalue_names[i];
            out_.upvalues.push_back(u);
        }
        analyze();
        block_pc_.assign(f_.blocks.size(), 0);
        std::vector<ir::BlockId> order = layout();
        for (size_t k = 0; k < order.size(); ++k) {
            block_pc_[static_cast<size_t>(order[k])] = pc();
            gen_block(f_.blocks[static_cast<size_t>(order[k])], k + 1 < order.size() ? order[k + 1] : -1);
        }
        for (auto& [pos, block] : fixups_) {
            uint32_t target = block_pc_[static_cast<size_t>(block)];
            std::memcpy(out_.code.data() + pos, &target, 4);
        }
        if (out_.local_kinds.size() > std::numeric_limits<uint16_t>::max())
            throw LimitError("function '" + f_.name + "' needs more than 65535 local slots");
        out_.max_stack = static_cast<uint32_t>(max_depth_);
    }

private:
    // Block layout: greedy traces. After placing a block, place its preferred
    // successor next if it is not placed yet, so that the common edge falls
    // through: the target of a Jump, or the true target of a Branch (loop
    // bodies and `then` branches), else its false target. Remaining blocks
    // follow in creation order. Measured in docs/OPTIMIZATION.md.
    std::vector<ir::BlockId> layout() const {
        size_t n = f_.blocks.size();
        std::vector<bool> placed(n, false);
        std::vector<ir::BlockId> order;
        order.reserve(n);
        if (!opts_.block_layout) {
            for (size_t i = 0; i < n; ++i) order.push_back(static_cast<ir::BlockId>(i));
            return order;
        }
        for (size_t start = 0; start < n; ++start) {
            ir::BlockId b = static_cast<ir::BlockId>(start);
            while (b >= 0 && !placed[static_cast<size_t>(b)]) {
                placed[static_cast<size_t>(b)] = true;
                order.push_back(b);
                const ir::Block& blk = f_.blocks[static_cast<size_t>(b)];
                ir::BlockId next = -1;
                if (blk.terminated()) {
                    const ir::Instr& t = blk.instrs.back();
                    if (t.op == ir::Op::Jump) {
                        next = t.target;
                    } else if (t.op == ir::Op::Branch) {
                        next = !placed[static_cast<size_t>(t.target)] ? t.target : t.target2;
                    }
                }
                b = next;
            }
        }
        return order;
    }

    struct TempInfo {
        int uses = 0;
        ir::BlockId def_block = -1;
        bool all_uses_in_def_block = true;
        const ir::Instr* def = nullptr;
        int slot = -1;  // frame slot, when spilled / long-lived
    };

    void analyze() {
        temps_.assign(f_.temp_kinds.size(), TempInfo{});
        for (const auto& b : f_.blocks)
            for (const auto& in : b.instrs)
                if (in.dst != ir::kNoTemp) {
                    temps_[static_cast<size_t>(in.dst)].def_block = b.id;
                    temps_[static_cast<size_t>(in.dst)].def = &in;
                }
        for (const auto& b : f_.blocks)
            for (const auto& in : b.instrs)
                for (TempId a : in.args) {
                    auto& t = temps_[static_cast<size_t>(a)];
                    ++t.uses;
                    if (t.def_block != b.id) t.all_uses_in_def_block = false;
                }
    }

    bool remat(TempId t) const { return is_const_op(temps_[static_cast<size_t>(t)].def->op); }
    bool stackable(TempId t) const {
        const auto& ti = temps_[static_cast<size_t>(t)];
        return ti.uses == 1 && ti.all_uses_in_def_block;
    }

    // ---- emission
    uint32_t pc() const { return static_cast<uint32_t>(out_.code.size()); }
    void op(Opcode o, int pops, int pushes) {
        note_line();
        last_pos_ = static_cast<int64_t>(out_.code.size());
        last_op_ = o;
        out_.code.push_back(static_cast<uint8_t>(o));
        depth_ -= pops;
        depth_ += pushes;
        max_depth_ = std::max(max_depth_, depth_);
    }
    void u8(uint8_t v) { out_.code.push_back(v); }
    void u16(uint64_t v) {
        if (v > 0xFFFF) throw LimitError("function '" + f_.name + "': operand exceeds 65535");
        uint16_t x = static_cast<uint16_t>(v);
        raw(&x, 2);
    }
    void u32(uint64_t v) {
        if (v > 0xFFFFFFFFull) throw LimitError("function '" + f_.name + "': operand exceeds 2^32");
        uint32_t x = static_cast<uint32_t>(v);
        raw(&x, 4);
    }
    void raw(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        out_.code.insert(out_.code.end(), b, b + n);
    }
    void note_line() {
        if (!cur_loc_.valid()) return;
        if (!out_.lines.empty() && out_.lines.back().line == cur_loc_.line && out_.lines.back().column == cur_loc_.column)
            return;
        if (!out_.lines.empty() && out_.lines.back().pc == pc()) {
            out_.lines.back().line = cur_loc_.line;
            out_.lines.back().column = cur_loc_.column;
            return;
        }
        out_.lines.push_back(bc::LineEntry{pc(), cur_loc_.line, cur_loc_.column});
    }

    int slot_for(TempId t) {
        auto& ti = temps_[static_cast<size_t>(t)];
        if (ti.slot < 0) {
            ti.slot = static_cast<int>(out_.local_kinds.size());
            out_.local_kinds.push_back(f_.temp_kinds[static_cast<size_t>(t)]);
            out_.local_names.push_back("");
        }
        return ti.slot;
    }

    void store_slot(int slot) {
        op(Opcode::STORE_LOCAL, 1, 0);
        u16(static_cast<uint64_t>(slot));
    }

    // Superinstructions: a LOAD_LOCAL directly after a LOAD_LOCAL or a
    // STORE_LOCAL in the same block is merged into it. Safe because nothing
    // jumps into the middle of a block (jump targets are block starts, where
    // last_pos_ is reset).
    void load_slot(int slot) {
        if (opts_.superinstructions && last_pos_ >= 0 &&
            (last_op_ == Opcode::LOAD_LOCAL || last_op_ == Opcode::STORE_LOCAL)) {
            Opcode fused = last_op_ == Opcode::LOAD_LOCAL ? Opcode::LOAD_LOCAL2 : Opcode::STORE_LOAD;
            out_.code[static_cast<size_t>(last_pos_)] = static_cast<uint8_t>(fused);
            u16(static_cast<uint64_t>(slot));
            last_op_ = fused;
            ++depth_;
            max_depth_ = std::max(max_depth_, depth_);
            return;
        }
        op(Opcode::LOAD_LOCAL, 0, 1);
        u16(static_cast<uint64_t>(slot));
    }

    // An integer comparison directly followed by a conditional jump becomes
    // one compare-and-jump (the comparison is inverted for "jump if false";
    // exact for integers, so floats are never fused).
    bool fuse_compare_jump(Opcode jump, ir::BlockId target) {
        if (!opts_.superinstructions || last_pos_ < 0 || last_pos_ + 1 != static_cast<int64_t>(out_.code.size())) return false;
        int c = static_cast<int>(last_op_) - static_cast<int>(Opcode::IEQ);
        if (c < 0 || c > 5) return false;  // IEQ INE ILT ILE IGT IGE
        static const int kNegate[6] = {1, 0, 5, 4, 3, 2};  // eq<->ne, lt<->ge, le<->gt
        if (jump == Opcode::JUMP_IF_FALSE) c = kNegate[c];
        out_.code.pop_back();
        Opcode fused = static_cast<Opcode>(static_cast<int>(Opcode::IJEQ) + c);
        // The comparison already pushed its Bool (depth accounted); the fused
        // instruction consumes it like the jump would.
        depth_ += 1;  // undo the compare's net effect ...
        op(fused, 2, 0);  // ... and apply the fused one: pop 2 ints
        fixups_.push_back({pc(), target});
        u32(0);
        return true;
    }

    void emit_const(const ir::Instr& in) {
        switch (in.op) {
            case Op::ConstInt:
                op(Opcode::PUSH_INT, 0, 1);
                raw(&in.imm, 8);
                break;
            case Op::ConstFloat:
                op(Opcode::PUSH_FLOAT, 0, 1);
                raw(&in.fimm, 8);
                break;
            case Op::ConstBool: op(in.imm ? Opcode::PUSH_TRUE : Opcode::PUSH_FALSE, 0, 1); break;
            case Op::ConstString:
                op(Opcode::PUSH_STR, 0, 1);
                u32(static_cast<uint64_t>(in.imm));
                break;
            case Op::ConstUnit: op(Opcode::PUSH_UNIT, 0, 1); break;
            default: break;
        }
    }

    void spill_pending() {
        // Pop from the top: each value goes to its own slot.
        while (!pending_.empty()) {
            TempId t = pending_.back();
            pending_.pop_back();
            store_slot(slot_for(t));
        }
    }

    bool is_pending(TempId t) const { return std::find(pending_.begin(), pending_.end(), t) != pending_.end(); }

    void load_arg(TempId t) {
        if (remat(t)) {
            Span saved = cur_loc_;
            emit_const(*temps_[static_cast<size_t>(t)].def);
            cur_loc_ = saved;
            return;
        }
        load_slot(slot_for(t));
    }

    // Arrange for the instruction's arguments to be the top values of the
    // operand stack, in order.
    void prepare_args(const std::vector<TempId>& args) {
        size_t n = args.size(), m = pending_.size();
        for (size_t k = std::min(n, m) + 1; k-- > 0;) {
            bool match = true;
            for (size_t j = 0; j < k && match; ++j) match = pending_[m - k + j] == args[j];
            for (size_t j = k; j < n && match; ++j) match = !is_pending(args[j]);
            if (!match) continue;
            pending_.resize(m - k);
            for (size_t j = k; j < n; ++j) load_arg(args[j]);
            return;
        }
        spill_pending();
        for (TempId a : args) load_arg(a);
    }

    // After an instruction that pushed the value of temp dst.
    void place_result(TempId dst) {
        const auto& ti = temps_[static_cast<size_t>(dst)];
        if (ti.uses == 0) {
            op(Opcode::POP, 1, 0);
        } else if (stackable(dst)) {
            pending_.push_back(dst);
        } else {
            store_slot(slot_for(dst));
        }
    }

    void jump_to(Opcode o, ir::BlockId b) {
        if (o != Opcode::JUMP && fuse_compare_jump(o, b)) return;
        op(o, o == Opcode::JUMP ? 0 : 1, 0);
        fixups_.push_back({pc(), b});
        u32(0);
    }

    void gen_block(const ir::Block& b, ir::BlockId next) {
        pending_.clear();
        depth_ = 0;
        last_pos_ = -1;  // block start: a jump target, never fuse across it
        for (const auto& in : b.instrs) {
            cur_loc_ = in.loc;
            if (in.dst != ir::kNoTemp && is_const_op(in.op)) continue;  // materialized at each use
            if (in.dst != ir::kNoTemp && temps_[static_cast<size_t>(in.dst)].uses == 0 && ir::is_pure(in.op)) continue;
            prepare_args(in.args);
            gen_instr(in, next);
            if (in.dst != ir::kNoTemp) place_result(in.dst);
        }
        if (!pending_.empty() || depth_ != 0)
            throw std::logic_error("codegen: operand stack not empty at end of block in " + f_.name);
    }

    void gen_instr(const ir::Instr& in, ir::BlockId next) {
        int argc = static_cast<int>(in.args.size());
        int pushes = in.dst != ir::kNoTemp ? 1 : 0;
        auto simple = [&](Opcode o) { op(o, argc, pushes); };
        auto cmp = [&](Opcode base) {
            op(static_cast<Opcode>(static_cast<int>(base) + static_cast<int>(in.imm)), 2, 1);
        };
        switch (in.op) {
            case Op::ConstInt:
            case Op::ConstFloat:
            case Op::ConstBool:
            case Op::ConstString:
            case Op::ConstUnit: emit_const(in); break;
            case Op::LoadLocal: load_slot(static_cast<int>(in.imm)); break;
            case Op::StoreLocal: op(Opcode::STORE_LOCAL, 1, 0); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::ClearLocal: op(Opcode::CLEAR_LOCAL, 0, 0); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::LoadUpval: op(Opcode::LOAD_UPVAL, 0, 1); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::StoreUpval: op(Opcode::STORE_UPVAL, 1, 0); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::LoadGlobal: op(Opcode::LOAD_GLOBAL, 0, 1); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::StoreGlobal: op(Opcode::STORE_GLOBAL, 1, 0); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::IAdd: simple(Opcode::IADD); break;
            case Op::ISub: simple(Opcode::ISUB); break;
            case Op::IMul: simple(Opcode::IMUL); break;
            case Op::IDiv: simple(Opcode::IDIV); break;
            case Op::IMod: simple(Opcode::IMOD); break;
            case Op::INeg: simple(Opcode::INEG); break;
            case Op::FAdd: simple(Opcode::FADD); break;
            case Op::FSub: simple(Opcode::FSUB); break;
            case Op::FMul: simple(Opcode::FMUL); break;
            case Op::FDiv: simple(Opcode::FDIV); break;
            case Op::FNeg: simple(Opcode::FNEG); break;
            case Op::Not: simple(Opcode::NOT); break;
            case Op::I2F: simple(Opcode::I2F); break;
            case Op::F2I: simple(Opcode::F2I); break;
            // CmpOp order (Eq, Ne, Lt, Le, Gt, Ge) matches the opcode order.
            case Op::ICmp: cmp(Opcode::IEQ); break;
            case Op::FCmp: cmp(Opcode::FEQ); break;
            case Op::BCmp: cmp(Opcode::BEQ); break;
            case Op::SCmp: cmp(Opcode::SEQ); break;
            case Op::SConcat: simple(Opcode::SCONCAT); break;
            case Op::SLen: simple(Opcode::SLEN); break;
            case Op::SIndex: simple(Opcode::SINDEX); break;
            case Op::Call: simple(Opcode::CALL); u32(static_cast<uint64_t>(in.imm)); break;
            case Op::CallIndirect: simple(Opcode::CALL_INDIRECT); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::CallNative: simple(Opcode::CALL_NATIVE); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::MakeClosure: op(Opcode::CLOSURE, 0, 1); u32(static_cast<uint64_t>(in.imm)); break;
            case Op::CloseUpvalues: op(Opcode::CLOSE_UPVALUES, 0, 0); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::MakeStruct: simple(Opcode::MAKE_STRUCT); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::GetField:
            case Op::SetField:
                simple(in.op == Op::GetField ? Opcode::GET_FIELD : Opcode::SET_FIELD);
                u16(static_cast<uint64_t>(in.imm));
                u16(static_cast<uint64_t>(in.imm2));
                break;
            case Op::MakeArray:
                simple(Opcode::MAKE_ARRAY);
                u8(static_cast<uint8_t>(in.elem));
                u32(in.args.size());
                break;
            case Op::ArrayRepeat: simple(Opcode::ARRAY_REPEAT); u8(static_cast<uint8_t>(in.elem)); break;
            case Op::ArrayGet: simple(Opcode::ARRAY_GET); u8(static_cast<uint8_t>(in.elem)); break;
            case Op::ArraySet: simple(Opcode::ARRAY_SET); u8(static_cast<uint8_t>(in.elem)); break;
            case Op::ArrayLen: simple(Opcode::ARRAY_LEN); break;
            case Op::ArrayPush: simple(Opcode::ARRAY_PUSH); u8(static_cast<uint8_t>(in.elem)); break;
            case Op::ArrayPop: simple(Opcode::ARRAY_POP); u8(static_cast<uint8_t>(in.elem)); break;
            case Op::MakeEnum:
                simple(Opcode::MAKE_ENUM);
                u16(static_cast<uint64_t>(in.imm));
                u16(static_cast<uint64_t>(in.imm2));
                break;
            case Op::EnumTag: simple(Opcode::ENUM_TAG); u16(static_cast<uint64_t>(in.imm)); break;
            case Op::EnumField:
                simple(Opcode::ENUM_FIELD);
                u16(static_cast<uint64_t>(in.imm));
                u16(static_cast<uint64_t>(in.imm2));
                u16(static_cast<uint64_t>(in.imm3));
                break;
            case Op::Jump:
                if (in.target != next) jump_to(Opcode::JUMP, in.target);
                break;
            case Op::Branch:
                if (in.target == next) {
                    jump_to(Opcode::JUMP_IF_FALSE, in.target2);
                } else if (in.target2 == next) {
                    jump_to(Opcode::JUMP_IF_TRUE, in.target);
                } else {
                    jump_to(Opcode::JUMP_IF_TRUE, in.target);
                    jump_to(Opcode::JUMP, in.target2);
                }
                break;
            case Op::Return: op(Opcode::RETURN, 1, 0); break;
            case Op::Unreachable: op(Opcode::UNREACHABLE, 0, 0); break;
        }
    }

    const ir::Function& f_;
    bc::Function& out_;
    CodegenOptions opts_;
    std::vector<TempInfo> temps_;
    std::vector<TempId> pending_;
    std::vector<uint32_t> block_pc_;
    std::vector<std::pair<uint32_t, ir::BlockId>> fixups_;
    int depth_ = 0;
    int max_depth_ = 0;
    int64_t last_pos_ = -1;  // start of the last emitted instruction in this block
    Opcode last_op_ = Opcode::NOP;
    Span cur_loc_;
};

}  // namespace

bc::Module generate(const ir::Module& m, const std::string& source_name, CodegenOptions opts) {
    bc::Module out;
    out.source_name = source_name;
    out.strings = m.strings;
    for (const auto& s : m.structs) out.structs.push_back(bc::StructDef{s.name, s.field_names, s.field_kinds});
    for (const auto& e : m.enums) {
        bc::EnumDef d;
        d.name = e.name;
        for (const auto& v : e.variants) d.variants.push_back(bc::VariantDef{v.name, v.field_kinds});
        out.enums.push_back(std::move(d));
    }
    for (const auto& s : m.signatures) out.signatures.push_back(bc::Signature{s.params, s.ret});
    for (const auto& g : m.globals) out.globals.push_back(bc::GlobalDef{g.name, g.kind});
    if (m.globals.size() > 65536) throw LimitError("program has more than 65536 globals");
    if (m.signatures.size() > 65536) throw LimitError("program has more than 65536 function signatures");
    if (m.structs.size() > 65536 || m.enums.size() > 65536) throw LimitError("program has too many types");
    out.functions.resize(m.functions.size());
    for (size_t i = 0; i < m.functions.size(); ++i) FnGen(m.functions[i], out.functions[i], opts).run();
    out.init_function = m.init_function;
    out.main_function = m.main_function;
    return out;
}

}  // namespace ember::codegen

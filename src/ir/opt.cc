#include "ir/opt.h"

#include <cmath>
#include <cstdint>
#include <optional>

#include "ir/validate.h"

namespace ember::ir {

std::string OptStats::summary() const {
    return "folded=" + std::to_string(folded) + " branches=" + std::to_string(branches_folded) +
           " blocks_removed=" + std::to_string(blocks_removed) + " threaded=" + std::to_string(jumps_threaded) +
           " dead=" + std::to_string(dead_removed);
}

namespace {

struct ConstVal {
    Kind kind;
    int64_t i = 0;
    double f = 0;
    bool b = false;
};

bool cmp_int(CmpOp c, int64_t a, int64_t b) {
    switch (c) {
        case CmpOp::Eq: return a == b;
        case CmpOp::Ne: return a != b;
        case CmpOp::Lt: return a < b;
        case CmpOp::Le: return a <= b;
        case CmpOp::Gt: return a > b;
        case CmpOp::Ge: return a >= b;
    }
    return false;
}

bool cmp_float(CmpOp c, double a, double b) {
    switch (c) {
        case CmpOp::Eq: return a == b;
        case CmpOp::Ne: return a != b;
        case CmpOp::Lt: return a < b;
        case CmpOp::Le: return a <= b;
        case CmpOp::Gt: return a > b;
        case CmpOp::Ge: return a >= b;
    }
    return false;
}

// Evaluate an instruction over constant operands with exactly the runtime
// semantics; returns nothing if it is not foldable or would trap.
std::optional<ConstVal> fold(const Instr& in, const std::vector<std::optional<ConstVal>>& cv) {
    auto arg = [&](size_t k) -> const std::optional<ConstVal>& { return cv[static_cast<size_t>(in.args[k])]; };
    for (size_t k = 0; k < in.args.size(); ++k)
        if (!arg(k)) return std::nullopt;
    ConstVal r{Kind::Int};
    int64_t out = 0;
    switch (in.op) {
        case Op::IAdd:
            if (__builtin_add_overflow(arg(0)->i, arg(1)->i, &out)) return std::nullopt;
            r.i = out;
            return r;
        case Op::ISub:
            if (__builtin_sub_overflow(arg(0)->i, arg(1)->i, &out)) return std::nullopt;
            r.i = out;
            return r;
        case Op::IMul:
            if (__builtin_mul_overflow(arg(0)->i, arg(1)->i, &out)) return std::nullopt;
            r.i = out;
            return r;
        case Op::IDiv:
        case Op::IMod: {
            int64_t a = arg(0)->i, b = arg(1)->i;
            if (b == 0 || (a == INT64_MIN && b == -1)) return std::nullopt;
            r.i = in.op == Op::IDiv ? a / b : a % b;
            return r;
        }
        case Op::INeg:
            if (arg(0)->i == INT64_MIN) return std::nullopt;
            r.i = -arg(0)->i;
            return r;
        case Op::FAdd: r = {Kind::Float}; r.f = arg(0)->f + arg(1)->f; return r;
        case Op::FSub: r = {Kind::Float}; r.f = arg(0)->f - arg(1)->f; return r;
        case Op::FMul: r = {Kind::Float}; r.f = arg(0)->f * arg(1)->f; return r;
        case Op::FDiv: r = {Kind::Float}; r.f = arg(0)->f / arg(1)->f; return r;
        case Op::FNeg: r = {Kind::Float}; r.f = -arg(0)->f; return r;
        case Op::Not: r = {Kind::Bool}; r.b = !arg(0)->b; return r;
        case Op::I2F: r = {Kind::Float}; r.f = static_cast<double>(arg(0)->i); return r;
        case Op::F2I: {
            // Same rule as the VM: trunc(d) must be representable (NaN fails).
            double t = std::trunc(arg(0)->f);
            if (!(t >= -9223372036854775808.0 && t < 9223372036854775808.0)) return std::nullopt;
            r.i = static_cast<int64_t>(t);
            return r;
        }
        case Op::ICmp: r = {Kind::Bool}; r.b = cmp_int(static_cast<CmpOp>(in.imm), arg(0)->i, arg(1)->i); return r;
        case Op::FCmp: r = {Kind::Bool}; r.b = cmp_float(static_cast<CmpOp>(in.imm), arg(0)->f, arg(1)->f); return r;
        case Op::BCmp:
            r = {Kind::Bool};
            r.b = static_cast<CmpOp>(in.imm) == CmpOp::Eq ? arg(0)->b == arg(1)->b : arg(0)->b != arg(1)->b;
            return r;
        default: return std::nullopt;
    }
}

void make_const(Instr& in, const ConstVal& v) {
    in.args.clear();
    switch (v.kind) {
        case Kind::Int: in.op = Op::ConstInt; in.imm = v.i; break;
        case Kind::Float: in.op = Op::ConstFloat; in.fimm = v.f; break;
        case Kind::Bool: in.op = Op::ConstBool; in.imm = v.b; break;
        default: break;
    }
}

void constant_fold(Function& f, OptStats& st) {
    std::vector<std::optional<ConstVal>> cv(f.temp_kinds.size());
    // Temps are defined once and definitions dominate uses, so a single
    // pass in block order sees constant definitions before most uses; the
    // driver iterates to a fixed point for the rest.
    for (auto& b : f.blocks) {
        for (auto& in : b.instrs) {
            if (in.dst == kNoTemp) continue;
            auto& slot = cv[static_cast<size_t>(in.dst)];
            switch (in.op) {
                case Op::ConstInt: slot = ConstVal{Kind::Int, in.imm}; break;
                case Op::ConstFloat: { ConstVal c{Kind::Float}; c.f = in.fimm; slot = c; break; }
                case Op::ConstBool: { ConstVal c{Kind::Bool}; c.b = in.imm != 0; slot = c; break; }
                default: {
                    bool all_args_known = true;
                    for (TempId a : in.args) all_args_known &= cv[static_cast<size_t>(a)].has_value();
                    if (!all_args_known || in.args.empty()) break;
                    if (auto r = fold(in, cv)) {
                        make_const(in, *r);
                        slot = r;
                        ++st.folded;
                    }
                }
            }
        }
    }
    // Branches on constants become jumps.
    for (auto& b : f.blocks) {
        if (!b.terminated()) continue;
        Instr& t = b.instrs.back();
        if (t.op != Op::Branch) continue;
        auto& c = cv[static_cast<size_t>(t.args[0])];
        if (!c) continue;
        BlockId to = c->b ? t.target : t.target2;
        t.op = Op::Jump;
        t.args.clear();
        t.target = to;
        t.target2 = -1;
        ++st.branches_folded;
    }
}

// Branch on `Not c` (single use) becomes a branch on `c` with the targets
// swapped; the Not is then dead and removed by DCE.
void branch_on_not(Function& f, OptStats& st) {
    std::vector<int> uses(f.temp_kinds.size(), 0);
    std::vector<const Instr*> def(f.temp_kinds.size(), nullptr);
    for (auto& b : f.blocks)
        for (auto& in : b.instrs) {
            for (TempId a : in.args) ++uses[static_cast<size_t>(a)];
            if (in.dst != kNoTemp) def[static_cast<size_t>(in.dst)] = &in;
        }
    for (auto& b : f.blocks) {
        if (!b.terminated()) continue;
        Instr& t = b.instrs.back();
        if (t.op != Op::Branch) continue;
        const Instr* d = def[static_cast<size_t>(t.args[0])];
        if (!d || d->op != Op::Not || uses[static_cast<size_t>(t.args[0])] != 1) continue;
        t.args[0] = d->args[0];
        std::swap(t.target, t.target2);
        ++st.branches_folded;
    }
}

// Redirect edges that point to a block containing only `Jump X`.
void thread_jumps(Function& f, OptStats& st) {
    auto forward = [&](BlockId b) {
        // Follow chains of empty jump blocks; bounded to avoid cycles.
        for (int steps = 0; steps < 64; ++steps) {
            const Block& blk = f.blocks[static_cast<size_t>(b)];
            if (blk.instrs.size() != 1 || blk.instrs[0].op != Op::Jump || blk.instrs[0].target == b) return b;
            b = blk.instrs[0].target;
        }
        return b;
    };
    for (auto& b : f.blocks) {
        if (!b.terminated()) continue;
        Instr& t = b.instrs.back();
        if (t.op == Op::Jump || t.op == Op::Branch) {
            BlockId n = forward(t.target);
            if (n != t.target) { t.target = n; ++st.jumps_threaded; }
        }
        if (t.op == Op::Branch) {
            BlockId n = forward(t.target2);
            if (n != t.target2) { t.target2 = n; ++st.jumps_threaded; }
            if (t.target == t.target2) {
                // Both edges go to the same block: the condition is only
                // needed for its (already evaluated) temp.
                t.op = Op::Jump;
                t.args.clear();
                t.target2 = -1;
            }
        }
    }
}

// Merge B into A when A ends with `Jump B` and A is B's only predecessor.
void merge_blocks(Function& f, OptStats& st) {
    bool changed = true;
    while (changed) {
        changed = false;
        auto preds = predecessors(f);
        for (auto& a : f.blocks) {
            if (!a.terminated() || a.instrs.back().op != Op::Jump) continue;
            BlockId bid = a.instrs.back().target;
            if (bid == 0 || bid == a.id) continue;
            if (preds[static_cast<size_t>(bid)].size() != 1) continue;
            Block& b = f.blocks[static_cast<size_t>(bid)];
            if (b.instrs.empty()) continue;
            a.instrs.pop_back();
            for (auto& in : b.instrs) a.instrs.push_back(std::move(in));
            b.instrs.clear();
            // Leave b as an unreachable self-contained block; it is removed below.
            Instr un;
            un.op = Op::Unreachable;
            b.instrs.push_back(un);
            ++st.blocks_removed;
            changed = true;
            break;  // predecessor lists are stale now
        }
    }
}

void dead_code(Function& f, OptStats& st) {
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<int> uses(f.temp_kinds.size(), 0);
        for (auto& b : f.blocks)
            for (auto& in : b.instrs)
                for (TempId a : in.args) ++uses[static_cast<size_t>(a)];
        for (auto& b : f.blocks) {
            std::vector<Instr> kept;
            kept.reserve(b.instrs.size());
            for (auto& in : b.instrs) {
                if (in.dst != kNoTemp && uses[static_cast<size_t>(in.dst)] == 0 && is_pure(in.op)) {
                    ++st.dead_removed;
                    changed = true;
                    continue;
                }
                kept.push_back(std::move(in));
            }
            b.instrs = std::move(kept);
        }
    }
}

}  // namespace

OptStats optimize(Module& m, int level) {
    OptStats st;
    for (auto& f : m.functions) {
        size_t before = f.blocks.size();
        remove_unreachable_blocks(f);
        st.blocks_removed += static_cast<int>(before - f.blocks.size());
        if (level >= 1) {
            for (int iter = 0; iter < 8; ++iter) {
                OptStats prev = st;
                constant_fold(f, st);
                branch_on_not(f, st);
                thread_jumps(f, st);
                remove_unreachable_blocks(f);
                merge_blocks(f, st);
                remove_unreachable_blocks(f);
                dead_code(f, st);
                if (st.folded == prev.folded && st.branches_folded == prev.branches_folded &&
                    st.jumps_threaded == prev.jumps_threaded && st.dead_removed == prev.dead_removed &&
                    st.blocks_removed == prev.blocks_removed)
                    break;
            }
        }
    }
    return st;
}

}  // namespace ember::ir

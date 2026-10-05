#include "ir/validate.h"

#include <algorithm>
#include <sstream>

#include "common/natives.h"

namespace ember::ir {

std::vector<std::vector<BlockId>> predecessors(const Function& f) {
    std::vector<std::vector<BlockId>> preds(f.blocks.size());
    for (const auto& b : f.blocks) {
        if (!b.terminated()) continue;
        const Instr& t = b.instrs.back();
        auto add = [&](BlockId to) {
            if (to >= 0 && static_cast<size_t>(to) < f.blocks.size()) {
                auto& p = preds[static_cast<size_t>(to)];
                if (std::find(p.begin(), p.end(), b.id) == p.end()) p.push_back(b.id);
            }
        };
        if (t.op == Op::Jump) add(t.target);
        if (t.op == Op::Branch) {
            add(t.target);
            add(t.target2);
        }
    }
    return preds;
}

static std::vector<bool> reachable_blocks(const Function& f) {
    std::vector<bool> seen(f.blocks.size(), false);
    if (f.blocks.empty()) return seen;
    std::vector<BlockId> work{0};
    seen[0] = true;
    while (!work.empty()) {
        BlockId b = work.back();
        work.pop_back();
        const auto& blk = f.blocks[static_cast<size_t>(b)];
        if (!blk.terminated()) continue;
        const Instr& t = blk.instrs.back();
        std::vector<BlockId> succ;
        if (t.op == Op::Jump) succ = {t.target};
        if (t.op == Op::Branch) succ = {t.target, t.target2};
        for (BlockId s : succ) {
            if (s >= 0 && static_cast<size_t>(s) < f.blocks.size() && !seen[static_cast<size_t>(s)]) {
                seen[static_cast<size_t>(s)] = true;
                work.push_back(s);
            }
        }
    }
    return seen;
}

void remove_unreachable_blocks(Function& f) {
    auto seen = reachable_blocks(f);
    std::vector<BlockId> remap(f.blocks.size(), -1);
    std::vector<Block> kept;
    for (size_t i = 0; i < f.blocks.size(); ++i) {
        if (!seen[i]) continue;
        remap[i] = static_cast<BlockId>(kept.size());
        kept.push_back(std::move(f.blocks[i]));
    }
    for (auto& b : kept) {
        b.id = remap[static_cast<size_t>(b.id)];
        if (!b.terminated()) continue;
        Instr& t = b.instrs.back();
        if (t.op == Op::Jump || t.op == Op::Branch) t.target = remap[static_cast<size_t>(t.target)];
        if (t.op == Op::Branch) t.target2 = remap[static_cast<size_t>(t.target2)];
    }
    f.blocks = std::move(kept);
}

namespace {

// Iterative dominator computation (Cooper, Harvey, Kennedy) over reachable
// blocks in reverse post-order.
struct Dominators {
    std::vector<int> idom;  // -1 for unreachable, entry's idom is itself
    std::vector<int> rpo_index;

    explicit Dominators(const Function& f) {
        size_t n = f.blocks.size();
        idom.assign(n, -1);
        rpo_index.assign(n, -1);
        if (n == 0) return;
        std::vector<BlockId> post;
        std::vector<int> state(n, 0);
        std::vector<std::pair<BlockId, int>> stack{{0, 0}};
        state[0] = 1;
        auto succs = [&](BlockId b) {
            std::vector<BlockId> s;
            const auto& blk = f.blocks[static_cast<size_t>(b)];
            if (!blk.terminated()) return s;
            const Instr& t = blk.instrs.back();
            if (t.op == Op::Jump) s.push_back(t.target);
            if (t.op == Op::Branch) {
                s.push_back(t.target);
                s.push_back(t.target2);
            }
            return s;
        };
        while (!stack.empty()) {
            auto& [b, i] = stack.back();
            auto s = succs(b);
            if (static_cast<size_t>(i) < s.size()) {
                BlockId nb = s[static_cast<size_t>(i)];
                ++i;
                if (nb >= 0 && static_cast<size_t>(nb) < n && state[static_cast<size_t>(nb)] == 0) {
                    state[static_cast<size_t>(nb)] = 1;
                    stack.push_back({nb, 0});
                }
            } else {
                post.push_back(b);
                stack.pop_back();
            }
        }
        std::vector<BlockId> rpo(post.rbegin(), post.rend());
        for (size_t i = 0; i < rpo.size(); ++i) rpo_index[static_cast<size_t>(rpo[i])] = static_cast<int>(i);
        auto preds = predecessors(f);
        idom[0] = 0;
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t i = 1; i < rpo.size(); ++i) {
                BlockId b = rpo[i];
                int new_idom = -1;
                for (BlockId p : preds[static_cast<size_t>(b)]) {
                    if (idom[static_cast<size_t>(p)] == -1) continue;
                    new_idom = new_idom == -1 ? p : intersect(p, new_idom);
                }
                if (new_idom != idom[static_cast<size_t>(b)]) {
                    idom[static_cast<size_t>(b)] = new_idom;
                    changed = true;
                }
            }
        }
    }

    int intersect(int a, int b) const {
        while (a != b) {
            while (rpo_index[static_cast<size_t>(a)] > rpo_index[static_cast<size_t>(b)]) a = idom[static_cast<size_t>(a)];
            while (rpo_index[static_cast<size_t>(b)] > rpo_index[static_cast<size_t>(a)]) b = idom[static_cast<size_t>(b)];
        }
        return a;
    }

    bool dominates(BlockId a, BlockId b) const {
        if (idom[static_cast<size_t>(b)] == -1) return false;
        while (true) {
            if (a == b) return true;
            if (b == 0) return false;
            b = idom[static_cast<size_t>(b)];
        }
    }
};

class Validator {
public:
    Validator(const Module& m, std::vector<std::string>& errs) : m_(m), errs_(errs) {}

    void function(const Function& f, size_t index) {
        f_ = &f;
        fname_ = "fn" + std::to_string(index) + " " + f.name;
        if (f.blocks.empty()) {
            err("function has no blocks");
            return;
        }
        if (f.param_kinds.size() != f.num_params || f.locals.size() < f.num_params) err("parameter metadata mismatch");
        for (uint32_t i = 0; i < f.num_params && i < f.locals.size() && i < f.param_kinds.size(); ++i)
            if (f.locals[i].kind != f.param_kinds[i]) err("parameter " + std::to_string(i) + " kind mismatch");
        if (f.upvalue_kinds.size() != f.captures.size() || f.upvalue_names.size() != f.captures.size())
            err("upvalue metadata mismatch");
        // Definitions.
        std::vector<std::pair<int, int>> def(f.temp_kinds.size(), {-1, -1});  // block, position
        for (const auto& b : f.blocks) {
            if (b.id != static_cast<BlockId>(&b - f.blocks.data())) err("block id mismatch");
            for (size_t i = 0; i < b.instrs.size(); ++i) {
                const Instr& in = b.instrs[i];
                if (in.dst == kNoTemp) continue;
                if (in.dst < 0 || static_cast<size_t>(in.dst) >= f.temp_kinds.size()) {
                    err(at(b, i) + "destination temp out of range");
                    continue;
                }
                auto& d = def[static_cast<size_t>(in.dst)];
                if (d.first != -1) err(at(b, i) + "temp %" + std::to_string(in.dst) + " defined twice");
                d = {b.id, static_cast<int>(i)};
            }
        }
        Dominators dom(f);
        for (const auto& b : f.blocks) {
            if (b.instrs.empty() || !is_terminator(b.instrs.back().op)) {
                err("b" + std::to_string(b.id) + " does not end with a terminator");
            }
            bool reachable = dom.idom[static_cast<size_t>(b.id)] != -1;
            for (size_t i = 0; i < b.instrs.size(); ++i) {
                const Instr& in = b.instrs[i];
                if (is_terminator(in.op) && i + 1 != b.instrs.size()) err(at(b, i) + "terminator in the middle of a block");
                for (TempId a : in.args) {
                    if (a < 0 || static_cast<size_t>(a) >= f.temp_kinds.size()) {
                        err(at(b, i) + "argument temp out of range");
                        continue;
                    }
                    auto d = def[static_cast<size_t>(a)];
                    if (d.first == -1) {
                        err(at(b, i) + "use of undefined temp %" + std::to_string(a));
                    } else if (reachable) {
                        bool ok = d.first == b.id ? d.second < static_cast<int>(i) : dom.dominates(d.first, b.id);
                        if (!ok) err(at(b, i) + "definition of %" + std::to_string(a) + " does not dominate its use");
                    }
                }
                check_kinds(b, i, in);
            }
        }
    }

private:
    std::string at(const Block& b, size_t i) const {
        return "b" + std::to_string(b.id) + "[" + std::to_string(i) + "] " + op_name(b.instrs[i].op) + ": ";
    }
    void err(const std::string& s) { errs_.push_back(fname_ + ": " + s); }

    Kind tk(TempId t) const {
        if (t < 0 || static_cast<size_t>(t) >= f_->temp_kinds.size()) return Kind::Any;
        return f_->temp_kinds[static_cast<size_t>(t)];
    }

    void expect(const Block& b, size_t i, const Instr& in, std::vector<Kind> args, Kind dst) {
        if (in.args.size() != args.size()) {
            err(at(b, i) + "expected " + std::to_string(args.size()) + " arguments, got " + std::to_string(in.args.size()));
            return;
        }
        for (size_t k = 0; k < args.size(); ++k)
            if (args[k] != Kind::Any && tk(in.args[k]) != args[k])
                err(at(b, i) + "argument " + std::to_string(k) + " has kind " + kind_name(tk(in.args[k])) + ", expected " +
                    kind_name(args[k]));
        if (dst == Kind::Any) return;  // "no result"
        if (in.dst == kNoTemp) {
            err(at(b, i) + "missing destination");
        } else if (tk(in.dst) != dst) {
            err(at(b, i) + "destination has kind " + std::string(kind_name(tk(in.dst))) + ", expected " + kind_name(dst));
        }
    }
    void no_dst(const Block& b, size_t i, const Instr& in) {
        if (in.dst != kNoTemp) err(at(b, i) + "unexpected destination");
    }
    bool in_range(const Block& b, size_t i, int64_t v, size_t n, const char* what) {
        if (v < 0 || static_cast<size_t>(v) >= n) {
            err(at(b, i) + what + " index " + std::to_string(v) + " out of range");
            return false;
        }
        return true;
    }
    void check_target(const Block& b, size_t i, BlockId t) {
        if (t < 0 || static_cast<size_t>(t) >= f_->blocks.size()) err(at(b, i) + "jump target out of range");
    }

    void check_kinds(const Block& b, size_t i, const Instr& in) {
        using K = Kind;
        const Function& f = *f_;
        switch (in.op) {
            case Op::ConstInt: expect(b, i, in, {}, K::Int); break;
            case Op::ConstFloat: expect(b, i, in, {}, K::Float); break;
            case Op::ConstBool: expect(b, i, in, {}, K::Bool); break;
            case Op::ConstString:
                expect(b, i, in, {}, K::Ref);
                in_range(b, i, in.imm, m_.strings.size(), "string");
                break;
            case Op::ConstUnit: expect(b, i, in, {}, K::Unit); break;
            case Op::LoadLocal:
                if (in_range(b, i, in.imm, f.locals.size(), "local"))
                    expect(b, i, in, {}, f.locals[static_cast<size_t>(in.imm)].kind);
                break;
            case Op::StoreLocal:
                no_dst(b, i, in);
                if (in_range(b, i, in.imm, f.locals.size(), "local"))
                    expect(b, i, in, {f.locals[static_cast<size_t>(in.imm)].kind}, K::Any);
                break;
            case Op::ClearLocal:
            case Op::CloseUpvalues:
                no_dst(b, i, in);
                expect(b, i, in, {}, K::Any);
                in_range(b, i, in.imm, f.locals.size(), "local");
                break;
            case Op::LoadUpval:
                if (in_range(b, i, in.imm, f.upvalue_kinds.size(), "upvalue"))
                    expect(b, i, in, {}, f.upvalue_kinds[static_cast<size_t>(in.imm)]);
                break;
            case Op::StoreUpval:
                no_dst(b, i, in);
                if (in_range(b, i, in.imm, f.upvalue_kinds.size(), "upvalue"))
                    expect(b, i, in, {f.upvalue_kinds[static_cast<size_t>(in.imm)]}, K::Any);
                break;
            case Op::LoadGlobal:
                if (in_range(b, i, in.imm, m_.globals.size(), "global"))
                    expect(b, i, in, {}, m_.globals[static_cast<size_t>(in.imm)].kind);
                break;
            case Op::StoreGlobal:
                no_dst(b, i, in);
                if (in_range(b, i, in.imm, m_.globals.size(), "global"))
                    expect(b, i, in, {m_.globals[static_cast<size_t>(in.imm)].kind}, K::Any);
                break;
            case Op::IAdd:
            case Op::ISub:
            case Op::IMul:
            case Op::IDiv:
            case Op::IMod: expect(b, i, in, {K::Int, K::Int}, K::Int); break;
            case Op::INeg: expect(b, i, in, {K::Int}, K::Int); break;
            case Op::FAdd:
            case Op::FSub:
            case Op::FMul:
            case Op::FDiv: expect(b, i, in, {K::Float, K::Float}, K::Float); break;
            case Op::FNeg: expect(b, i, in, {K::Float}, K::Float); break;
            case Op::Not: expect(b, i, in, {K::Bool}, K::Bool); break;
            case Op::I2F: expect(b, i, in, {K::Int}, K::Float); break;
            case Op::F2I: expect(b, i, in, {K::Float}, K::Int); break;
            case Op::ICmp:
            case Op::FCmp:
            case Op::BCmp:
            case Op::SCmp: {
                K k = in.op == Op::ICmp ? K::Int : in.op == Op::FCmp ? K::Float : in.op == Op::BCmp ? K::Bool : K::Ref;
                expect(b, i, in, {k, k}, K::Bool);
                if (in.imm < 0 || in.imm > static_cast<int64_t>(CmpOp::Ge)) err(at(b, i) + "bad comparison");
                if (in.op == Op::BCmp && in.imm != static_cast<int64_t>(CmpOp::Eq) && in.imm != static_cast<int64_t>(CmpOp::Ne))
                    err(at(b, i) + "Bool only supports eq/ne");
                break;
            }
            case Op::SConcat: expect(b, i, in, {K::Ref, K::Ref}, K::Ref); break;
            case Op::SLen: expect(b, i, in, {K::Ref}, K::Int); break;
            case Op::SIndex: expect(b, i, in, {K::Ref, K::Int}, K::Ref); break;
            case Op::Call: {
                if (!in_range(b, i, in.imm, m_.functions.size(), "function")) break;
                const Function& callee = m_.functions[static_cast<size_t>(in.imm)];
                if (!callee.captures.empty()) err(at(b, i) + "direct call to a function with upvalues");
                expect(b, i, in, callee.param_kinds, callee.ret_kind);
                break;
            }
            case Op::CallIndirect: {
                if (!in_range(b, i, in.imm, m_.signatures.size(), "signature")) break;
                const Signature& s = m_.signatures[static_cast<size_t>(in.imm)];
                std::vector<K> args{K::Ref};
                args.insert(args.end(), s.params.begin(), s.params.end());
                expect(b, i, in, args, s.ret);
                break;
            }
            case Op::CallNative: {
                if (!in_range(b, i, in.imm, natives().size(), "native")) break;
                const NativeDesc& d = natives()[static_cast<size_t>(in.imm)];
                expect(b, i, in, d.params, d.ret);
                break;
            }
            case Op::MakeClosure: {
                if (!in_range(b, i, in.imm, m_.functions.size(), "function")) break;
                expect(b, i, in, {}, K::Ref);
                const Function& child = m_.functions[static_cast<size_t>(in.imm)];
                for (size_t c = 0; c < child.captures.size() && c < child.upvalue_kinds.size(); ++c) {
                    const CaptureDesc& cd = child.captures[c];
                    K want = child.upvalue_kinds[c];
                    if (cd.from_local) {
                        if (cd.index >= f.locals.size()) err(at(b, i) + "capture of missing local");
                        else if (f.locals[cd.index].kind != want) err(at(b, i) + "capture kind mismatch");
                    } else {
                        if (cd.index >= f.upvalue_kinds.size()) err(at(b, i) + "capture of missing upvalue");
                        else if (f.upvalue_kinds[cd.index] != want) err(at(b, i) + "capture kind mismatch");
                    }
                }
                break;
            }
            case Op::MakeStruct:
                if (in_range(b, i, in.imm, m_.structs.size(), "struct"))
                    expect(b, i, in, m_.structs[static_cast<size_t>(in.imm)].field_kinds, K::Ref);
                break;
            case Op::GetField:
            case Op::SetField: {
                if (!in_range(b, i, in.imm, m_.structs.size(), "struct")) break;
                const auto& s = m_.structs[static_cast<size_t>(in.imm)];
                if (!in_range(b, i, in.imm2, s.field_kinds.size(), "field")) break;
                K fk = s.field_kinds[static_cast<size_t>(in.imm2)];
                if (in.op == Op::GetField) {
                    expect(b, i, in, {K::Ref}, fk);
                } else {
                    no_dst(b, i, in);
                    expect(b, i, in, {K::Ref, fk}, K::Any);
                }
                break;
            }
            case Op::MakeArray: {
                std::vector<K> args(in.args.size(), in.elem);
                expect(b, i, in, args, K::Ref);
                break;
            }
            case Op::ArrayRepeat: expect(b, i, in, {in.elem, K::Int}, K::Ref); break;
            case Op::ArrayGet: expect(b, i, in, {K::Ref, K::Int}, in.elem); break;
            case Op::ArraySet:
                no_dst(b, i, in);
                expect(b, i, in, {K::Ref, K::Int, in.elem}, K::Any);
                break;
            case Op::ArrayLen: expect(b, i, in, {K::Ref}, K::Int); break;
            case Op::ArrayPush: expect(b, i, in, {K::Ref, in.elem}, K::Unit); break;
            case Op::ArrayPop: expect(b, i, in, {K::Ref}, in.elem); break;
            case Op::MakeEnum: {
                if (!in_range(b, i, in.imm, m_.enums.size(), "enum")) break;
                const auto& e = m_.enums[static_cast<size_t>(in.imm)];
                if (!in_range(b, i, in.imm2, e.variants.size(), "variant")) break;
                expect(b, i, in, e.variants[static_cast<size_t>(in.imm2)].field_kinds, K::Ref);
                break;
            }
            case Op::EnumTag:
                in_range(b, i, in.imm, m_.enums.size(), "enum");
                expect(b, i, in, {K::Ref}, K::Int);
                break;
            case Op::EnumField: {
                if (!in_range(b, i, in.imm, m_.enums.size(), "enum")) break;
                const auto& e = m_.enums[static_cast<size_t>(in.imm)];
                if (!in_range(b, i, in.imm2, e.variants.size(), "variant")) break;
                const auto& v = e.variants[static_cast<size_t>(in.imm2)];
                if (!in_range(b, i, in.imm3, v.field_kinds.size(), "field")) break;
                expect(b, i, in, {K::Ref}, v.field_kinds[static_cast<size_t>(in.imm3)]);
                break;
            }
            case Op::Jump:
                no_dst(b, i, in);
                expect(b, i, in, {}, K::Any);
                check_target(b, i, in.target);
                break;
            case Op::Branch:
                no_dst(b, i, in);
                expect(b, i, in, {K::Bool}, K::Any);
                check_target(b, i, in.target);
                check_target(b, i, in.target2);
                break;
            case Op::Return:
                no_dst(b, i, in);
                expect(b, i, in, {f.ret_kind}, K::Any);
                break;
            case Op::Unreachable:
                no_dst(b, i, in);
                expect(b, i, in, {}, K::Any);
                break;
        }
    }

    const Module& m_;
    std::vector<std::string>& errs_;
    const Function* f_ = nullptr;
    std::string fname_;
};

}  // namespace

std::vector<std::string> validate(const Module& m) {
    std::vector<std::string> errs;
    if (m.init_function >= m.functions.size()) errs.push_back("init function index out of range");
    if (m.main_function >= m.functions.size()) errs.push_back("main function index out of range");
    Validator v(m, errs);
    for (size_t i = 0; i < m.functions.size(); ++i) v.function(m.functions[i], i);
    return errs;
}

}  // namespace ember::ir

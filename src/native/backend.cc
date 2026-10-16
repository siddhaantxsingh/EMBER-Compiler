#include "native/backend.h"

#include <chrono>
#include <mutex>
#include <unordered_map>

#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>

#include "common/natives.h"

namespace ember::native {

using ir::Op;

namespace {

enum : uint16_t {
    kAbs = kFirstModuleNative,
    kFabs,
    kMin,
    kMax,
    kSqrt,
    kPow,
    kFloor,
    kCeil,
    kSubstr,
    kFind,
    kUpper,
    kLower,
    kSplit,
    kJoin,
    kCharCode,
    kFromCharCode,
    kRepeat,
    kClock,
};

std::string where(const ir::Function& f) { return " in function '" + f.name + "'"; }

}  // namespace

// ------------------------------------------------------------------ support

Support check_support(const ir::Module& m) {
    Support s;
    auto reject = [&](std::string why) {
        if (s.ok) {
            s.ok = false;
            s.reason = std::move(why);
        }
    };
    if (natives().size() != kClock + 1u) reject("native table changed; backend needs updating");
    for (const auto& f : m.functions) {
        for (const auto& b : f.blocks)
            for (const auto& in : b.instrs) {
                switch (in.op) {
                    case Op::CallNative:
                        if (in.imm < 0 || in.imm > kClock) reject("unknown native" + where(f));
                        break;
                    default: break;
                }
            }
    }
    // Every value of kind Ref is a string, array, struct, enum or closure,
    // told apart by a header tag.
    return s;
}

// ------------------------------------------------------------------ lowering

namespace {

void init_llvm_once() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
    });
}

int64_t pack_loc(const Span& s) { return static_cast<int64_t>((static_cast<uint64_t>(s.line) << 32) | s.column); }

class Lowerer {
public:
    Lowerer(const ir::Module& m, llvm::LLVMContext& ctx, llvm::Module& mod) : m_(m), ctx_(ctx), mod_(mod), b_(ctx) {
        i1_ = llvm::Type::getInt1Ty(ctx);
        i8_ = llvm::Type::getInt8Ty(ctx);
        i32_ = llvm::Type::getInt32Ty(ctx);
        i64_ = llvm::Type::getInt64Ty(ctx);
        f64_ = llvm::Type::getDoubleTy(ctx);
        ptr_ = llvm::PointerType::get(ctx, 0);
        void_ = llvm::Type::getVoidTy(ctx);
    }

    void run() {
        declare_runtime();
        declare_globals();
        for (size_t i = 0; i < m_.functions.size(); ++i) declare_function(i);
        for (size_t i = 0; i < m_.functions.size(); ++i) lower_function(i);
        // Exported entry points.
        make_entry("ember_init", m_.init_function);
        make_entry("ember_main", m_.main_function);
    }

private:
    llvm::Type* ty(Kind k) const {
        switch (k) {
            case Kind::Unit: return i8_;
            case Kind::Bool: return i1_;
            case Kind::Int: return i64_;
            case Kind::Float: return f64_;
            case Kind::Ref: return ptr_;
            case Kind::Any: break;
        }
        return i8_;
    }

    llvm::Constant* zero(Kind k) {
        switch (k) {
            case Kind::Unit: return llvm::ConstantInt::get(i8_, 0);
            case Kind::Bool: return llvm::ConstantInt::getFalse(ctx_);
            case Kind::Int: return llvm::ConstantInt::get(i64_, 0);
            case Kind::Float: return llvm::ConstantFP::get(f64_, 0.0);
            case Kind::Ref: return string_const_raw("");
            case Kind::Any: break;
        }
        return llvm::ConstantInt::get(i8_, 0);
    }

    // ---- runtime declarations
    llvm::Function* rt(const char* name, llvm::Type* ret, std::vector<llvm::Type*> params, bool noreturn = false) {
        auto* fty = llvm::FunctionType::get(ret, params, false);
        auto* fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, name, mod_);
        if (noreturn) {
            fn->addFnAttr(llvm::Attribute::NoReturn);
            fn->addFnAttr(llvm::Attribute::Cold);
        }
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        rt_[name] = fn;
        return fn;
    }

    void declare_runtime() {
        auto ext = [&](const char* name, llvm::Type* t) {
            return new llvm::GlobalVariable(mod_, t, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
        };
        g_depth_ = ext("ember_rt_depth", i64_);
        g_max_depth_ = ext("ember_rt_max_depth", i64_);
        g_frame_fn_ = ext("ember_rt_frame_fn", ptr_);
        g_frame_loc_ = ext("ember_rt_frame_loc", ptr_);
        rt("ember_rt_trap", void_, {i32_, i64_}, true);
        rt("ember_rt_trap_f2i", void_, {f64_}, true);
        rt("ember_rt_print_int", void_, {i64_});
        rt("ember_rt_print_float", void_, {f64_});
        rt("ember_rt_print_bool", void_, {i32_});
        rt("ember_rt_print_unit", void_, {});
        rt("ember_rt_print_str", void_, {ptr_});
        rt("ember_rt_str_int", ptr_, {i64_});
        rt("ember_rt_str_float", ptr_, {f64_});
        rt("ember_rt_str_bool", ptr_, {i32_});
        rt("ember_rt_str_unit", ptr_, {});
        rt("ember_rt_concat", ptr_, {ptr_, ptr_});
        rt("ember_rt_sindex", ptr_, {ptr_, i64_});
        rt("ember_rt_scmp", i32_, {ptr_, ptr_});
        rt("ember_rt_input", ptr_, {});
        rt("ember_rt_parse_int", i64_, {ptr_});
        rt("ember_rt_panic", void_, {ptr_}, true);
        rt("ember_rt_assert", void_, {i32_, ptr_});
        rt("ember_rt_pow", f64_, {f64_, f64_});
        rt("ember_rt_clock", f64_, {});
        rt("ember_rt_substr", ptr_, {ptr_, i64_, i64_});
        rt("ember_rt_find", i64_, {ptr_, ptr_});
        rt("ember_rt_upper", ptr_, {ptr_});
        rt("ember_rt_lower", ptr_, {ptr_});
        rt("ember_rt_char_code", i64_, {ptr_, i64_});
        rt("ember_rt_from_char_code", ptr_, {i64_});
        rt("ember_rt_repeat", ptr_, {ptr_, i64_});
        rt("ember_rt_print_ref", void_, {ptr_});
        rt("ember_rt_str_ref", ptr_, {ptr_});
        rt("ember_rt_array_new", ptr_, {i64_, i64_});
        rt("ember_rt_array_repeat", ptr_, {i64_, i64_, i64_});
        rt("ember_rt_index_error", void_, {i64_, i64_}, true);
        rt("ember_rt_array_push", void_, {ptr_, i64_});
        rt("ember_rt_array_pop", i64_, {ptr_});
        rt("ember_rt_split", ptr_, {ptr_, ptr_});
        rt("ember_rt_join", ptr_, {ptr_, ptr_});
        rt("ember_rt_alloc_obj", ptr_, {i64_, i64_});
        rt("ember_rt_alloc_cell", ptr_, {i64_, i64_});
        rt("ember_rt_trap_uninit", void_, {ptr_}, true);
        // Pure runtime helpers LLVM may treat as readnone.
        for (const char* n : {"ember_rt_scmp", "ember_rt_find"}) rt_[n]->setOnlyReadsMemory();
    }

    // ---- globals and constants
    void declare_globals() {
        for (size_t i = 0; i < m_.globals.size(); ++i) {
            const auto& gi = m_.globals[i];
            auto* gv = new llvm::GlobalVariable(mod_, ty(gi.kind), false, llvm::GlobalValue::InternalLinkage, zero(gi.kind),
                                                "g." + gi.name);
            auto* flag = new llvm::GlobalVariable(mod_, i1_, false, llvm::GlobalValue::InternalLinkage,
                                                  llvm::ConstantInt::getFalse(ctx_), "g." + gi.name + ".init");
            globals_.push_back(gv);
            global_init_.push_back(flag);
        }
    }

    llvm::Constant* string_const_raw(const std::string& s) {
        auto it = strconst_.find(s);
        if (it != strconst_.end()) return it->second;
        auto* data = llvm::ConstantDataArray::getString(ctx_, s, true);
        auto* sty = llvm::StructType::get(ctx_, {i64_, i64_, data->getType()}, true);
        auto* init = llvm::ConstantStruct::get(
            sty, {llvm::ConstantInt::get(i64_, kTagString), llvm::ConstantInt::get(i64_, static_cast<int64_t>(s.size())), data});
        auto* gv = new llvm::GlobalVariable(mod_, sty, true, llvm::GlobalValue::PrivateLinkage, init, "str");
        gv->setAlignment(llvm::Align(8));
        gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        strconst_[s] = gv;
        return gv;
    }

    // ---- functions
    void declare_function(size_t i) {
        const auto& f = m_.functions[i];
        std::vector<llvm::Type*> params{ptr_};  // hidden first parameter: the closure (or null)
        for (Kind k : f.param_kinds) params.push_back(ty(k));
        auto* fty = llvm::FunctionType::get(ty(f.ret_kind), params, false);
        auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage, "em." + std::to_string(i) + "." + f.name, mod_);
        fn->addFnAttr(llvm::Attribute::NoUnwind);
        fns_.push_back(fn);
    }

    void make_entry(const char* name, uint32_t idx) {
        auto* fty = llvm::FunctionType::get(i8_, {}, false);
        auto* fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, name, mod_);
        auto* bb = llvm::BasicBlock::Create(ctx_, "entry", fn);
        b_.SetInsertPoint(bb);
        llvm::Value* r = b_.CreateCall(fns_[idx], {llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))});
        if (r->getType() != i8_) r = llvm::ConstantInt::get(i8_, 0);
        b_.CreateRet(r);
    }

    // Per-function state.
    const ir::Function* f_ = nullptr;
    llvm::Function* fn_ = nullptr;
    std::vector<llvm::AllocaInst*> locals_;
    std::vector<llvm::Value*> temps_;
    std::vector<llvm::BasicBlock*> blocks_;
    llvm::Value* depth_in_ = nullptr;   // ember_rt_depth on entry
    llvm::Value* loc_slot_ = nullptr;   // &ember_rt_frame_loc[depth+1]
    Span last_loc_;
    std::vector<bool> captured_;
    std::vector<llvm::AllocaInst*> cells_;

    llvm::Value* cell_field(llvm::Value* cell, uint64_t i) { return b_.CreateConstGEP1_64(i64_, cell, i); }
    llvm::Value* upval_cell(int64_t idx) {
        return b_.CreateLoad(ptr_, b_.CreateConstGEP1_64(i64_, fn_->getArg(0), 2 + static_cast<uint64_t>(idx)));
    }

    void store_loc(const Span& s) {
        if (s.valid()) last_loc_ = s;
        b_.CreateStore(llvm::ConstantInt::get(i64_, pack_loc(last_loc_)), loc_slot_);
    }

    // Emit "if (cond) trap(kind)" and continue in a fresh block.
    void trap_if(llvm::Value* cond, int32_t kind, const Span& loc, int64_t arg = 0) {
        auto* trap_bb = llvm::BasicBlock::Create(ctx_, "trap", fn_);
        auto* cont = llvm::BasicBlock::Create(ctx_, "cont", fn_);
        b_.CreateCondBr(cond, trap_bb, cont);
        b_.SetInsertPoint(trap_bb);
        store_loc(loc);
        b_.CreateCall(rt_["ember_rt_trap"], {llvm::ConstantInt::get(i32_, kind), llvm::ConstantInt::get(i64_, arg)});
        b_.CreateUnreachable();
        b_.SetInsertPoint(cont);
    }

    std::vector<ir::BlockId> rpo() const {
        size_t n = f_->blocks.size();
        std::vector<int> state(n, 0);
        std::vector<ir::BlockId> post;
        // Iterative DFS.
        std::vector<std::pair<ir::BlockId, int>> stack;
        stack.push_back({0, 0});
        state[0] = 1;
        while (!stack.empty()) {
            auto& [b, k] = stack.back();
            const auto& term = f_->blocks[static_cast<size_t>(b)].instrs.back();
            std::vector<ir::BlockId> succ;
            if (term.op == Op::Jump) succ = {term.target};
            else if (term.op == Op::Branch) succ = {term.target, term.target2};
            if (k < static_cast<int>(succ.size())) {
                ir::BlockId s = succ[static_cast<size_t>(k)];
                ++k;
                if (state[static_cast<size_t>(s)] == 0) {
                    state[static_cast<size_t>(s)] = 1;
                    stack.push_back({s, 0});
                }
            } else {
                post.push_back(b);
                stack.pop_back();
            }
        }
        return std::vector<ir::BlockId>(post.rbegin(), post.rend());
    }

    llvm::Value* t(ir::TempId id) const { return temps_[static_cast<size_t>(id)]; }

    void lower_function(size_t idx) {
        f_ = &m_.functions[idx];
        fn_ = fns_[idx];
        last_loc_ = f_->loc;
        temps_.assign(f_->temp_kinds.size(), nullptr);
        blocks_.assign(f_->blocks.size(), nullptr);
        locals_.clear();

        auto* entry = llvm::BasicBlock::Create(ctx_, "entry", fn_);
        b_.SetInsertPoint(entry);
        for (size_t i = 0; i < f_->locals.size(); ++i) {
            auto* a = b_.CreateAlloca(ty(f_->locals[i].kind), nullptr, f_->locals[i].name.empty() ? "tmp" : f_->locals[i].name);
            locals_.push_back(a);
        }
        for (size_t i = 0; i < f_->locals.size(); ++i) {
            if (i < f_->num_params) b_.CreateStore(fn_->getArg(static_cast<unsigned>(i) + 1), locals_[i]);
            else b_.CreateStore(zero(f_->locals[i].kind), locals_[i]);
        }
        // Captured locals live in heap cells { i64 bits, i64 initialized };
        // see the comment on CloseUpvalues below.
        captured_.assign(f_->locals.size(), false);
        cells_.assign(f_->locals.size(), nullptr);
        for (const auto& blk : f_->blocks)
            for (const auto& in : blk.instrs)
                if (in.op == Op::MakeClosure)
                    for (const auto& cd : m_.functions[static_cast<size_t>(in.imm)].captures)
                        if (cd.from_local) captured_[cd.index] = true;
        // Call-depth check and frame bookkeeping (VM: push_frame).
        depth_in_ = b_.CreateLoad(i64_, g_depth_, "depth");
        auto* maxd = b_.CreateLoad(i64_, g_max_depth_, "maxdepth");
        {
            auto* trap_bb = llvm::BasicBlock::Create(ctx_, "overflow", fn_);
            auto* cont = llvm::BasicBlock::Create(ctx_, "body", fn_);
            b_.CreateCondBr(b_.CreateICmpUGE(depth_in_, maxd), trap_bb, cont);
            b_.SetInsertPoint(trap_bb);
            // The frame is not pushed: the caller's call site is the innermost frame.
            b_.CreateCall(rt_["ember_rt_trap"], {llvm::ConstantInt::get(i32_, kTrapStackOverflow), llvm::ConstantInt::get(i64_, 0)});
            b_.CreateUnreachable();
            b_.SetInsertPoint(cont);
        }
        auto* d1 = b_.CreateAdd(depth_in_, llvm::ConstantInt::get(i64_, 1), "d1");
        b_.CreateStore(d1, g_depth_);
        auto* fnarr = b_.CreateLoad(ptr_, g_frame_fn_);
        b_.CreateStore(llvm::ConstantInt::get(i32_, static_cast<int64_t>(idx)), b_.CreateGEP(i32_, fnarr, d1));
        auto* locarr = b_.CreateLoad(ptr_, g_frame_loc_);
        loc_slot_ = b_.CreateGEP(i64_, locarr, d1, "locslot");
        b_.CreateStore(llvm::ConstantInt::get(i64_, pack_loc(f_->loc)), loc_slot_);
        for (size_t i = 0; i < f_->locals.size(); ++i) {
            if (!captured_[i]) continue;
            cells_[i] = b_.CreateAlloca(ptr_, nullptr, "cell");
            Kind k = f_->locals[i].kind;
            // Ref locals start uninitialized in the VM; others get a default.
            llvm::Value* init = llvm::ConstantInt::get(i64_, i < f_->num_params || k != Kind::Ref ? 1 : 0);
            llvm::Value* v = to_bits(b_.CreateLoad(ty(k), locals_[i]), k);
            b_.CreateStore(b_.CreateCall(rt_["ember_rt_alloc_cell"], {v, init}), cells_[i]);
        }

        auto order = rpo();
        for (ir::BlockId bid : order) blocks_[static_cast<size_t>(bid)] = llvm::BasicBlock::Create(ctx_, "b" + std::to_string(bid), fn_);
        b_.CreateBr(blocks_[0]);
        for (ir::BlockId bid : order) {
            b_.SetInsertPoint(blocks_[static_cast<size_t>(bid)]);
            for (const auto& in : f_->blocks[static_cast<size_t>(bid)].instrs) lower_instr(in);
        }
    }

    llvm::Value* intrinsic(llvm::Intrinsic::ID id, llvm::Type* t, std::vector<llvm::Value*> args) {
        auto* f = llvm::Intrinsic::getDeclaration(&mod_, id, {t});
        return b_.CreateCall(f, args);
    }

    llvm::Value* checked_arith(llvm::Intrinsic::ID id, llvm::Value* a, llvm::Value* bv, const Span& loc) {
        auto* r = intrinsic(id, i64_, {a, bv});
        trap_if(b_.CreateExtractValue(r, 1), kTrapOverflow, loc);
        return b_.CreateExtractValue(r, 0);
    }

    llvm::Value* call_rt(const char* name, std::vector<llvm::Value*> args, const Span& loc, bool may_fail) {
        if (may_fail) store_loc(loc);
        return b_.CreateCall(rt_.at(name), args);
    }

    llvm::Value* unit() { return llvm::ConstantInt::get(i8_, 0); }

    // Array element slots are 8 bytes (see runtime.h).
    llvm::Value* to_bits(llvm::Value* v, Kind k) {
        switch (k) {
            case Kind::Int: return v;
            case Kind::Float: return b_.CreateBitCast(v, i64_);
            case Kind::Bool:
            case Kind::Unit: return b_.CreateZExt(v, i64_);
            default: return b_.CreatePtrToInt(v, i64_);
        }
    }
    llvm::Value* from_bits(llvm::Value* v, Kind k) {
        switch (k) {
            case Kind::Int: return v;
            case Kind::Float: return b_.CreateBitCast(v, f64_);
            case Kind::Bool: return b_.CreateICmpNE(v, llvm::ConstantInt::get(i64_, 0));
            case Kind::Unit: return unit();
            default: return b_.CreateIntToPtr(v, ptr_);
        }
    }

    void def(const ir::Instr& in, llvm::Value* v) {
        if (in.dst != ir::kNoTemp) temps_[static_cast<size_t>(in.dst)] = v;
    }

    llvm::Value* cmp(const ir::Instr& in) {
        auto c = static_cast<ir::CmpOp>(in.imm);
        llvm::Value* a = t(in.args[0]);
        llvm::Value* bv = t(in.args[1]);
        using P = llvm::CmpInst::Predicate;
        if (in.op == Op::ICmp || in.op == Op::BCmp) {
            P p = P::ICMP_EQ;
            switch (c) {
                case ir::CmpOp::Eq: p = P::ICMP_EQ; break;
                case ir::CmpOp::Ne: p = P::ICMP_NE; break;
                case ir::CmpOp::Lt: p = P::ICMP_SLT; break;
                case ir::CmpOp::Le: p = P::ICMP_SLE; break;
                case ir::CmpOp::Gt: p = P::ICMP_SGT; break;
                case ir::CmpOp::Ge: p = P::ICMP_SGE; break;
            }
            return b_.CreateICmp(p, a, bv);
        }
        if (in.op == Op::FCmp) {
            // Same as C++ operators on doubles: only != is true for NaN.
            P p = P::FCMP_OEQ;
            switch (c) {
                case ir::CmpOp::Eq: p = P::FCMP_OEQ; break;
                case ir::CmpOp::Ne: p = P::FCMP_UNE; break;
                case ir::CmpOp::Lt: p = P::FCMP_OLT; break;
                case ir::CmpOp::Le: p = P::FCMP_OLE; break;
                case ir::CmpOp::Gt: p = P::FCMP_OGT; break;
                case ir::CmpOp::Ge: p = P::FCMP_OGE; break;
            }
            return b_.CreateFCmp(p, a, bv);
        }
        // SCmp: three-way compare in the runtime.
        llvm::Value* r = b_.CreateCall(rt_["ember_rt_scmp"], {a, bv});
        llvm::Value* z = llvm::ConstantInt::get(i32_, 0);
        switch (c) {
            case ir::CmpOp::Eq: return b_.CreateICmpEQ(r, z);
            case ir::CmpOp::Ne: return b_.CreateICmpNE(r, z);
            case ir::CmpOp::Lt: return b_.CreateICmpSLT(r, z);
            case ir::CmpOp::Le: return b_.CreateICmpSLE(r, z);
            case ir::CmpOp::Gt: return b_.CreateICmpSGT(r, z);
            case ir::CmpOp::Ge: return b_.CreateICmpSGE(r, z);
        }
        return r;
    }

    Kind kind_of(ir::TempId id) const { return f_->temp_kinds[static_cast<size_t>(id)]; }

    llvm::Value* native(const ir::Instr& in) {
        auto id = static_cast<uint16_t>(in.imm);
        auto arg = [&](size_t i) { return t(in.args[i]); };
        switch (id) {
            case kNativePrint: {
                Kind k = kind_of(in.args[0]);
                switch (k) {
                    case Kind::Int: b_.CreateCall(rt_["ember_rt_print_int"], {arg(0)}); break;
                    case Kind::Float: b_.CreateCall(rt_["ember_rt_print_float"], {arg(0)}); break;
                    case Kind::Bool: b_.CreateCall(rt_["ember_rt_print_bool"], {b_.CreateZExt(arg(0), i32_)}); break;
                    case Kind::Unit: b_.CreateCall(rt_["ember_rt_print_unit"], {}); break;
                    default: call_rt("ember_rt_print_ref", {arg(0)}, in.loc, true); break;
                }
                return unit();
            }
            case kNativeStr: {
                Kind k = kind_of(in.args[0]);
                switch (k) {
                    case Kind::Int: return call_rt("ember_rt_str_int", {arg(0)}, in.loc, true);
                    case Kind::Float: return call_rt("ember_rt_str_float", {arg(0)}, in.loc, true);
                    case Kind::Bool: return call_rt("ember_rt_str_bool", {b_.CreateZExt(arg(0), i32_)}, in.loc, true);
                    case Kind::Unit: return call_rt("ember_rt_str_unit", {}, in.loc, true);
                    default: return call_rt("ember_rt_str_ref", {arg(0)}, in.loc, true);  // str(s) is s for strings
                }
            }
            case kNativeInput: return call_rt("ember_rt_input", {}, in.loc, true);
            case kNativeParseInt: return call_rt("ember_rt_parse_int", {arg(0)}, in.loc, true);
            case kNativePanic: call_rt("ember_rt_panic", {arg(0)}, in.loc, true); return unit();
            case kNativeAssert: call_rt("ember_rt_assert", {b_.CreateZExt(arg(0), i32_), arg(1)}, in.loc, true); return unit();
            case kAbs: {
                auto* x = arg(0);
                trap_if(b_.CreateICmpEQ(x, llvm::ConstantInt::get(i64_, INT64_MIN)), kTrapOverflow, in.loc);
                return b_.CreateSelect(b_.CreateICmpSLT(x, llvm::ConstantInt::get(i64_, 0)), b_.CreateNeg(x), x);
            }
            case kFabs: return intrinsic(llvm::Intrinsic::fabs, f64_, {arg(0)});
            case kMin: return b_.CreateSelect(b_.CreateICmpSLT(arg(0), arg(1)), arg(0), arg(1));
            case kMax: return b_.CreateSelect(b_.CreateICmpSLT(arg(0), arg(1)), arg(1), arg(0));
            case kSqrt: return intrinsic(llvm::Intrinsic::sqrt, f64_, {arg(0)});
            case kPow: return b_.CreateCall(rt_["ember_rt_pow"], {arg(0), arg(1)});
            case kFloor: return intrinsic(llvm::Intrinsic::floor, f64_, {arg(0)});
            case kCeil: return intrinsic(llvm::Intrinsic::ceil, f64_, {arg(0)});
            case kSubstr: return call_rt("ember_rt_substr", {arg(0), arg(1), arg(2)}, in.loc, true);
            case kFind: return b_.CreateCall(rt_["ember_rt_find"], {arg(0), arg(1)});
            case kUpper: return call_rt("ember_rt_upper", {arg(0)}, in.loc, true);
            case kLower: return call_rt("ember_rt_lower", {arg(0)}, in.loc, true);
            case kCharCode: return call_rt("ember_rt_char_code", {arg(0), arg(1)}, in.loc, true);
            case kFromCharCode: return call_rt("ember_rt_from_char_code", {arg(0)}, in.loc, true);
            case kRepeat: return call_rt("ember_rt_repeat", {arg(0), arg(1)}, in.loc, true);
            case kClock: return b_.CreateCall(rt_["ember_rt_clock"], {});
            case kSplit: return call_rt("ember_rt_split", {arg(0), arg(1)}, in.loc, true);
            case kJoin: return call_rt("ember_rt_join", {arg(0), arg(1)}, in.loc, true);
            default: break;
        }
        throw std::logic_error("unsupported native reached the backend");
    }

    void lower_instr(const ir::Instr& in) {
        auto a = [&](size_t i) { return t(in.args[i]); };
        auto I = [&](int64_t v) { return llvm::ConstantInt::get(i64_, v); };
        switch (in.op) {
            case Op::ConstInt: def(in, I(in.imm)); break;
            case Op::ConstFloat: def(in, llvm::ConstantFP::get(f64_, in.fimm)); break;
            case Op::ConstBool: def(in, llvm::ConstantInt::get(i1_, in.imm != 0)); break;
            case Op::ConstString: def(in, string_const_raw(m_.strings[static_cast<size_t>(in.imm)])); break;
            case Op::ConstUnit: def(in, unit()); break;
            case Op::LoadLocal: {
                auto i = static_cast<size_t>(in.imm);
                auto* slot = locals_[i];
                if (captured_[i]) {
                    auto* cell = b_.CreateLoad(ptr_, cells_[i]);
                    def(in, from_bits(b_.CreateLoad(i64_, cell_field(cell, 0)), f_->locals[i].kind));
                } else {
                    def(in, b_.CreateLoad(slot->getAllocatedType(), slot));
                }
                break;
            }
            case Op::StoreLocal: {
                auto i = static_cast<size_t>(in.imm);
                if (captured_[i]) {
                    auto* cell = b_.CreateLoad(ptr_, cells_[i]);
                    b_.CreateStore(to_bits(a(0), f_->locals[i].kind), cell_field(cell, 0));
                    b_.CreateStore(I(1), cell_field(cell, 1));
                } else {
                    b_.CreateStore(a(0), locals_[i]);
                }
                break;
            }
            case Op::ClearLocal: {
                auto i = static_cast<size_t>(in.imm);
                if (captured_[i]) b_.CreateStore(I(0), cell_field(b_.CreateLoad(ptr_, cells_[i]), 1));
                break;
            }
            case Op::CloseUpvalues: {
                // VM: closures created so far keep the closed upvalue, and the
                // next capture of the slot opens a new one with the slot's
                // current value. Here: give each captured slot >= imm a fresh
                // cell holding the same value; existing closures keep the old.
                for (size_t i = static_cast<size_t>(in.imm); i < f_->locals.size(); ++i) {
                    if (!captured_[i]) continue;
                    auto* cell = b_.CreateLoad(ptr_, cells_[i]);
                    auto* v = b_.CreateLoad(i64_, cell_field(cell, 0));
                    auto* ok = b_.CreateLoad(i64_, cell_field(cell, 1));
                    b_.CreateStore(b_.CreateCall(rt_["ember_rt_alloc_cell"], {v, ok}), cells_[i]);
                }
                break;
            }
            case Op::LoadUpval: {
                auto* cell = upval_cell(in.imm);
                auto* ok = b_.CreateLoad(i64_, cell_field(cell, 1));
                auto* bad_bb = llvm::BasicBlock::Create(ctx_, "uninit", fn_);
                auto* cont = llvm::BasicBlock::Create(ctx_, "cont", fn_);
                b_.CreateCondBr(b_.CreateICmpEQ(ok, I(0)), bad_bb, cont);
                b_.SetInsertPoint(bad_bb);
                store_loc(in.loc);
                b_.CreateCall(rt_["ember_rt_trap_uninit"], {b_.CreateGlobalStringPtr(f_->upvalue_names[static_cast<size_t>(in.imm)])});
                b_.CreateUnreachable();
                b_.SetInsertPoint(cont);
                def(in, from_bits(b_.CreateLoad(i64_, cell_field(cell, 0)), f_->upvalue_kinds[static_cast<size_t>(in.imm)]));
                break;
            }
            case Op::StoreUpval: {
                auto* cell = upval_cell(in.imm);
                b_.CreateStore(to_bits(a(0), f_->upvalue_kinds[static_cast<size_t>(in.imm)]), cell_field(cell, 0));
                b_.CreateStore(I(1), cell_field(cell, 1));
                break;
            }
            case Op::MakeClosure: {
                const auto& child = m_.functions[static_cast<size_t>(in.imm)];
                store_loc(in.loc);
                auto* obj = b_.CreateCall(rt_["ember_rt_alloc_obj"],
                                          {I(kTagClosure | (in.imm << 8)), I(2 + static_cast<int64_t>(child.captures.size()))});
                b_.CreateStore(fns_[static_cast<size_t>(in.imm)], b_.CreateConstGEP1_64(i64_, obj, 1));
                for (size_t c = 0; c < child.captures.size(); ++c) {
                    const auto& cd = child.captures[c];
                    llvm::Value* cell = cd.from_local ? static_cast<llvm::Value*>(b_.CreateLoad(ptr_, cells_[cd.index]))
                                                      : upval_cell(cd.index);
                    b_.CreateStore(cell, b_.CreateConstGEP1_64(i64_, obj, 2 + c));
                }
                def(in, obj);
                break;
            }
            case Op::CallIndirect: {
                // Well-typed IR: the closure's function has this signature (the
                // VM re-checks it for hand-written bytecode; IR cannot differ).
                const auto& sig = m_.signatures[static_cast<size_t>(in.imm)];
                std::vector<llvm::Type*> pts{ptr_};
                for (Kind k : sig.params) pts.push_back(ty(k));
                auto* fty = llvm::FunctionType::get(ty(sig.ret), pts, false);
                auto* clo = a(0);
                auto* fp = b_.CreateLoad(ptr_, b_.CreateConstGEP1_64(i64_, clo, 1));
                std::vector<llvm::Value*> args{clo};
                for (size_t i = 1; i < in.args.size(); ++i) args.push_back(a(i));
                store_loc(in.loc);
                def(in, b_.CreateCall(fty, fp, args));
                break;
            }
            case Op::LoadGlobal: {
                auto g = static_cast<size_t>(in.imm);
                auto* ok = b_.CreateLoad(i1_, global_init_[g]);
                trap_if(b_.CreateNot(ok), kTrapUninitGlobal, in.loc, static_cast<int64_t>(g));
                def(in, b_.CreateLoad(globals_[g]->getValueType(), globals_[g]));
                break;
            }
            case Op::StoreGlobal: {
                auto g = static_cast<size_t>(in.imm);
                b_.CreateStore(a(0), globals_[g]);
                b_.CreateStore(llvm::ConstantInt::getTrue(ctx_), global_init_[g]);
                break;
            }
            case Op::IAdd: def(in, checked_arith(llvm::Intrinsic::sadd_with_overflow, a(0), a(1), in.loc)); break;
            case Op::ISub: def(in, checked_arith(llvm::Intrinsic::ssub_with_overflow, a(0), a(1), in.loc)); break;
            case Op::IMul: def(in, checked_arith(llvm::Intrinsic::smul_with_overflow, a(0), a(1), in.loc)); break;
            case Op::IDiv: {
                trap_if(b_.CreateICmpEQ(a(1), I(0)), kTrapDivZero, in.loc);
                trap_if(b_.CreateAnd(b_.CreateICmpEQ(a(0), I(INT64_MIN)), b_.CreateICmpEQ(a(1), I(-1))), kTrapOverflow, in.loc);
                def(in, b_.CreateSDiv(a(0), a(1)));
                break;
            }
            case Op::IMod: {
                trap_if(b_.CreateICmpEQ(a(1), I(0)), kTrapDivZero, in.loc);
                // VM: b == -1 gives 0 (avoids INT64_MIN % -1).
                auto* minus1 = b_.CreateICmpEQ(a(1), I(-1));
                auto* safe = b_.CreateSelect(minus1, I(1), a(1));
                def(in, b_.CreateSRem(a(0), safe));
                break;
            }
            case Op::INeg: {
                trap_if(b_.CreateICmpEQ(a(0), I(INT64_MIN)), kTrapOverflow, in.loc);
                def(in, b_.CreateNeg(a(0)));
                break;
            }
            case Op::FAdd: def(in, b_.CreateFAdd(a(0), a(1))); break;
            case Op::FSub: def(in, b_.CreateFSub(a(0), a(1))); break;
            case Op::FMul: def(in, b_.CreateFMul(a(0), a(1))); break;
            case Op::FDiv: def(in, b_.CreateFDiv(a(0), a(1))); break;
            case Op::FNeg: def(in, b_.CreateFNeg(a(0))); break;
            case Op::Not: def(in, b_.CreateNot(a(0))); break;
            case Op::I2F: def(in, b_.CreateSIToFP(a(0), f64_)); break;
            case Op::F2I: {
                auto* tr = intrinsic(llvm::Intrinsic::trunc, f64_, {a(0)});
                auto* lo = b_.CreateFCmpOGE(tr, llvm::ConstantFP::get(f64_, -9223372036854775808.0));
                auto* hi = b_.CreateFCmpOLT(tr, llvm::ConstantFP::get(f64_, 9223372036854775808.0));
                auto* bad_bb = llvm::BasicBlock::Create(ctx_, "badconv", fn_);
                auto* cont = llvm::BasicBlock::Create(ctx_, "cont", fn_);
                b_.CreateCondBr(b_.CreateAnd(lo, hi), cont, bad_bb);
                b_.SetInsertPoint(bad_bb);
                store_loc(in.loc);
                b_.CreateCall(rt_["ember_rt_trap_f2i"], {a(0)});
                b_.CreateUnreachable();
                b_.SetInsertPoint(cont);
                def(in, b_.CreateFPToSI(tr, i64_));
                break;
            }
            case Op::ICmp:
            case Op::FCmp:
            case Op::BCmp:
            case Op::SCmp: def(in, cmp(in)); break;
            case Op::SConcat: def(in, call_rt("ember_rt_concat", {a(0), a(1)}, in.loc, true)); break;
            case Op::SLen: def(in, b_.CreateLoad(i64_, b_.CreateConstGEP1_64(i64_, a(0), 1))); break;
            case Op::MakeArray: {
                store_loc(in.loc);
                auto* arr = b_.CreateCall(rt_["ember_rt_array_new"], {I(static_cast<int64_t>(in.elem)), I(static_cast<int64_t>(in.args.size()))});
                if (!in.args.empty()) {
                    auto* data = b_.CreateLoad(ptr_, b_.CreateConstGEP1_64(i64_, arr, 3));
                    for (size_t i = 0; i < in.args.size(); ++i)
                        b_.CreateStore(to_bits(a(i), in.elem), b_.CreateConstGEP1_64(i64_, data, i));
                }
                def(in, arr);
                break;
            }
            case Op::MakeStruct:
            case Op::MakeEnum: {
                bool st = in.op == Op::MakeStruct;
                int64_t hdr = st ? 1 : 2;
                int64_t tag = (st ? kTagStruct : kTagEnum) | (in.imm << 8);
                store_loc(in.loc);
                auto* obj = b_.CreateCall(rt_["ember_rt_alloc_obj"], {I(tag), I(hdr + static_cast<int64_t>(in.args.size()))});
                if (!st) b_.CreateStore(I(in.imm2), b_.CreateConstGEP1_64(i64_, obj, 1));
                const auto& kinds = st ? m_.structs[static_cast<size_t>(in.imm)].field_kinds
                                       : m_.enums[static_cast<size_t>(in.imm)].variants[static_cast<size_t>(in.imm2)].field_kinds;
                for (size_t i = 0; i < in.args.size(); ++i)
                    b_.CreateStore(to_bits(a(i), kinds[i]), b_.CreateConstGEP1_64(i64_, obj, static_cast<uint64_t>(hdr) + i));
                def(in, obj);
                break;
            }
            case Op::GetField:
            case Op::SetField: {
                Kind fk = m_.structs[static_cast<size_t>(in.imm)].field_kinds[static_cast<size_t>(in.imm2)];
                auto* slot = b_.CreateConstGEP1_64(i64_, a(0), 1 + static_cast<uint64_t>(in.imm2));
                if (in.op == Op::GetField) def(in, from_bits(b_.CreateLoad(i64_, slot), fk));
                else b_.CreateStore(to_bits(a(1), fk), slot);
                break;
            }
            case Op::EnumTag: def(in, b_.CreateLoad(i64_, b_.CreateConstGEP1_64(i64_, a(0), 1))); break;
            case Op::EnumField: {
                auto* v = b_.CreateLoad(i64_, b_.CreateConstGEP1_64(i64_, a(0), 1));
                trap_if(b_.CreateICmpNE(v, I(in.imm2)), kTrapWrongVariant, in.loc);
                Kind fk = m_.enums[static_cast<size_t>(in.imm)].variants[static_cast<size_t>(in.imm2)].field_kinds[static_cast<size_t>(in.imm3)];
                def(in, from_bits(b_.CreateLoad(i64_, b_.CreateConstGEP1_64(i64_, a(0), 2 + static_cast<uint64_t>(in.imm3))), fk));
                break;
            }
            case Op::ArrayRepeat:
                def(in, call_rt("ember_rt_array_repeat", {I(static_cast<int64_t>(in.elem)), to_bits(a(0), in.elem), a(1)}, in.loc, true));
                break;
            case Op::ArrayLen: def(in, b_.CreateLoad(i64_, b_.CreateConstGEP1_64(i64_, a(0), 1))); break;
            case Op::ArrayGet:
            case Op::ArraySet: {
                auto* arr = a(0);
                auto* idx = a(1);
                auto* len = b_.CreateLoad(i64_, b_.CreateConstGEP1_64(i64_, arr, 1));
                // Unsigned compare also catches negative indices.
                auto* bad_bb = llvm::BasicBlock::Create(ctx_, "oob", fn_);
                auto* cont = llvm::BasicBlock::Create(ctx_, "inb", fn_);
                b_.CreateCondBr(b_.CreateICmpUGE(idx, len), bad_bb, cont);
                b_.SetInsertPoint(bad_bb);
                store_loc(in.loc);
                b_.CreateCall(rt_["ember_rt_index_error"], {idx, len});
                b_.CreateUnreachable();
                b_.SetInsertPoint(cont);
                auto* data = b_.CreateLoad(ptr_, b_.CreateConstGEP1_64(i64_, arr, 3));
                auto* slot = b_.CreateGEP(i64_, data, idx);
                if (in.op == Op::ArrayGet) def(in, from_bits(b_.CreateLoad(i64_, slot), in.elem));
                else b_.CreateStore(to_bits(a(2), in.elem), slot);
                break;
            }
            case Op::ArrayPush:
                call_rt("ember_rt_array_push", {a(0), to_bits(a(1), in.elem)}, in.loc, true);
                def(in, unit());
                break;
            case Op::ArrayPop: def(in, from_bits(call_rt("ember_rt_array_pop", {a(0)}, in.loc, true), in.elem)); break;
            case Op::SIndex: def(in, call_rt("ember_rt_sindex", {a(0), a(1)}, in.loc, true)); break;
            case Op::Call: {
                std::vector<llvm::Value*> args{llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx_, 0))};
                for (size_t i = 0; i < in.args.size(); ++i) args.push_back(a(i));
                store_loc(in.loc);
                def(in, b_.CreateCall(fns_[static_cast<size_t>(in.imm)], args));
                break;
            }
            case Op::CallNative: def(in, native(in)); break;
            case Op::Jump: b_.CreateBr(blocks_[static_cast<size_t>(in.target)]); break;
            case Op::Branch:
                b_.CreateCondBr(a(0), blocks_[static_cast<size_t>(in.target)], blocks_[static_cast<size_t>(in.target2)]);
                break;
            case Op::Return: {
                b_.CreateStore(depth_in_, g_depth_);
                llvm::Value* r = in.args.empty() ? unit() : a(0);
                b_.CreateRet(r);
                break;
            }
            case Op::Unreachable:
                store_loc(in.loc);
                b_.CreateCall(rt_["ember_rt_trap"], {llvm::ConstantInt::get(i32_, kTrapUnreachable), I(0)});
                b_.CreateUnreachable();
                break;
            default: throw std::logic_error(std::string("unsupported IR op reached the backend: ") + ir::op_name(in.op));
        }
    }

    const ir::Module& m_;
    llvm::LLVMContext& ctx_;
    llvm::Module& mod_;
    llvm::IRBuilder<> b_;
    llvm::Type *i1_, *i8_, *i32_, *i64_, *f64_, *ptr_, *void_;
    llvm::GlobalVariable *g_depth_ = nullptr, *g_max_depth_ = nullptr, *g_frame_fn_ = nullptr, *g_frame_loc_ = nullptr;
    std::unordered_map<std::string, llvm::Function*> rt_;
    std::unordered_map<std::string, llvm::Constant*> strconst_;
    std::vector<llvm::GlobalVariable*> globals_;
    std::vector<llvm::GlobalVariable*> global_init_;
    std::vector<llvm::Function*> fns_;
};

llvm::OptimizationLevel opt_level(int n) {
    switch (n) {
        case 0: return llvm::OptimizationLevel::O0;
        case 1: return llvm::OptimizationLevel::O1;
        case 3: return llvm::OptimizationLevel::O3;
        default: return llvm::OptimizationLevel::O2;
    }
}

// Builds, verifies and optimizes the LLVM module. Returns an error message
// on failure (unsupported program or backend bug).
struct Built {
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> mod;
    std::string error;
    bool unsupported = false;
};

Built build_module(const ir::Module& m, const NativeOptions& opts, llvm::TargetMachine* tm) {
    Built out;
    Support s = check_support(m);
    if (!s.ok) {
        out.unsupported = true;
        out.error = s.reason;
        return out;
    }
    out.ctx = std::make_unique<llvm::LLVMContext>();
    out.mod = std::make_unique<llvm::Module>("ember", *out.ctx);
    if (tm) {
        out.mod->setDataLayout(tm->createDataLayout());
        out.mod->setTargetTriple(tm->getTargetTriple().str());
    }
    try {
        Lowerer(m, *out.ctx, *out.mod).run();
    } catch (const std::exception& e) {
        out.error = std::string("lowering failed: ") + e.what();
        return out;
    }
    std::string verr;
    llvm::raw_string_ostream vs(verr);
    if (llvm::verifyModule(*out.mod, &vs)) {
        out.error = "LLVM verifier rejected the generated module:\n" + vs.str();
        return out;
    }
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb(tm);
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::ModulePassManager mpm = opts.llvm_opt_level <= 0 ? pb.buildO0DefaultPipeline(llvm::OptimizationLevel::O0)
                                                           : pb.buildPerModuleDefaultPipeline(opt_level(opts.llvm_opt_level));
    mpm.run(*out.mod, mam);
    if (llvm::verifyModule(*out.mod, &vs)) {
        out.error = "LLVM verifier rejected the optimized module:\n" + vs.str();
        return out;
    }
    return out;
}

llvm::orc::JITTargetMachineBuilder host_jtmb(const NativeOptions& opts) {
    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        llvm::consumeError(jtmb.takeError());
        return llvm::orc::JITTargetMachineBuilder(llvm::Triple(llvm::sys::getProcessTriple()));
    }
    jtmb->setCodeGenOptLevel(opts.llvm_opt_level >= 2 ? llvm::CodeGenOptLevel::Aggressive
                             : opts.llvm_opt_level == 1 ? llvm::CodeGenOptLevel::Less
                                                        : llvm::CodeGenOptLevel::None);
    return std::move(*jtmb);
}

std::string to_string(llvm::Error e) {
    std::string s;
    llvm::raw_string_ostream os(s);
    os << e;
    return os.str();
}

}  // namespace

bool emit_llvm_ir(const ir::Module& m, const NativeOptions& opts, std::string& text, std::string& error) {
    init_llvm_once();
    auto jtmb = host_jtmb(opts);
    auto tm = jtmb.createTargetMachine();
    if (!tm) {
        error = to_string(tm.takeError());
        return false;
    }
    Built b = build_module(m, opts, tm->get());
    if (!b.mod || !b.error.empty()) {
        error = b.error;
        return false;
    }
    llvm::raw_string_ostream os(text);
    b.mod->print(os, nullptr);
    os.flush();
    return true;
}

NativeRun compile_and_run(const ir::Module& m, std::ostream& out, std::istream& in, const NativeOptions& opts) {
    NativeRun r;
    auto t0 = std::chrono::steady_clock::now();
    init_llvm_once();
    auto jtmb = host_jtmb(opts);
    auto tm = jtmb.createTargetMachine();
    if (!tm) {
        r.internal_error = true;
        r.internal_message = to_string(tm.takeError());
        return r;
    }
    Built b = build_module(m, opts, tm->get());
    if (b.unsupported) {
        r.unsupported = true;
        r.unsupported_reason = b.error;
        return r;
    }
    if (!b.error.empty()) {
        r.internal_error = true;
        r.internal_message = b.error;
        return r;
    }
    auto jit = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(jtmb)).create();
    if (!jit) {
        r.internal_error = true;
        r.internal_message = to_string(jit.takeError());
        return r;
    }
    auto& jd = (*jit)->getMainJITDylib();
    llvm::orc::SymbolMap syms;
    for (const auto& s : runtime_symbols())
        syms[(*jit)->mangleAndIntern(s.name)] = {llvm::orc::ExecutorAddr::fromPtr(s.addr), llvm::JITSymbolFlags::Exported};
    if (auto err = jd.define(llvm::orc::absoluteSymbols(std::move(syms)))) {
        r.internal_error = true;
        r.internal_message = to_string(std::move(err));
        return r;
    }
    // libm fallbacks (e.g. floor/trunc on targets without SSE4.1).
    auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess((*jit)->getDataLayout().getGlobalPrefix());
    if (gen) jd.addGenerator(std::move(*gen));
    else llvm::consumeError(gen.takeError());
    if (auto err = (*jit)->addIRModule(llvm::orc::ThreadSafeModule(std::move(b.mod), std::move(b.ctx)))) {
        r.internal_error = true;
        r.internal_message = to_string(std::move(err));
        return r;
    }
    auto init = (*jit)->lookup("ember_init");
    auto mainf = init ? (*jit)->lookup("ember_main") : llvm::Expected<llvm::orc::ExecutorAddr>(llvm::orc::ExecutorAddr());
    if (!init || !mainf) {
        r.internal_error = true;
        r.internal_message = !init ? to_string(init.takeError()) : to_string(mainf.takeError());
        return r;
    }
    auto t1 = std::chrono::steady_clock::now();
    r.compile_seconds = std::chrono::duration<double>(t1 - t0).count();
    RtConfig cfg;
    cfg.out = &out;
    cfg.in = &in;
    cfg.max_call_depth = opts.max_call_depth;
    cfg.max_string_bytes = opts.max_string_bytes;
    cfg.arena_limit_bytes = opts.arena_limit_bytes;
    for (const auto& f : m.functions) cfg.function_names.push_back(f.name);
    for (const auto& g : m.globals) cfg.global_names.push_back(g.name);
    for (const auto& st : m.structs) {
        RtStructInfo si{st.name, st.field_names, {}};
        for (Kind k : st.field_kinds) si.field_kinds.push_back(static_cast<int64_t>(k));
        cfg.structs.push_back(std::move(si));
    }
    for (const auto& e : m.enums) {
        RtEnumInfo ei;
        ei.name = e.name;
        for (const auto& v : e.variants) {
            ei.variant_names.push_back(v.name);
            std::vector<int64_t> ks;
            for (Kind k : v.field_kinds) ks.push_back(static_cast<int64_t>(k));
            ei.field_kinds.push_back(std::move(ks));
        }
        cfg.enums.push_back(std::move(ei));
    }
    r.outcome = run_entry(cfg, init->toPtr<EntryFn>(), mainf->toPtr<EntryFn>());
    r.run_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    return r;
}

}  // namespace ember::native

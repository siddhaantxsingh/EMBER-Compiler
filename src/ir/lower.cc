#include "ir/lower.h"

#include <climits>
#include <stdexcept>
#include <unordered_map>

#include "common/natives.h"

namespace ember::ir {

Kind kind_of(const Type* t) {
    switch (t->kind) {
        case TypeKind::Int: return Kind::Int;
        case TypeKind::Float: return Kind::Float;
        case TypeKind::Bool: return Kind::Bool;
        case TypeKind::Unit: return Kind::Unit;
        case TypeKind::Error: throw std::logic_error("lowering a program with type errors");
        default: return Kind::Ref;
    }
}

uint32_t Module::intern_string(const std::string& s) {
    for (size_t i = 0; i < strings.size(); ++i)
        if (strings[i] == s) return static_cast<uint32_t>(i);
    strings.push_back(s);
    return static_cast<uint32_t>(strings.size() - 1);
}

uint32_t Module::intern_signature(const Signature& s) {
    for (size_t i = 0; i < signatures.size(); ++i)
        if (signatures[i] == s) return static_cast<uint32_t>(i);
    signatures.push_back(s);
    return static_cast<uint32_t>(signatures.size() - 1);
}

namespace {

class FnLowerer;

struct ModuleLowerer {
    Program& prog;
    SemaResult& sema;
    LowerOptions opts;
    Module mod;

    ModuleLowerer(Program& p, SemaResult& s, LowerOptions o) : prog(p), sema(s), opts(o) {}
    void run();
    void lower_function(FnInfo* info);
};

class FnLowerer {
public:
    FnLowerer(ModuleLowerer& ml, FnInfo* info, Function& f) : ml_(ml), info_(info), f_(f) {}

    void lower_body(FnDecl& decl);
    void lower_globals_init();

private:
    struct Scope {
        int min_captured = INT_MAX;
    };
    struct Loop {
        BlockId break_target, continue_target;
        size_t outer_depth;  // scopes to keep on break
        size_t body_depth;   // scopes to keep on continue
    };

    // --- emission
    Instr& emit(Instr in);
    TempId emit_value(Op op, Kind k, std::vector<TempId> args, Span loc, int64_t imm = 0, int64_t imm2 = 0,
                      int64_t imm3 = 0);
    void emit_void(Op op, std::vector<TempId> args, Span loc, int64_t imm = 0, int64_t imm2 = 0);
    void jump(BlockId b, Span loc);
    void branch(TempId cond, BlockId t, BlockId f, Span loc);
    void set_block(BlockId b) { cur_ = b; }
    bool terminated() const { return f_.blocks[static_cast<size_t>(cur_)].terminated(); }
    TempId const_int(int64_t v, Span loc);
    TempId const_unit(Span loc);

    // --- variables
    int slot_of(Symbol* s);
    int new_hidden_local(Kind k);
    void declare_local_value(Symbol* s, TempId value, Span loc);
    TempId load_symbol(Symbol* s, Span loc);
    void store_symbol(Symbol* s, TempId v, Span loc);

    // --- scopes
    void push_scope() { scopes_.emplace_back(); }
    void pop_scope(Span loc);
    void close_scopes_above(size_t depth, Span loc);
    void note_captured(Symbol* s);

    // --- statements and expressions
    void lower_block(ember::Block& b);
    void lower_stmt(Stmt& s);
    void lower_if(IfStmt& s);
    void lower_assign(AssignStmt& a);
    TempId lower_expr(Expr& e);
    TempId lower_call(CallExpr& c);
    TempId lower_builtin(CallExpr& c, Builtin b);
    TempId lower_binary(BinaryExpr& b);
    TempId binop(BinaryOp op, Type* operand_type, TempId l, TempId r, Span loc);
    TempId lower_logical(LogicalExpr& l);
    TempId lower_match(MatchExpr& m);
    TempId lower_closure(FnDecl& fn, Span loc);
    void finish_function(Kind ret, Span loc);

    ModuleLowerer& ml_;
    FnInfo* info_;
    Function& f_;
    BlockId cur_ = 0;
    std::unordered_map<Symbol*, int> slots_;
    std::vector<Scope> scopes_;
    std::vector<Loop> loops_;
};

// ------------------------------------------------------------ emission

Instr& FnLowerer::emit(Instr in) {
    if (terminated()) {
        // Code after return/break/continue: put it in a fresh block that has
        // no predecessors; the optimizer deletes it.
        cur_ = f_.new_block();
    }
    auto& b = f_.blocks[static_cast<size_t>(cur_)];
    b.instrs.push_back(std::move(in));
    return b.instrs.back();
}

TempId FnLowerer::emit_value(Op op, Kind k, std::vector<TempId> args, Span loc, int64_t imm, int64_t imm2,
                             int64_t imm3) {
    Instr in;
    in.op = op;
    in.dst = f_.new_temp(k);
    in.args = std::move(args);
    in.imm = imm;
    in.imm2 = imm2;
    in.imm3 = imm3;
    in.loc = loc;
    return emit(std::move(in)).dst;
}

void FnLowerer::emit_void(Op op, std::vector<TempId> args, Span loc, int64_t imm, int64_t imm2) {
    Instr in;
    in.op = op;
    in.args = std::move(args);
    in.imm = imm;
    in.imm2 = imm2;
    in.loc = loc;
    emit(std::move(in));
}

void FnLowerer::jump(BlockId b, Span loc) {
    if (terminated()) return;
    Instr in;
    in.op = Op::Jump;
    in.target = b;
    in.loc = loc;
    emit(std::move(in));
}

void FnLowerer::branch(TempId cond, BlockId t, BlockId f, Span loc) {
    Instr in;
    in.op = Op::Branch;
    in.args = {cond};
    in.target = t;
    in.target2 = f;
    in.loc = loc;
    emit(std::move(in));
}

TempId FnLowerer::const_int(int64_t v, Span loc) { return emit_value(Op::ConstInt, Kind::Int, {}, loc, v); }
TempId FnLowerer::const_unit(Span loc) { return emit_value(Op::ConstUnit, Kind::Unit, {}, loc); }

// ------------------------------------------------------------ variables

int FnLowerer::slot_of(Symbol* s) {
    auto it = slots_.find(s);
    if (it == slots_.end()) throw std::logic_error("lowering: local without a slot: " + s->name);
    return it->second;
}

int FnLowerer::new_hidden_local(Kind k) {
    f_.locals.push_back(LocalInfo{"", k, 0});
    return static_cast<int>(f_.locals.size() - 1);
}

void FnLowerer::note_captured(Symbol* s) {
    if (s->captured && !scopes_.empty()) {
        int slot = slot_of(s);
        if (slot < scopes_.back().min_captured) scopes_.back().min_captured = slot;
    }
}

void FnLowerer::declare_local_value(Symbol* s, TempId value, Span loc) {
    emit_void(Op::StoreLocal, {value}, loc, slot_of(s));
    note_captured(s);
}

TempId FnLowerer::load_symbol(Symbol* s, Span loc) {
    switch (s->kind) {
        case SymbolKind::Local:
            if (s->owner == info_) return emit_value(Op::LoadLocal, kind_of(s->type), {}, loc, slot_of(s));
            return emit_value(Op::LoadUpval, kind_of(s->type), {}, loc, info_->capture_index(s));
        case SymbolKind::Global:
            return emit_value(Op::LoadGlobal, kind_of(s->type), {}, loc, s->global_index);
        case SymbolKind::Function:
            // A top-level function used as a value: a closure with no upvalues.
            return emit_value(Op::MakeClosure, Kind::Ref, {}, loc, s->fn->info->index);
        case SymbolKind::Const:
            switch (s->type->kind) {
                case TypeKind::Int: return emit_value(Op::ConstInt, Kind::Int, {}, loc, s->const_int);
                case TypeKind::Bool: return emit_value(Op::ConstBool, Kind::Bool, {}, loc, s->const_bool);
                case TypeKind::String:
                    return emit_value(Op::ConstString, Kind::Ref, {}, loc, ml_.mod.intern_string(s->const_string));
                case TypeKind::Float: {
                    TempId t = emit_value(Op::ConstFloat, Kind::Float, {}, loc);
                    f_.blocks[static_cast<size_t>(cur_)].instrs.back().fimm = s->const_float;
                    return t;
                }
                default: break;
            }
            break;
        case SymbolKind::Builtin: break;
    }
    throw std::logic_error("lowering: cannot load symbol " + s->name);
}

void FnLowerer::store_symbol(Symbol* s, TempId v, Span loc) {
    switch (s->kind) {
        case SymbolKind::Local:
            if (s->owner == info_) emit_void(Op::StoreLocal, {v}, loc, slot_of(s));
            else emit_void(Op::StoreUpval, {v}, loc, info_->capture_index(s));
            return;
        case SymbolKind::Global: emit_void(Op::StoreGlobal, {v}, loc, s->global_index); return;
        default: throw std::logic_error("lowering: cannot store to " + s->name);
    }
}

// ------------------------------------------------------------ scopes

void FnLowerer::pop_scope(Span loc) {
    Scope s = scopes_.back();
    scopes_.pop_back();
    if (s.min_captured != INT_MAX && !terminated()) emit_void(Op::CloseUpvalues, {}, loc, s.min_captured);
}

void FnLowerer::close_scopes_above(size_t depth, Span loc) {
    int min = INT_MAX;
    for (size_t i = depth; i < scopes_.size(); ++i) min = std::min(min, scopes_[i].min_captured);
    if (min != INT_MAX) emit_void(Op::CloseUpvalues, {}, loc, min);
}

// ------------------------------------------------------------ functions

void FnLowerer::finish_function(Kind ret, Span loc) {
    if (terminated()) return;
    Instr in;
    in.loc = loc;
    if (ret == Kind::Unit) {
        in.op = Op::Return;
        in.args = {const_unit(loc)};
    } else {
        // The type checker proved every path returns (or panics).
        in.op = Op::Unreachable;
    }
    emit(std::move(in));
}

void FnLowerer::lower_body(FnDecl& decl) {
    f_.name = info_->display_name;
    f_.loc = decl.span;
    Type* ft = decl.fn_type;
    f_.num_params = static_cast<uint32_t>(decl.params.size());
    for (Type* p : ft->params) f_.param_kinds.push_back(kind_of(p));
    f_.ret_kind = kind_of(ft->ret);
    for (size_t i = 0; i < info_->locals.size(); ++i) {
        Symbol* s = info_->locals[i];
        slots_[s] = static_cast<int>(i);
        f_.locals.push_back(LocalInfo{s->name, kind_of(s->type), s->span.line});
    }
    for (auto& c : info_->captures) {
        f_.upvalue_kinds.push_back(kind_of(c.sym->type));
        f_.upvalue_names.push_back(c.sym->name);
        uint32_t index = 0;
        if (c.from_parent_local) {
            // Slot of the variable in the parent frame (locals are params first,
            // then declaration order; the slot is the position in that list).
            const auto& pl = info_->parent->locals;
            size_t k = 0;
            while (k < pl.size() && pl[k] != c.sym) ++k;
            if (k == pl.size()) throw std::logic_error("lowering: captured local not found in parent");
            index = static_cast<uint32_t>(k);
        } else {
            index = static_cast<uint32_t>(c.parent_upvalue_index);
        }
        f_.captures.push_back(CaptureDesc{c.from_parent_local, index});
    }
    cur_ = f_.new_block();
    push_scope();
    for (auto& p : decl.params) note_captured(p.sym);
    lower_block(*decl.body);
    scopes_.pop_back();  // parameters are closed by the VM on return
    finish_function(f_.ret_kind, Span{decl.body->span.end, decl.body->span.end, decl.body->span.line, 1});
}

void FnLowerer::lower_globals_init() {
    f_.name = "<globals>";
    f_.ret_kind = Kind::Unit;
    // Initializers can declare locals too (match bindings, lambda captures).
    for (size_t i = 0; i < info_->locals.size(); ++i) {
        Symbol* s = info_->locals[i];
        slots_[s] = static_cast<int>(i);
        f_.locals.push_back(LocalInfo{s->name, kind_of(s->type), s->span.line});
    }
    cur_ = f_.new_block();
    push_scope();
    for (auto& item : ml_.prog.items) {
        if (item.kind != ItemKind::Global) continue;
        auto& g = *item.global;
        TempId v = lower_expr(*g.init);
        emit_void(Op::StoreGlobal, {v}, g.span, g.sym->global_index);
    }
    scopes_.pop_back();
    finish_function(Kind::Unit, Span{});
}

TempId FnLowerer::lower_closure(FnDecl& fn, Span loc) {
    FnInfo* child = fn.info;
    // Captures are read from this frame by the VM when MakeClosure runs; the
    // descriptors live in the child's metadata.
    return emit_value(Op::MakeClosure, Kind::Ref, {}, loc, child->index);
}

// ------------------------------------------------------------ statements

void FnLowerer::lower_block(ember::Block& b) {
    push_scope();
    // Nested functions are bound at block entry so they can call each other
    // (and themselves) regardless of order. Such a closure may run before a
    // captured `let` of this block has executed (or, in a loop, see the
    // previous iteration's slot), so those slots are reset to
    // "uninitialized" first; reading one through an upvalue traps.
    bool has_nested_fn = false;
    for (auto& s : b.stmts) has_nested_fn |= s->kind == StmtKind::Fn;
    if (has_nested_fn) {
        for (auto& s : b.stmts) {
            if (s->kind != StmtKind::Let) continue;
            auto& l = static_cast<LetStmt&>(*s);
            if (l.sym && l.sym->captured) emit_void(Op::ClearLocal, {}, l.span, slot_of(l.sym));
        }
    }
    for (auto& s : b.stmts) {
        if (s->kind != StmtKind::Fn) continue;
        auto& fs = static_cast<FnStmt&>(*s);
        TempId c = lower_closure(*fs.fn, fs.span);
        declare_local_value(fs.fn->sym, c, fs.span);
    }
    for (auto& s : b.stmts) lower_stmt(*s);
    pop_scope(Span{b.span.end, b.span.end, b.span.line, 1});
}

void FnLowerer::lower_if(IfStmt& s) {
    TempId c = lower_expr(*s.cond);
    BlockId then_b = f_.new_block();
    BlockId join = f_.new_block();
    BlockId else_b = s.else_stmt ? f_.new_block() : join;
    branch(c, then_b, else_b, s.cond->span);
    set_block(then_b);
    lower_block(*s.then_block);
    jump(join, s.span);
    if (s.else_stmt) {
        set_block(else_b);
        lower_stmt(*s.else_stmt);
        jump(join, s.span);
    }
    set_block(join);
}

void FnLowerer::lower_stmt(Stmt& s) {
    switch (s.kind) {
        case StmtKind::Let: {
            auto& l = static_cast<LetStmt&>(s);
            TempId v = lower_expr(*l.init);
            declare_local_value(l.sym, v, l.span);
            break;
        }
        case StmtKind::Fn: break;  // bound at block entry
        case StmtKind::If: lower_if(static_cast<IfStmt&>(s)); break;
        case StmtKind::While: {
            auto& w = static_cast<WhileStmt&>(s);
            BlockId head = f_.new_block(), body = f_.new_block(), exit = f_.new_block();
            jump(head, w.span);
            set_block(head);
            TempId c = lower_expr(*w.cond);
            branch(c, body, exit, w.cond->span);
            set_block(body);
            // With rotation, the condition is evaluated again at the bottom
            // (`latch`), so the back edge is a conditional branch instead of
            // a jump to the top followed by the test.
            BlockId latch = ml_.opts.rotate_loops ? f_.new_block() : head;
            loops_.push_back(Loop{exit, latch, scopes_.size(), scopes_.size()});
            lower_block(*w.body);
            loops_.pop_back();
            jump(latch, w.span);
            if (ml_.opts.rotate_loops) {
                set_block(latch);
                TempId c2 = lower_expr(*w.cond);
                branch(c2, body, exit, w.cond->span);
            }
            set_block(exit);
            break;
        }
        case StmtKind::ForRange: {
            auto& f = static_cast<ForRangeStmt&>(s);
            int counter = new_hidden_local(Kind::Int), limit = new_hidden_local(Kind::Int);
            emit_void(Op::StoreLocal, {lower_expr(*f.start)}, f.start->span, counter);
            emit_void(Op::StoreLocal, {lower_expr(*f.end)}, f.end->span, limit);
            BlockId head = f_.new_block(), body = f_.new_block(), inc = f_.new_block(), exit = f_.new_block();
            jump(head, f.span);
            set_block(head);
            TempId cv = emit_value(Op::LoadLocal, Kind::Int, {}, f.span, counter);
            TempId lv = emit_value(Op::LoadLocal, Kind::Int, {}, f.span, limit);
            TempId cond = emit_value(Op::ICmp, Kind::Bool, {cv, lv}, f.span, static_cast<int64_t>(CmpOp::Lt));
            branch(cond, body, exit, f.span);
            set_block(body);
            size_t outer = scopes_.size();
            push_scope();  // loop variable scope
            TempId iv = emit_value(Op::LoadLocal, Kind::Int, {}, f.var_span, counter);
            declare_local_value(f.sym, iv, f.var_span);
            loops_.push_back(Loop{exit, inc, outer, scopes_.size()});
            lower_block(*f.body);
            loops_.pop_back();
            jump(inc, f.span);
            set_block(inc);
            if (f.sym->captured) emit_void(Op::CloseUpvalues, {}, f.span, slot_of(f.sym));
            scopes_.pop_back();
            TempId c2 = emit_value(Op::LoadLocal, Kind::Int, {}, f.span, counter);
            TempId one = const_int(1, f.span);
            TempId n = emit_value(Op::IAdd, Kind::Int, {c2, one}, f.span);
            emit_void(Op::StoreLocal, {n}, f.span, counter);
            if (ml_.opts.rotate_loops) {
                // Reload rather than reuse n: a single-use temp stays on the
                // VM operand stack, a reused one would need a frame slot.
                TempId cv2 = emit_value(Op::LoadLocal, Kind::Int, {}, f.span, counter);
                TempId lv2 = emit_value(Op::LoadLocal, Kind::Int, {}, f.span, limit);
                TempId again = emit_value(Op::ICmp, Kind::Bool, {cv2, lv2}, f.span, static_cast<int64_t>(CmpOp::Lt));
                branch(again, body, exit, f.span);
            } else {
                jump(head, f.span);
            }
            set_block(exit);
            break;
        }
        case StmtKind::ForEach: {
            auto& f = static_cast<ForEachStmt&>(s);
            Kind ek = kind_of(f.sym->type);
            int arr = new_hidden_local(Kind::Ref), idx = new_hidden_local(Kind::Int);
            emit_void(Op::StoreLocal, {lower_expr(*f.iterable)}, f.iterable->span, arr);
            emit_void(Op::StoreLocal, {const_int(0, f.span)}, f.span, idx);
            BlockId head = f_.new_block(), body = f_.new_block(), inc = f_.new_block(), exit = f_.new_block();
            jump(head, f.span);
            set_block(head);
            TempId iv = emit_value(Op::LoadLocal, Kind::Int, {}, f.span, idx);
            TempId av = emit_value(Op::LoadLocal, Kind::Ref, {}, f.span, arr);
            TempId len = emit_value(Op::ArrayLen, Kind::Int, {av}, f.span);
            TempId cond = emit_value(Op::ICmp, Kind::Bool, {iv, len}, f.span, static_cast<int64_t>(CmpOp::Lt));
            branch(cond, body, exit, f.span);
            set_block(body);
            size_t outer = scopes_.size();
            push_scope();
            TempId a2 = emit_value(Op::LoadLocal, Kind::Ref, {}, f.span, arr);
            TempId i2 = emit_value(Op::LoadLocal, Kind::Int, {}, f.span, idx);
            Instr get;
            get.op = Op::ArrayGet;
            get.dst = f_.new_temp(ek);
            get.args = {a2, i2};
            get.elem = ek;
            get.loc = f.var_span;
            TempId elem = emit(std::move(get)).dst;
            declare_local_value(f.sym, elem, f.var_span);
            loops_.push_back(Loop{exit, inc, outer, scopes_.size()});
            lower_block(*f.body);
            loops_.pop_back();
            jump(inc, f.span);
            set_block(inc);
            if (f.sym->captured) emit_void(Op::CloseUpvalues, {}, f.span, slot_of(f.sym));
            scopes_.pop_back();
            TempId i3 = emit_value(Op::LoadLocal, Kind::Int, {}, f.span, idx);
            TempId n = emit_value(Op::IAdd, Kind::Int, {i3, const_int(1, f.span)}, f.span);
            emit_void(Op::StoreLocal, {n}, f.span, idx);
            if (ml_.opts.rotate_loops) {
                TempId i4 = emit_value(Op::LoadLocal, Kind::Int, {}, f.span, idx);
                TempId av2 = emit_value(Op::LoadLocal, Kind::Ref, {}, f.span, arr);
                TempId len2 = emit_value(Op::ArrayLen, Kind::Int, {av2}, f.span);
                TempId again = emit_value(Op::ICmp, Kind::Bool, {i4, len2}, f.span, static_cast<int64_t>(CmpOp::Lt));
                branch(again, body, exit, f.span);
            } else {
                jump(head, f.span);
            }
            set_block(exit);
            break;
        }
        case StmtKind::Break: {
            const Loop& l = loops_.back();
            close_scopes_above(l.outer_depth, s.span);
            jump(l.break_target, s.span);
            break;
        }
        case StmtKind::Continue: {
            const Loop& l = loops_.back();
            close_scopes_above(l.body_depth, s.span);
            jump(l.continue_target, s.span);
            break;
        }
        case StmtKind::Return: {
            auto& r = static_cast<ReturnStmt&>(s);
            TempId v = r.value ? lower_expr(*r.value) : const_unit(s.span);
            Instr in;
            in.op = Op::Return;
            in.args = {v};
            in.loc = s.span;
            emit(std::move(in));
            break;
        }
        case StmtKind::Assign: lower_assign(static_cast<AssignStmt&>(s)); break;
        case StmtKind::ExprStmt: lower_expr(*static_cast<ExprStmt&>(s).expr); break;
        case StmtKind::BlockStmt: lower_block(*static_cast<BlockStmt&>(s).block); break;
    }
}

void FnLowerer::lower_assign(AssignStmt& a) {
    Expr& t = *a.target;
    Type* tt = t.type;
    switch (t.kind) {
        case ExprKind::Ident: {
            Symbol* s = static_cast<IdentExpr&>(t).sym;
            TempId v;
            if (a.compound) {
                TempId old = load_symbol(s, t.span);
                TempId rhs = lower_expr(*a.value);
                v = binop(a.op, tt, old, rhs, a.span);
            } else {
                v = lower_expr(*a.value);
            }
            store_symbol(s, v, a.span);
            return;
        }
        case ExprKind::Index: {
            auto& ix = static_cast<IndexExpr&>(t);
            Kind ek = kind_of(tt);
            TempId arr = lower_expr(*ix.object);
            TempId idx = lower_expr(*ix.index);
            TempId v;
            if (a.compound) {
                Instr get;
                get.op = Op::ArrayGet;
                get.dst = f_.new_temp(ek);
                get.args = {arr, idx};
                get.elem = ek;
                get.loc = t.span;
                TempId old = emit(std::move(get)).dst;
                TempId rhs = lower_expr(*a.value);
                v = binop(a.op, tt, old, rhs, a.span);
            } else {
                v = lower_expr(*a.value);
            }
            Instr set;
            set.op = Op::ArraySet;
            set.args = {arr, idx, v};
            set.elem = ek;
            set.loc = a.span;
            emit(std::move(set));
            return;
        }
        case ExprKind::Field: {
            auto& fe = static_cast<FieldExpr&>(t);
            int sid = fe.object->type->struct_decl->index;
            TempId obj = lower_expr(*fe.object);
            TempId v;
            if (a.compound) {
                TempId old = emit_value(Op::GetField, kind_of(tt), {obj}, t.span, sid, fe.field_index);
                TempId rhs = lower_expr(*a.value);
                v = binop(a.op, tt, old, rhs, a.span);
            } else {
                v = lower_expr(*a.value);
            }
            emit_void(Op::SetField, {obj, v}, a.span, sid, fe.field_index);
            return;
        }
        default: throw std::logic_error("lowering: invalid assignment target");
    }
}

// ------------------------------------------------------------ expressions

TempId FnLowerer::binop(BinaryOp op, Type* ty, TempId l, TempId r, Span loc) {
    auto cmp = [&](Op o, CmpOp c) { return emit_value(o, Kind::Bool, {l, r}, loc, static_cast<int64_t>(c)); };
    auto cmp_op = [](BinaryOp b) {
        switch (b) {
            case BinaryOp::Eq: return CmpOp::Eq;
            case BinaryOp::Ne: return CmpOp::Ne;
            case BinaryOp::Lt: return CmpOp::Lt;
            case BinaryOp::Le: return CmpOp::Le;
            case BinaryOp::Gt: return CmpOp::Gt;
            default: return CmpOp::Ge;
        }
    };
    bool is_cmp = op == BinaryOp::Eq || op == BinaryOp::Ne || op == BinaryOp::Lt || op == BinaryOp::Le ||
                  op == BinaryOp::Gt || op == BinaryOp::Ge;
    switch (ty->kind) {
        case TypeKind::Int:
            if (is_cmp) return cmp(Op::ICmp, cmp_op(op));
            switch (op) {
                case BinaryOp::Add: return emit_value(Op::IAdd, Kind::Int, {l, r}, loc);
                case BinaryOp::Sub: return emit_value(Op::ISub, Kind::Int, {l, r}, loc);
                case BinaryOp::Mul: return emit_value(Op::IMul, Kind::Int, {l, r}, loc);
                case BinaryOp::Div: return emit_value(Op::IDiv, Kind::Int, {l, r}, loc);
                case BinaryOp::Mod: return emit_value(Op::IMod, Kind::Int, {l, r}, loc);
                default: break;
            }
            break;
        case TypeKind::Float:
            if (is_cmp) return cmp(Op::FCmp, cmp_op(op));
            switch (op) {
                case BinaryOp::Add: return emit_value(Op::FAdd, Kind::Float, {l, r}, loc);
                case BinaryOp::Sub: return emit_value(Op::FSub, Kind::Float, {l, r}, loc);
                case BinaryOp::Mul: return emit_value(Op::FMul, Kind::Float, {l, r}, loc);
                case BinaryOp::Div: return emit_value(Op::FDiv, Kind::Float, {l, r}, loc);
                default: break;
            }
            break;
        case TypeKind::String:
            if (is_cmp) return cmp(Op::SCmp, cmp_op(op));
            if (op == BinaryOp::Add) return emit_value(Op::SConcat, Kind::Ref, {l, r}, loc);
            break;
        case TypeKind::Bool:
            if (op == BinaryOp::Eq || op == BinaryOp::Ne) return cmp(Op::BCmp, cmp_op(op));
            break;
        case TypeKind::Unit:
            // () == () is always true. Both operands were evaluated for
            // their side effects already.
            if (op == BinaryOp::Eq || op == BinaryOp::Ne)
                return emit_value(Op::ConstBool, Kind::Bool, {}, loc, op == BinaryOp::Eq);
            break;
        default: break;
    }
    throw std::logic_error("lowering: unsupported binary operation");
}

TempId FnLowerer::lower_binary(BinaryExpr& b) {
    TempId l = lower_expr(*b.lhs);
    TempId r = lower_expr(*b.rhs);
    return binop(b.op, b.lhs->type, l, r, b.op_span.valid() ? b.op_span : b.span);
}

TempId FnLowerer::lower_logical(LogicalExpr& l) {
    // r = lhs; if (r is the deciding value) skip; else r = rhs
    int r = new_hidden_local(Kind::Bool);
    TempId lv = lower_expr(*l.lhs);
    emit_void(Op::StoreLocal, {lv}, l.lhs->span, r);
    BlockId rhs_b = f_.new_block(), join = f_.new_block();
    if (l.op == LogicalOp::And) branch(lv, rhs_b, join, l.span);
    else branch(lv, join, rhs_b, l.span);
    set_block(rhs_b);
    TempId rv = lower_expr(*l.rhs);
    emit_void(Op::StoreLocal, {rv}, l.rhs->span, r);
    jump(join, l.span);
    set_block(join);
    return emit_value(Op::LoadLocal, Kind::Bool, {}, l.span, r);
}

TempId FnLowerer::lower_builtin(CallExpr& c, Builtin b) {
    auto native = [&](NativeId id, Kind ret) {
        std::vector<TempId> args;
        for (auto& a : c.args) args.push_back(lower_expr(*a));
        return emit_value(Op::CallNative, ret, std::move(args), c.span, id);
    };
    switch (b) {
        case Builtin::Print: return native(kNativePrint, Kind::Unit);
        case Builtin::Str: return native(kNativeStr, Kind::Ref);
        case Builtin::Input: return native(kNativeInput, Kind::Ref);
        case Builtin::ParseInt: return native(kNativeParseInt, Kind::Int);
        case Builtin::Panic: return native(kNativePanic, Kind::Unit);
        case Builtin::Assert: return native(kNativeAssert, Kind::Unit);
        case Builtin::Len: {
            TempId v = lower_expr(*c.args[0]);
            return emit_value(c.args[0]->type->kind == TypeKind::String ? Op::SLen : Op::ArrayLen, Kind::Int, {v}, c.span);
        }
        case Builtin::Push: {
            TempId arr = lower_expr(*c.args[0]);
            TempId v = lower_expr(*c.args[1]);
            Instr in;
            in.op = Op::ArrayPush;
            in.dst = f_.new_temp(Kind::Unit);
            in.args = {arr, v};
            in.elem = kind_of(c.args[0]->type->elem);
            in.loc = c.span;
            return emit(std::move(in)).dst;
        }
        case Builtin::Pop: {
            TempId arr = lower_expr(*c.args[0]);
            Kind ek = kind_of(c.args[0]->type->elem);
            Instr in;
            in.op = Op::ArrayPop;
            in.dst = f_.new_temp(ek);
            in.args = {arr};
            in.elem = ek;
            in.loc = c.span;
            return emit(std::move(in)).dst;
        }
        case Builtin::Int: return emit_value(Op::F2I, Kind::Int, {lower_expr(*c.args[0])}, c.span);
        case Builtin::Float: return emit_value(Op::I2F, Kind::Float, {lower_expr(*c.args[0])}, c.span);
    }
    throw std::logic_error("lowering: unknown builtin");
}

TempId FnLowerer::lower_call(CallExpr& c) {
    Kind ret = kind_of(c.type);
    if (c.callee->kind == ExprKind::Ident) {
        Symbol* s = static_cast<IdentExpr&>(*c.callee).sym;
        if (s->kind == SymbolKind::Builtin) return lower_builtin(c, s->builtin);
        if (s->kind == SymbolKind::Function) {
            // Direct call: top-level functions have no upvalues.
            std::vector<TempId> args;
            for (auto& a : c.args) args.push_back(lower_expr(*a));
            return emit_value(Op::Call, ret, std::move(args), c.span, s->fn->info->index);
        }
    }
    Type* ft = c.callee->type;
    Signature sig;
    for (Type* p : ft->params) sig.params.push_back(kind_of(p));
    sig.ret = kind_of(ft->ret);
    uint32_t sid = ml_.mod.intern_signature(sig);
    std::vector<TempId> args;
    args.push_back(lower_expr(*c.callee));
    for (auto& a : c.args) args.push_back(lower_expr(*a));
    return emit_value(Op::CallIndirect, ret, std::move(args), c.span, sid);
}

TempId FnLowerer::lower_match(MatchExpr& m) {
    Kind rk = kind_of(m.type);
    int result = new_hidden_local(rk);
    Type* st = m.scrutinee->type;
    int scrut = new_hidden_local(kind_of(st));
    emit_void(Op::StoreLocal, {lower_expr(*m.scrutinee)}, m.scrutinee->span, scrut);
    int tag = -1;
    if (st->kind == TypeKind::Enum) {
        tag = new_hidden_local(Kind::Int);
        TempId sv = emit_value(Op::LoadLocal, Kind::Ref, {}, m.scrutinee->span, scrut);
        TempId tv = emit_value(Op::EnumTag, Kind::Int, {sv}, m.scrutinee->span, st->enum_decl->index);
        emit_void(Op::StoreLocal, {tv}, m.scrutinee->span, tag);
    }
    BlockId join = f_.new_block();
    for (auto& arm : m.arms) {
        Pattern& p = arm.pattern;
        BlockId body = f_.new_block();
        BlockId next = f_.new_block();
        Span ploc = p.span;
        switch (p.kind) {
            case PatternKind::Wildcard: jump(body, ploc); break;
            case PatternKind::Variant: {
                TempId tv = emit_value(Op::LoadLocal, Kind::Int, {}, ploc, tag);
                TempId want = const_int(p.variant_index, ploc);
                TempId c = emit_value(Op::ICmp, Kind::Bool, {tv, want}, ploc, static_cast<int64_t>(CmpOp::Eq));
                branch(c, body, next, ploc);
                break;
            }
            case PatternKind::Int: {
                TempId sv = emit_value(Op::LoadLocal, Kind::Int, {}, ploc, scrut);
                TempId want = const_int(p.int_value, ploc);
                branch(emit_value(Op::ICmp, Kind::Bool, {sv, want}, ploc, static_cast<int64_t>(CmpOp::Eq)), body,
                       next, ploc);
                break;
            }
            case PatternKind::String: {
                TempId sv = emit_value(Op::LoadLocal, Kind::Ref, {}, ploc, scrut);
                TempId want = emit_value(Op::ConstString, Kind::Ref, {}, ploc, ml_.mod.intern_string(p.string_value));
                branch(emit_value(Op::SCmp, Kind::Bool, {sv, want}, ploc, static_cast<int64_t>(CmpOp::Eq)), body,
                       next, ploc);
                break;
            }
            case PatternKind::Bool: {
                TempId sv = emit_value(Op::LoadLocal, Kind::Bool, {}, ploc, scrut);
                TempId want = emit_value(Op::ConstBool, Kind::Bool, {}, ploc, p.bool_value);
                branch(emit_value(Op::BCmp, Kind::Bool, {sv, want}, ploc, static_cast<int64_t>(CmpOp::Eq)), body,
                       next, ploc);
                break;
            }
        }
        set_block(body);
        push_scope();
        if (p.kind == PatternKind::Variant) {
            auto& v = p.decl->variants[static_cast<size_t>(p.variant_index)];
            for (size_t i = 0; i < p.binding_syms.size(); ++i) {
                Symbol* bs = p.binding_syms[i];
                if (!bs) continue;
                TempId sv = emit_value(Op::LoadLocal, Kind::Ref, {}, p.binding_spans[i], scrut);
                TempId fv = emit_value(Op::EnumField, kind_of(v.resolved[i]), {sv}, p.binding_spans[i],
                                       p.decl->index, p.variant_index, static_cast<int64_t>(i));
                declare_local_value(bs, fv, p.binding_spans[i]);
            }
        }
        TempId av;
        if (arm.block) {
            lower_block(*arm.block);
            av = const_unit(arm.span);
        } else {
            av = lower_expr(*arm.expr);
        }
        if (!terminated()) emit_void(Op::StoreLocal, {av}, arm.span, result);
        pop_scope(arm.span);
        jump(join, arm.span);
        set_block(next);
    }
    // Every value is matched (checked exhaustively), so the final "next"
    // block is unreachable.
    Instr un;
    un.op = Op::Unreachable;
    un.loc = m.span;
    emit(std::move(un));
    set_block(join);
    return emit_value(Op::LoadLocal, rk, {}, m.span, result);
}

TempId FnLowerer::lower_expr(Expr& e) {
    Span loc = e.span;
    switch (e.kind) {
        case ExprKind::IntLit: return const_int(static_cast<IntLit&>(e).value, loc);
        case ExprKind::FloatLit: {
            TempId t = emit_value(Op::ConstFloat, Kind::Float, {}, loc);
            f_.blocks[static_cast<size_t>(cur_)].instrs.back().fimm = static_cast<FloatLit&>(e).value;
            return t;
        }
        case ExprKind::BoolLit: return emit_value(Op::ConstBool, Kind::Bool, {}, loc, static_cast<BoolLit&>(e).value);
        case ExprKind::StringLit:
            return emit_value(Op::ConstString, Kind::Ref, {}, loc,
                              ml_.mod.intern_string(static_cast<StringLit&>(e).value));
        case ExprKind::UnitLit: return const_unit(loc);
        case ExprKind::Ident: return load_symbol(static_cast<IdentExpr&>(e).sym, loc);
        case ExprKind::Unary: {
            auto& u = static_cast<UnaryExpr&>(e);
            TempId v = lower_expr(*u.operand);
            if (u.op == UnaryOp::Not) return emit_value(Op::Not, Kind::Bool, {v}, loc);
            if (u.operand->type->kind == TypeKind::Int) return emit_value(Op::INeg, Kind::Int, {v}, loc);
            return emit_value(Op::FNeg, Kind::Float, {v}, loc);
        }
        case ExprKind::Binary: return lower_binary(static_cast<BinaryExpr&>(e));
        case ExprKind::Logical: return lower_logical(static_cast<LogicalExpr&>(e));
        case ExprKind::Call: return lower_call(static_cast<CallExpr&>(e));
        case ExprKind::ModuleCall: {
            auto& mc = static_cast<ModuleCallExpr&>(e);
            std::vector<TempId> args;
            for (auto& a : mc.args) args.push_back(lower_expr(*a));
            return emit_value(Op::CallNative, kind_of(e.type), std::move(args), loc, kFirstModuleNative + mc.native_id);
        }
        case ExprKind::Index: {
            auto& ix = static_cast<IndexExpr&>(e);
            TempId o = lower_expr(*ix.object);
            TempId i = lower_expr(*ix.index);
            if (ix.object->type->kind == TypeKind::String) return emit_value(Op::SIndex, Kind::Ref, {o, i}, loc);
            Kind ek = kind_of(e.type);
            Instr get;
            get.op = Op::ArrayGet;
            get.dst = f_.new_temp(ek);
            get.args = {o, i};
            get.elem = ek;
            get.loc = loc;
            return emit(std::move(get)).dst;
        }
        case ExprKind::Field: {
            auto& fe = static_cast<FieldExpr&>(e);
            TempId o = lower_expr(*fe.object);
            return emit_value(Op::GetField, kind_of(e.type), {o}, fe.field_span,
                              fe.object->type->struct_decl->index, fe.field_index);
        }
        case ExprKind::ArrayLit: {
            auto& a = static_cast<ArrayLit&>(e);
            std::vector<TempId> elems;
            for (auto& x : a.elems) elems.push_back(lower_expr(*x));
            Instr in;
            in.op = Op::MakeArray;
            in.dst = f_.new_temp(Kind::Ref);
            in.args = std::move(elems);
            in.elem = kind_of(e.type->elem);
            in.loc = loc;
            return emit(std::move(in)).dst;
        }
        case ExprKind::ArrayRepeat: {
            auto& r = static_cast<ArrayRepeat&>(e);
            TempId v = lower_expr(*r.value);
            TempId n = lower_expr(*r.count);
            Instr in;
            in.op = Op::ArrayRepeat;
            in.dst = f_.new_temp(Kind::Ref);
            in.args = {v, n};
            in.elem = kind_of(e.type->elem);
            in.loc = loc;
            return emit(std::move(in)).dst;
        }
        case ExprKind::StructLit: {
            auto& s = static_cast<StructLit&>(e);
            // Evaluate in source order, then pass in declaration order.
            std::vector<TempId> by_decl(s.decl->fields.size(), kNoTemp);
            for (auto& fi : s.fields) {
                TempId v = lower_expr(*fi.value);
                for (size_t k = 0; k < s.decl->fields.size(); ++k)
                    if (s.decl->fields[k].name == fi.name) by_decl[k] = v;
            }
            return emit_value(Op::MakeStruct, Kind::Ref, by_decl, loc, s.decl->index);
        }
        case ExprKind::EnumCtor: {
            auto& c = static_cast<EnumCtor&>(e);
            std::vector<TempId> args;
            for (auto& a : c.args) args.push_back(lower_expr(*a));
            return emit_value(Op::MakeEnum, Kind::Ref, std::move(args), loc, c.decl->index, c.variant_index);
        }
        case ExprKind::Lambda: return lower_closure(*static_cast<LambdaExpr&>(e).fn, loc);
        case ExprKind::Match: return lower_match(static_cast<MatchExpr&>(e));
    }
    throw std::logic_error("lowering: unknown expression");
}

// ------------------------------------------------------------ module

void ModuleLowerer::lower_function(FnInfo* info) {
    Function& f = mod.functions[static_cast<size_t>(info->index)];
    FnLowerer fl(*this, info, f);
    if (info->decl) {
        fl.lower_body(*info->decl);
    } else {
        fl.lower_globals_init();
    }
}

void ModuleLowerer::run() {
    for (auto* s : sema.structs) {
        StructInfo si;
        si.name = s->name;
        for (auto& f : s->fields) {
            si.field_names.push_back(f.name);
            si.field_kinds.push_back(kind_of(f.resolved));
        }
        mod.structs.push_back(std::move(si));
    }
    for (auto* e : sema.enums) {
        EnumInfo ei;
        ei.name = e->name;
        for (auto& v : e->variants) {
            VariantInfo vi;
            vi.name = v.name;
            for (Type* t : v.resolved) vi.field_kinds.push_back(kind_of(t));
            ei.variants.push_back(std::move(vi));
        }
        mod.enums.push_back(std::move(ei));
    }
    for (auto* g : sema.globals) mod.globals.push_back(GlobalInfo{g->name, kind_of(g->type)});
    mod.functions.resize(sema.functions.size());
    for (auto& fi : sema.functions) {
        if (!fi->decl) mod.init_function = static_cast<uint32_t>(fi->index);
        lower_function(fi.get());
    }
    mod.main_function = static_cast<uint32_t>(sema.main->index);
}

}  // namespace

Module lower_program(Program& prog, SemaResult& sema, LowerOptions opts) {
    ModuleLowerer ml(prog, sema, opts);
    ml.run();
    return std::move(ml.mod);
}

}  // namespace ember::ir

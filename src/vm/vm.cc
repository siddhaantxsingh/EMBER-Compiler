#include "vm/vm.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <new>

#include "bc/opcodes.h"
#include "bc/verify.h"
#include "common/natives.h"
#include "vm/natives_rt.h"

// Computed-goto dispatch is used with GCC and Clang unless disabled with
// -DEMBER_COMPUTED_GOTO=0 (it is a language extension; the switch loop is the
// portable fallback and is also tested in CI).
#ifndef EMBER_COMPUTED_GOTO
#if defined(__GNUC__) || defined(__clang__)
#define EMBER_COMPUTED_GOTO 1
#else
#define EMBER_COMPUTED_GOTO 0
#endif
#endif

#if EMBER_COMPUTED_GOTO
#pragma GCC diagnostic ignored "-Wpedantic"  // labels as values / computed goto
#endif

namespace ember::vm {

using bc::Opcode;
using bc::read_operand;

const char* error_kind_name(ErrorKind k) {
    switch (k) {
        case ErrorKind::DivisionByZero: return "division by zero";
        case ErrorKind::IntegerOverflow: return "integer overflow";
        case ErrorKind::IndexOutOfBounds: return "index out of bounds";
        case ErrorKind::InvalidConversion: return "invalid conversion";
        case ErrorKind::EmptyArray: return "empty array";
        case ErrorKind::StackOverflow: return "stack overflow";
        case ErrorKind::OutOfMemory: return "out of memory";
        case ErrorKind::UninitializedGlobal: return "uninitialized global";
        case ErrorKind::UninitializedVariable: return "uninitialized variable";
        case ErrorKind::Panic: return "panic";
        case ErrorKind::AssertionFailed: return "assertion failed";
        case ErrorKind::InstructionLimit: return "instruction limit exceeded";
        case ErrorKind::ValueTooLarge: return "value too large";
        case ErrorKind::InvalidArgument: return "invalid argument";
        case ErrorKind::TypeMismatch: return "type mismatch";
        case ErrorKind::Internal: return "internal error";
    }
    return "error";
}

std::string RunResult::render(const std::string& source_name) const {
    if (ok) return "";
    std::string s = "RuntimeError: " + message + "\n";
    auto line = [&](const TraceEntry& t) {
        s += "  at " + t.function + " (" + source_name;
        if (t.line) s += ":" + std::to_string(t.line) + ":" + std::to_string(t.column);
        s += ")\n";
    };
    // Long traces (deep recursion) are abbreviated: innermost 10, outermost 5.
    if (trace.size() <= 20) {
        for (const auto& t : trace) line(t);
    } else {
        for (size_t i = 0; i < 10; ++i) line(trace[i]);
        s += "  ... " + std::to_string(trace.size() - 15) + " more frames ...\n";
        for (size_t i = trace.size() - 5; i < trace.size(); ++i) line(trace[i]);
    }
    return s;
}

void VM::fail(ErrorKind k, std::string msg) {
    throw Error{k, msg.empty() ? std::string(error_kind_name(k)) : std::move(msg)};
}

VM::VM(bc::Module module, VMOptions opts)
    : mod_(std::move(module)), opts_(opts), heap_(opts.max_heap_bytes, opts.gc_stress), out_(&std::cout), in_(&std::cin) {
    auto vr = bc::verify(mod_);
    if (!vr.ok()) {
        std::string msg = "bytecode verification failed:";
        for (const auto& e : vr.errors) msg += "\n  " + e;
        throw InvalidModule(msg);
    }
    if (opts_.max_stack_values < 16) opts_.max_stack_values = 16;
    stack_.reset(new Value[opts_.max_stack_values]);
    stack_end_ = stack_.get() + opts_.max_stack_values;
    sp_ = stack_.get();
    frames_.reserve(std::min<uint32_t>(opts_.max_call_depth, 1u << 16) + 1);
    globals_.assign(mod_.globals.size(), Value::uninit());
    heap_.set_root_marker([this](Heap& h) { mark_roots(h); });
    // String constants are allocated once and are permanent roots. They are
    // created before execution, so a failure here is reported by run().
    constants_.reserve(mod_.strings.size());
    // The counter is decremented before each instruction and traps at 0, so a
    // budget of N allows exactly N instructions.
    budget_left_ = opts_.instruction_budget ? opts_.instruction_budget + 1 : UINT64_MAX;
    budget_initial_ = budget_left_;
    counting_ = opts_.instruction_budget != 0 || opts_.count_instructions;
    for (size_t i = 0; i < mod_.functions.size(); ++i) {
        int idx = -1;
        bc::Signature s = mod_.functions[i].signature();
        for (size_t k = 0; k < mod_.signatures.size(); ++k)
            if (mod_.signatures[k] == s) {
                idx = static_cast<int>(k);
                break;
            }
        fn_sig_.push_back(idx);
    }
}

VM::~VM() = default;

void VM::mark_roots(Heap& h) {
    for (Value* v = stack_.get(); v < sp_; ++v) h.mark_value(*v);
    for (const Value& v : globals_) h.mark_value(v);
    for (StringObj* s : constants_) h.mark_object(s);
    for (const Value& v : temp_roots_) h.mark_value(v);
    for (const Frame& f : frames_) h.mark_object(f.closure);
    for (UpvalueObj* u = open_upvalues_; u; u = u->next_open) h.mark_object(u);
}

void VM::check_string_size(size_t n) const {
    if (n > opts_.max_string_bytes)
        fail(ErrorKind::ValueTooLarge, "value too large: string of " + std::to_string(n) + " bytes exceeds the limit of " +
                                           std::to_string(opts_.max_string_bytes));
}

void VM::check_array_size(size_t n) const {
    if (n > opts_.max_array_length)
        fail(ErrorKind::ValueTooLarge, "value too large: array of " + std::to_string(n) +
                                           " elements exceeds the limit of " + std::to_string(opts_.max_array_length));
}

StringObj* VM::new_string(std::string s) {
    check_string_size(s.size());
    size_t payload = s.capacity();
    StringObj* o = heap_.alloc<StringObj>(payload);
    o->s = std::move(s);
    return o;
}

ArrayObj* VM::new_array(Kind elem, size_t reserve_items) {
    check_array_size(reserve_items);
    ArrayObj* a = heap_.alloc<ArrayObj>(reserve_items * sizeof(Value), elem);
    a->items.reserve(reserve_items);
    return a;
}

FrameView VM::frame(size_t depth) const {
    const Frame& f = frames_[frames_.size() - 1 - depth];
    return FrameView{f.fn, f.fn_index, static_cast<uint32_t>(f.inst - f.fn->code.data()), f.base, f.closure};
}

void VM::push_frame(uint32_t fn_index, Value* base, Value* ret_slot, ClosureObj* closure) {
    const bc::Function& fn = mod_.functions[fn_index];
    if (frames_.size() >= opts_.max_call_depth)
        fail(ErrorKind::StackOverflow, "stack overflow (call depth limit " + std::to_string(opts_.max_call_depth) + ")");
    size_t nlocals = fn.local_kinds.size();
    if (static_cast<size_t>(stack_end_ - base) < nlocals + fn.max_stack)
        fail(ErrorKind::StackOverflow, "stack overflow (operand stack limit " + std::to_string(opts_.max_stack_values) + " values)");
    for (size_t i = fn.num_params; i < nlocals; ++i) base[i] = Value::default_for(fn.local_kinds[i]);
    sp_ = base + nlocals;
    const uint8_t* code = fn.code.data();
    frames_.push_back(Frame{&fn, fn_index, code, code, base, ret_slot, closure});
}

UpvalueObj* VM::capture_upvalue(Value* slot) {
    UpvalueObj* prev = nullptr;
    UpvalueObj* u = open_upvalues_;
    while (u && u->loc > slot) {
        prev = u;
        u = u->next_open;
    }
    if (u && u->loc == slot) return u;
    UpvalueObj* created = heap_.alloc<UpvalueObj>(0, slot);
    // The allocation may have collected, but the open list itself only
    // changes here, so recompute the insertion point to be safe.
    prev = nullptr;
    u = open_upvalues_;
    while (u && u->loc > slot) {
        prev = u;
        u = u->next_open;
    }
    created->next_open = u;
    if (prev) prev->next_open = created;
    else open_upvalues_ = created;
    return created;
}

void VM::close_upvalues(Value* from) {
    while (open_upvalues_ && open_upvalues_->loc >= from) {
        UpvalueObj* u = open_upvalues_;
        u->closed = *u->loc;
        u->loc = &u->closed;
        open_upvalues_ = u->next_open;
        u->next_open = nullptr;
    }
}

namespace {

inline StringObj* as_string(const Value& v) {
    if (v.tag != Tag::Ref || v.o->type != ObjType::String) VM::fail(ErrorKind::TypeMismatch, "type mismatch: expected a string");
    return static_cast<StringObj*>(v.o);
}

inline ArrayObj* as_array(const Value& v, Kind elem) {
    if (v.tag != Tag::Ref || v.o->type != ObjType::Array || static_cast<ArrayObj*>(v.o)->elem != elem)
        VM::fail(ErrorKind::TypeMismatch, "type mismatch: expected an array");
    return static_cast<ArrayObj*>(v.o);
}

inline ArrayObj* as_any_array(const Value& v) {
    if (v.tag != Tag::Ref || v.o->type != ObjType::Array) VM::fail(ErrorKind::TypeMismatch, "type mismatch: expected an array");
    return static_cast<ArrayObj*>(v.o);
}

inline StructObj* as_struct(const Value& v, uint16_t sid) {
    if (v.tag != Tag::Ref || v.o->type != ObjType::Struct || static_cast<StructObj*>(v.o)->sid != sid)
        VM::fail(ErrorKind::TypeMismatch, "type mismatch: expected a struct");
    return static_cast<StructObj*>(v.o);
}

inline EnumObj* as_enum(const Value& v, uint16_t eid) {
    if (v.tag != Tag::Ref || v.o->type != ObjType::Enum || static_cast<EnumObj*>(v.o)->eid != eid)
        VM::fail(ErrorKind::TypeMismatch, "type mismatch: expected an enum");
    return static_cast<EnumObj*>(v.o);
}

inline ClosureObj* as_closure(const Value& v) {
    if (v.tag != Tag::Ref || v.o->type != ObjType::Closure) VM::fail(ErrorKind::TypeMismatch, "type mismatch: expected a function");
    return static_cast<ClosureObj*>(v.o);
}

[[noreturn]] void index_error(int64_t i, size_t len) {
    VM::fail(ErrorKind::IndexOutOfBounds,
             "index out of bounds: index " + std::to_string(i) + " for length " + std::to_string(len));
}

int compare_strings(const Value& a, const Value& b) { return as_string(a)->s.compare(as_string(b)->s); }

}  // namespace

// kDebug: call the debug hook before each instruction.
// kCount: count instructions (instruction budget or --stats). Counting costs
// up to ~15% on call- and loop-heavy code (measured, docs/OPTIMIZATION.md),
// so the default loop does not do it.
template <bool kDebug, bool kCount>
void VM::execute() {
    Frame* frame = &frames_.back();
    const uint8_t* code = frame->fn->code.data();
    const uint8_t* ip = frame->ip;
    const uint8_t* inst = ip;
    Value* base = frame->base;
    Value* sp = sp_;
    uint64_t budget = budget_left_;

#define READ_U8() (ip += 1, ip[-1])
#define READ_U16() (ip += 2, read_operand<uint16_t>(ip - 2))
#define READ_U32() (ip += 4, read_operand<uint32_t>(ip - 4))
#define SYNC() (sp_ = sp)
#define RELOAD()                       \
    do {                               \
        frame = &frames_.back();       \
        code = frame->fn->code.data(); \
        ip = frame->ip;                \
        base = frame->base;            \
        sp = sp_;                      \
    } while (0)

// Per-instruction prologue: remember the instruction start (for traces),
// charge the instruction budget, and call the debugger hook if attached.
#define PROLOGUE()                                              \
    do {                                                        \
        inst = ip;                                              \
        if constexpr (kCount) {                                 \
            if (--budget == 0) fail(ErrorKind::InstructionLimit); \
        }                                                       \
        if constexpr (kDebug) {                                 \
            frame->inst = inst;                                 \
            frame->ip = ip;                                     \
            sp_ = sp;                                           \
            budget_left_ = budget;                              \
            hook_->before_instruction(*this);                   \
        }                                                       \
    } while (0)

#if EMBER_COMPUTED_GOTO
    // Direct threading: each handler jumps straight to the next handler
    // through a label table (GCC/Clang extension), instead of returning to
    // a single switch. See docs/OPTIMIZATION.md for the measurement.
    static const void* const kLabels[] = {
#define X(n, f) &&L_##n,
        EMBER_OPCODES(X)
#undef X
        &&L_COUNT_,
    };
#define CASE(op) L_##op:
#define NEXT()                       \
    do {                             \
        PROLOGUE();                  \
        goto* kLabels[*ip++];        \
    } while (0)
#else
#define CASE(op) case Opcode::op:
#define NEXT() break
#endif

    try {
#if EMBER_COMPUTED_GOTO
        NEXT();
        {
#else
        for (;;) {
            PROLOGUE();
            switch (static_cast<Opcode>(READ_U8())) {
#endif
                CASE(NOP) NEXT();
                CASE(POP) --sp; NEXT();
                CASE(PUSH_INT) *sp++ = Value::integer(read_operand<int64_t>(ip)); ip += 8; NEXT();
                CASE(PUSH_FLOAT) *sp++ = Value::flt(read_operand<double>(ip)); ip += 8; NEXT();
                CASE(PUSH_TRUE) *sp++ = Value::boolean(true); NEXT();
                CASE(PUSH_FALSE) *sp++ = Value::boolean(false); NEXT();
                CASE(PUSH_UNIT) *sp++ = Value::unit(); NEXT();
                CASE(PUSH_STR) *sp++ = Value::ref(constants_[READ_U32()]); NEXT();
                CASE(LOAD_LOCAL) *sp++ = base[READ_U16()]; NEXT();
                CASE(STORE_LOCAL) base[READ_U16()] = *--sp; NEXT();
                CASE(CLEAR_LOCAL) base[READ_U16()] = Value::uninit(); NEXT();
                CASE(LOAD_UPVAL) {
                    Value v = *frame->closure->upvalues[READ_U16()]->loc;
                    if (v.tag == Tag::Uninit)
                        fail(ErrorKind::UninitializedVariable,
                             "uninitialized variable: '" + frame->fn->upvalues[read_operand<uint16_t>(ip - 2)].name +
                                 "' was used before its declaration ran");
                    *sp++ = v;
                    NEXT();
                }
                CASE(STORE_UPVAL) *frame->closure->upvalues[READ_U16()]->loc = *--sp; NEXT();
                CASE(LOAD_GLOBAL) {
                    uint16_t g = READ_U16();
                    Value v = globals_[g];
                    if (v.tag == Tag::Uninit)
                        fail(ErrorKind::UninitializedGlobal,
                             "uninitialized global: '" + mod_.globals[g].name + "' was read before its initializer ran");
                    *sp++ = v;
                    NEXT();
                }
                CASE(STORE_GLOBAL) globals_[READ_U16()] = *--sp; NEXT();
                CASE(IADD) {
                    int64_t r;
                    if (__builtin_add_overflow(sp[-2].i, sp[-1].i, &r)) fail(ErrorKind::IntegerOverflow);
                    sp[-2] = Value::integer(r);
                    --sp;
                    NEXT();
                }
                CASE(ISUB) {
                    int64_t r;
                    if (__builtin_sub_overflow(sp[-2].i, sp[-1].i, &r)) fail(ErrorKind::IntegerOverflow);
                    sp[-2] = Value::integer(r);
                    --sp;
                    NEXT();
                }
                CASE(IMUL) {
                    int64_t r;
                    if (__builtin_mul_overflow(sp[-2].i, sp[-1].i, &r)) fail(ErrorKind::IntegerOverflow);
                    sp[-2] = Value::integer(r);
                    --sp;
                    NEXT();
                }
                CASE(IDIV) {
                    int64_t a = sp[-2].i, b = sp[-1].i;
                    if (b == 0) fail(ErrorKind::DivisionByZero);
                    if (a == INT64_MIN && b == -1) fail(ErrorKind::IntegerOverflow);
                    sp[-2] = Value::integer(a / b);
                    --sp;
                    NEXT();
                }
                CASE(IMOD) {
                    int64_t a = sp[-2].i, b = sp[-1].i;
                    if (b == 0) fail(ErrorKind::DivisionByZero);
                    sp[-2] = Value::integer(b == -1 ? 0 : a % b);
                    --sp;
                    NEXT();
                }
                CASE(INEG)
                    if (sp[-1].i == INT64_MIN) fail(ErrorKind::IntegerOverflow);
                    sp[-1] = Value::integer(-sp[-1].i);
                    NEXT();
                CASE(FADD) sp[-2] = Value::flt(sp[-2].f + sp[-1].f); --sp; NEXT();
                CASE(FSUB) sp[-2] = Value::flt(sp[-2].f - sp[-1].f); --sp; NEXT();
                CASE(FMUL) sp[-2] = Value::flt(sp[-2].f * sp[-1].f); --sp; NEXT();
                CASE(FDIV) sp[-2] = Value::flt(sp[-2].f / sp[-1].f); --sp; NEXT();
                CASE(FNEG) sp[-1] = Value::flt(-sp[-1].f); NEXT();
                CASE(NOT) sp[-1] = Value::boolean(!sp[-1].b); NEXT();
                CASE(I2F) sp[-1] = Value::flt(static_cast<double>(sp[-1].i)); NEXT();
                CASE(F2I) {
                    double t = std::trunc(sp[-1].f);
                    if (!(t >= -9223372036854775808.0 && t < 9223372036854775808.0))
                        fail(ErrorKind::InvalidConversion, "invalid conversion: " + format_value(sp[-1]) + " does not fit in Int");
                    sp[-1] = Value::integer(static_cast<int64_t>(t));
                    NEXT();
                }
#define CMP(OPC, FIELD, EXPR)                                \
    CASE(OPC) {                                      \
        auto a = sp[-2].FIELD;                               \
        auto b = sp[-1].FIELD;                               \
        sp[-2] = Value::boolean(EXPR);                       \
        --sp;                                                \
        NEXT();                                               \
    }
                CMP(IEQ, i, a == b)
                CMP(INE, i, a != b)
                CMP(ILT, i, a < b)
                CMP(ILE, i, a <= b)
                CMP(IGT, i, a > b)
                CMP(IGE, i, a >= b)
                CMP(FEQ, f, a == b)
                CMP(FNE, f, a != b)
                CMP(FLT, f, a < b)
                CMP(FLE, f, a <= b)
                CMP(FGT, f, a > b)
                CMP(FGE, f, a >= b)
                CMP(BEQ, b, a == b)
                CMP(BNE, b, a != b)
#undef CMP
#define SCMP(OPC, EXPR)                                      \
    CASE(OPC) {                                      \
        int c = compare_strings(sp[-2], sp[-1]);             \
        sp[-2] = Value::boolean(EXPR);                       \
        --sp;                                                \
        NEXT();                                               \
    }
                SCMP(SEQ, c == 0)
                SCMP(SNE, c != 0)
                SCMP(SLT, c < 0)
                SCMP(SLE, c <= 0)
                SCMP(SGT, c > 0)
                SCMP(SGE, c >= 0)
#undef SCMP
                CASE(SCONCAT) {
                    StringObj* s;
                    {
                        // Inner scope: the std::string must be destroyed
                        // before the computed goto in NEXT() leaves it.
                        const std::string& a = as_string(sp[-2])->s;
                        const std::string& b = as_string(sp[-1])->s;
                        check_string_size(a.size() + b.size());
                        std::string r;
                        r.reserve(a.size() + b.size());
                        r.append(a).append(b);
                        SYNC();
                        s = new_string(std::move(r));
                    }
                    sp[-2] = Value::ref(s);
                    --sp;
                    NEXT();
                }
                CASE(SLEN) sp[-1] = Value::integer(static_cast<int64_t>(as_string(sp[-1])->s.size())); NEXT();
                CASE(SINDEX) {
                    const std::string& s = as_string(sp[-2])->s;
                    int64_t i = sp[-1].i;
                    if (i < 0 || static_cast<uint64_t>(i) >= s.size()) index_error(i, s.size());
                    SYNC();
                    StringObj* r = new_string(std::string(1, s[static_cast<size_t>(i)]));
                    sp[-2] = Value::ref(r);
                    --sp;
                    NEXT();
                }
                CASE(CALL) {
                    uint32_t fi = READ_U32();
                    frame->ip = ip;
                    frame->inst = inst;
                    Value* nbase = sp - mod_.functions[fi].num_params;
                    SYNC();
                    push_frame(fi, nbase, nbase, nullptr);
                    RELOAD();
                    NEXT();
                }
                CASE(CALL_INDIRECT) {
                    uint16_t si = READ_U16();
                    size_t n = mod_.signatures[si].params.size();
                    ClosureObj* c = as_closure(sp[-static_cast<ptrdiff_t>(n) - 1]);
                    if (fn_sig_[c->fn] != si && !(mod_.functions[c->fn].signature() == mod_.signatures[si]))
                        fail(ErrorKind::TypeMismatch, "type mismatch: function called with the wrong signature");
                    frame->ip = ip;
                    frame->inst = inst;
                    Value* nbase = sp - n;
                    SYNC();
                    push_frame(c->fn, nbase, nbase - 1, c);
                    RELOAD();
                    NEXT();
                }
                CASE(CALL_NATIVE) {
                    uint16_t id = READ_U16();
                    size_t n = natives()[id].params.size();
                    frame->ip = ip;
                    frame->inst = inst;
                    SYNC();
                    Value* args = sp - n;
                    Value r = call_native(*this, id, args);
                    sp = args;
                    *sp++ = r;
                    NEXT();
                }
                CASE(RETURN) {
                    Value result = sp[-1];
                    close_upvalues(base);
                    Value* ret = frame->ret_slot;
                    frames_.pop_back();
                    *ret = result;
                    sp = ret + 1;
                    if (frames_.empty()) {
                        sp_ = sp;
                        budget_left_ = budget;
                        return;
                    }
                    sp_ = sp;
                    RELOAD();
                    NEXT();
                }
                CASE(CLOSURE) {
                    uint32_t fi = READ_U32();
                    const bc::Function& child = mod_.functions[fi];
                    SYNC();
                    ClosureObj* c = heap_.alloc<ClosureObj>(child.upvalues.size() * sizeof(void*), fi);
                    *sp++ = Value::ref(c);  // rooted before capturing (which allocates)
                    SYNC();
                    c->upvalues.reserve(child.upvalues.size());
                    for (const auto& u : child.upvalues) {
                        if (u.from_local) c->upvalues.push_back(capture_upvalue(base + u.index));
                        else c->upvalues.push_back(frame->closure->upvalues[u.index]);
                    }
                    NEXT();
                }
                CASE(CLOSE_UPVALUES) close_upvalues(base + READ_U16()); NEXT();
                CASE(MAKE_STRUCT) {
                    uint16_t sid = READ_U16();
                    size_t n = mod_.structs[sid].field_kinds.size();
                    SYNC();
                    StructObj* o = heap_.alloc<StructObj>(n * sizeof(Value), sid);
                    o->fields.assign(sp - n, sp);
                    sp -= n;
                    *sp++ = Value::ref(o);
                    NEXT();
                }
                CASE(GET_FIELD) {
                    uint16_t sid = READ_U16(), fi = READ_U16();
                    sp[-1] = as_struct(sp[-1], sid)->fields[fi];
                    NEXT();
                }
                CASE(SET_FIELD) {
                    uint16_t sid = READ_U16(), fi = READ_U16();
                    as_struct(sp[-2], sid)->fields[fi] = sp[-1];
                    sp -= 2;
                    NEXT();
                }
                CASE(MAKE_ARRAY) {
                    Kind k = static_cast<Kind>(READ_U8());
                    uint32_t n = READ_U32();
                    SYNC();
                    ArrayObj* a = new_array(k, n);
                    a->items.assign(sp - n, sp);
                    sp -= n;
                    *sp++ = Value::ref(a);
                    NEXT();
                }
                CASE(ARRAY_REPEAT) {
                    Kind k = static_cast<Kind>(READ_U8());
                    int64_t n = sp[-1].i;
                    if (n < 0) fail(ErrorKind::InvalidArgument, "invalid argument: negative array length " + std::to_string(n));
                    check_array_size(static_cast<size_t>(n));
                    SYNC();
                    ArrayObj* a = new_array(k, static_cast<size_t>(n));
                    a->items.assign(static_cast<size_t>(n), sp[-2]);
                    sp[-2] = Value::ref(a);
                    --sp;
                    NEXT();
                }
                CASE(ARRAY_GET) {
                    ArrayObj* a = as_array(sp[-2], static_cast<Kind>(READ_U8()));
                    int64_t i = sp[-1].i;
                    if (i < 0 || static_cast<uint64_t>(i) >= a->items.size()) index_error(i, a->items.size());
                    sp[-2] = a->items[static_cast<size_t>(i)];
                    --sp;
                    NEXT();
                }
                CASE(ARRAY_SET) {
                    ArrayObj* a = as_array(sp[-3], static_cast<Kind>(READ_U8()));
                    int64_t i = sp[-2].i;
                    if (i < 0 || static_cast<uint64_t>(i) >= a->items.size()) index_error(i, a->items.size());
                    a->items[static_cast<size_t>(i)] = sp[-1];
                    sp -= 3;
                    NEXT();
                }
                CASE(ARRAY_LEN) sp[-1] = Value::integer(static_cast<int64_t>(as_any_array(sp[-1])->items.size())); NEXT();
                CASE(ARRAY_PUSH) {
                    ArrayObj* a = as_array(sp[-2], static_cast<Kind>(READ_U8()));
                    check_array_size(a->items.size() + 1);
                    SYNC();
                    heap_.grow(a, sizeof(Value));
                    a->items.push_back(sp[-1]);
                    sp[-2] = Value::unit();
                    --sp;
                    NEXT();
                }
                CASE(ARRAY_POP) {
                    ArrayObj* a = as_array(sp[-1], static_cast<Kind>(READ_U8()));
                    if (a->items.empty()) fail(ErrorKind::EmptyArray, "empty array: pop from an empty array");
                    Value v = a->items.back();
                    a->items.pop_back();
                    heap_.shrink(a, sizeof(Value));
                    sp[-1] = v;
                    NEXT();
                }
                CASE(MAKE_ENUM) {
                    uint16_t eid = READ_U16(), vi = READ_U16();
                    size_t n = mod_.enums[eid].variants[vi].field_kinds.size();
                    SYNC();
                    EnumObj* e = heap_.alloc<EnumObj>(n * sizeof(Value), eid, vi);
                    e->fields.assign(sp - n, sp);
                    sp -= n;
                    *sp++ = Value::ref(e);
                    NEXT();
                }
                CASE(ENUM_TAG) {
                    uint16_t eid = READ_U16();
                    sp[-1] = Value::integer(as_enum(sp[-1], eid)->variant);
                    NEXT();
                }
                CASE(ENUM_FIELD) {
                    uint16_t eid = READ_U16(), vi = READ_U16(), fi = READ_U16();
                    EnumObj* e = as_enum(sp[-1], eid);
                    if (e->variant != vi) fail(ErrorKind::TypeMismatch, "type mismatch: wrong enum variant");
                    sp[-1] = e->fields[fi];
                    NEXT();
                }
                CASE(JUMP) ip = code + read_operand<uint32_t>(ip); NEXT();
                CASE(JUMP_IF_FALSE) {
                    uint32_t t = READ_U32();
                    if (!(--sp)->b) {
                        ip = code + t;
                        NEXT();
                    }
                    NEXT();
                }
                CASE(JUMP_IF_TRUE) {
                    uint32_t t = READ_U32();
                    if ((--sp)->b) {
                        ip = code + t;
                        NEXT();
                    }
                    NEXT();
                }
                CASE(UNREACHABLE) fail(ErrorKind::Internal, "internal error: reached unreachable code");
                CASE(LOAD_LOCAL2) {
                    uint16_t a = READ_U16(), b = READ_U16();
                    sp[0] = base[a];
                    sp[1] = base[b];
                    sp += 2;
                    NEXT();
                }
                CASE(STORE_LOAD) {
                    uint16_t a = READ_U16(), b = READ_U16();
                    base[a] = sp[-1];
                    sp[-1] = base[b];
                    NEXT();
                }
// Separate dispatch on each path: with a single "ip = cond ? target : ip"
// the compiler may emit a conditional move, which makes the next indirect
// jump wait for the comparison instead of being predicted (measured: +30%
// on loop-heavy code).
#define IJUMP(OPC, EXPR)                         \
    CASE(OPC) {                                  \
        uint32_t t = READ_U32();                 \
        int64_t a = sp[-2].i, b = sp[-1].i;      \
        sp -= 2;                                 \
        if (EXPR) {                              \
            ip = code + t;                       \
            NEXT();                              \
        }                                        \
        NEXT();                                  \
    }
                IJUMP(IJEQ, a == b)
                IJUMP(IJNE, a != b)
                IJUMP(IJLT, a < b)
                IJUMP(IJLE, a <= b)
                IJUMP(IJGT, a > b)
                IJUMP(IJGE, a >= b)
#undef IJUMP
                CASE(COUNT_) fail(ErrorKind::Internal, "internal error: invalid opcode");
#if !EMBER_COMPUTED_GOTO
            }
#endif
        }
    } catch (...) {
        if (!frames_.empty()) {
            frame = &frames_.back();
            if (frame->fn->code.data() == code) frame->inst = inst;
        }
        sp_ = sp;
        budget_left_ = budget;
        throw;
    }
#undef READ_U8
#undef READ_U16
#undef READ_U32
#undef SYNC
#undef RELOAD
#undef PROLOGUE
#undef CASE
#undef NEXT
}

void VM::call_entry(uint32_t fn_index) {
    push_frame(fn_index, sp_, sp_, nullptr);
    if (hook_) execute<true, true>();
    else if (counting_) execute<false, true>();
    else execute<false, false>();
    --sp_;  // discard the Unit result
}

RunResult VM::make_error(ErrorKind k, const std::string& msg) {
    RunResult r;
    r.ok = false;
    r.kind = k;
    r.message = msg;
    for (size_t i = frames_.size(); i-- > 0;) {
        const Frame& f = frames_[i];
        bc::LineEntry loc = f.fn->location(static_cast<uint32_t>(f.inst - f.fn->code.data()));
        r.trace.push_back(TraceEntry{f.fn->name, loc.line, loc.column});
    }
    frames_.clear();
    return r;
}

RunResult VM::run() {
    if (ran_) throw std::logic_error("VM::run called twice");
    ran_ = true;
    start_time_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    RunResult result;
    try {
        for (const auto& s : mod_.strings) {
            StringObj* o = new_string(s);
            constants_.push_back(o);
        }
        call_entry(mod_.init_function);
        call_entry(mod_.main_function);
    } catch (const Error& e) {
        result = make_error(e.kind, e.message);
    } catch (const OutOfMemory&) {
        result = make_error(ErrorKind::OutOfMemory, "out of memory (heap limit " + std::to_string(opts_.max_heap_bytes) + " bytes)");
    } catch (const std::bad_alloc&) {
        result = make_error(ErrorKind::OutOfMemory, "out of memory (host allocation failed)");
    }
    executed_ = budget_initial_ - budget_left_;
    out_->flush();
    return result;
}

}  // namespace ember::vm

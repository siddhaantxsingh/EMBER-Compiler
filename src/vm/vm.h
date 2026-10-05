// The Ember virtual machine: executes a verified bytecode module.
//
// The VM depends only on src/bc (module, opcodes, verifier) and src/common;
// it never sees source code, the AST or the IR. Constructing a VM verifies
// the module, so unverified bytecode can never be executed.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "bc/module.h"
#include "vm/heap.h"
#include "vm/value.h"

namespace ember::vm {

struct VMOptions {
    uint32_t max_call_depth = 10000;
    uint32_t max_stack_values = 1u << 20;
    size_t max_heap_bytes = 256u * 1024 * 1024;
    size_t max_string_bytes = 64u * 1024 * 1024;
    size_t max_array_length = 16u * 1024 * 1024;
    uint64_t instruction_budget = 0;  // 0 = unlimited
    bool gc_stress = false;           // collect before every allocation
    bool count_instructions = false;  // keep an exact instruction count (always on with a budget or a debugger)
};

// Runtime error categories (the text after "RuntimeError: ").
enum class ErrorKind {
    DivisionByZero,
    IntegerOverflow,
    IndexOutOfBounds,
    InvalidConversion,
    EmptyArray,
    StackOverflow,
    OutOfMemory,
    UninitializedGlobal,
    UninitializedVariable,
    Panic,
    AssertionFailed,
    InstructionLimit,
    ValueTooLarge,
    InvalidArgument,
    TypeMismatch,  // only reachable with hand-crafted (verified but ill-typed) bytecode
    Internal,
};
const char* error_kind_name(ErrorKind k);

struct TraceEntry {
    std::string function;
    uint32_t line = 0;
    uint32_t column = 0;
};

struct RunResult {
    bool ok = true;
    ErrorKind kind = ErrorKind::Internal;
    std::string message;              // e.g. "division by zero" or "panic: msg"
    std::vector<TraceEntry> trace;    // innermost first
    std::string render(const std::string& source_name) const;
};

struct InvalidModule : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class VM;

// Debugger interface: called before each instruction when attached.
class DebugHook {
public:
    virtual ~DebugHook() = default;
    virtual void before_instruction(VM& vm) = 0;
};

struct FrameView {
    const bc::Function* fn;
    uint32_t function_index;
    uint32_t pc;  // start of the current instruction
    const Value* locals;
    const ClosureObj* closure;
};

class VM {
public:
    // The VM keeps its own copy of the module (no lifetime coupling with the caller).
    VM(bc::Module module, VMOptions opts = {});
    ~VM();
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    void set_output(std::ostream* out) { out_ = out; }
    void set_input(std::istream* in) { in_ = in; }
    void set_debug_hook(DebugHook* h) { hook_ = h; }

    // Run the global initializers, then main. Can be called once.
    RunResult run();

    // Introspection (valid inside a DebugHook callback).
    size_t frame_count() const { return frames_.size(); }
    FrameView frame(size_t depth_from_top) const;
    const bc::Module& module() const { return mod_; }
    const Value& global(size_t i) const { return globals_[i]; }
    std::string format_value(const Value& v) const;
    const HeapStats& heap_stats() const { return heap_.stats(); }
    // Exact count when counting was enabled (budget, count_instructions or a
    // debug hook), otherwise 0.
    uint64_t instructions_executed() const { return executed_; }

    // Used by native functions.
    struct Error {
        ErrorKind kind;
        std::string message;
    };
    [[noreturn]] static void fail(ErrorKind k, std::string msg = "");
    StringObj* new_string(std::string s);
    ArrayObj* new_array(Kind elem, size_t reserve_items);
    void push_temp_root(Value v) { temp_roots_.push_back(v); }
    void pop_temp_root() { temp_roots_.pop_back(); }
    std::ostream& out() { return *out_; }
    std::istream& in() { return *in_; }
    void check_string_size(size_t n) const;
    void check_array_size(size_t n) const;
    double start_time() const { return start_time_; }
    size_t max_string_bytes() const { return opts_.max_string_bytes; }

private:
    struct Frame {
        const bc::Function* fn;
        uint32_t fn_index;
        const uint8_t* ip;       // next instruction (saved across calls)
        const uint8_t* inst;     // start of the current instruction
        Value* base;             // first local
        Value* ret_slot;         // where the result goes on return
        ClosureObj* closure;     // null for direct calls
    };

    template <bool kDebug, bool kCount>
    void execute();
    void call_entry(uint32_t fn_index);
    void push_frame(uint32_t fn_index, Value* base, Value* ret_slot, ClosureObj* closure);
    UpvalueObj* capture_upvalue(Value* slot);
    void close_upvalues(Value* from);
    void mark_roots(Heap& h);
    void format_into(std::string& out, const Value& v, int depth) const;
    RunResult make_error(ErrorKind k, const std::string& msg);

    const bc::Module mod_;
    VMOptions opts_;
    Heap heap_;
    std::unique_ptr<Value[]> stack_;
    Value* stack_end_;
    Value* sp_;
    std::vector<Frame> frames_;
    std::vector<Value> globals_;
    std::vector<StringObj*> constants_;
    std::vector<Value> temp_roots_;
    std::vector<int> fn_sig_;        // function -> index in module signatures (or -1)
    UpvalueObj* open_upvalues_ = nullptr;
    std::ostream* out_;
    std::istream* in_;
    DebugHook* hook_ = nullptr;
    uint64_t budget_left_;
    uint64_t budget_initial_ = 0;
    bool counting_ = false;
    uint64_t executed_ = 0;
    double start_time_ = 0;
    bool ran_ = false;
};

}  // namespace ember::vm

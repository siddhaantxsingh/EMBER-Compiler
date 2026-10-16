// Runtime support for code produced by the native (LLVM) backend.
//
// The generated code calls these extern "C" functions for everything that
// is not plain arithmetic: printing, strings, traps. The functions keep the
// exact observable behaviour of the VM (same output text, same runtime
// error messages, same stack-trace format), which the differential tests
// check.
//
// Strings and arrays are allocated from an arena that is freed when the run ends.
// There is no garbage collector in the native backend; a run that allocates
// more than the arena limit stops with a "native backend limit" error
// (a distinct exit status, never reported as a program error).
//
// The runtime is single-threaded and not reentrant: one native run at a time
// per process.
#pragma once

#include <csetjmp>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace ember::native {

// Heap objects share a header word: tag 0 = string, tag 1 = array (with
// the element Kind in bits 8..15). Layouts are shared with generated code.
//   string: { i64 tag, i64 len, [len+1 x i8] }
//   array:  { i64 tag, i64 len, i64 cap, ptr data }  (8-byte element slots)
//   struct: { i64 tag = 2 | sid << 8, slots... }
//   enum:   { i64 tag = 3 | eid << 8, i64 variant, slots... }
//   closure: { i64 tag = 4 | fn << 8, ptr code, ptr cells... }
//   cell (captured variable): { i64 bits, i64 initialized }
enum : int64_t { kTagString = 0, kTagArray = 1, kTagStruct = 2, kTagEnum = 3, kTagClosure = 4 };

struct EmStr {
    int64_t tag;
    int64_t len;
    char data[1];  // really len + 1 bytes (NUL-terminated for convenience)
};

// Element slots hold Int as i64, Float as the bits of a double, Bool and
// Unit as 0/1, references as pointers.
struct EmArr {
    int64_t tag;
    int64_t len;
    int64_t cap;
    int64_t* data;
};

// Error kinds the generated code can raise directly. The numbering is
// private to the backend; messages match vm::error_kind_name.
enum TrapKind : int32_t {
    kTrapDivZero = 0,
    kTrapOverflow = 1,
    kTrapUnreachable = 2,
    kTrapUninitGlobal = 3,  // arg = global index
    kTrapStackOverflow = 4,
    kTrapWrongVariant = 5,
};

struct RtStructInfo {
    std::string name;
    std::vector<std::string> field_names;
    std::vector<int64_t> field_kinds;  // Kind values
};

struct RtEnumInfo {
    std::string name;
    std::vector<std::string> variant_names;
    std::vector<std::vector<int64_t>> field_kinds;
};

struct RtTraceEntry {
    std::string function;
    uint32_t line = 0;
    uint32_t column = 0;
};

struct RtConfig {
    std::ostream* out = nullptr;
    std::istream* in = nullptr;
    uint32_t max_call_depth = 10000;
    size_t max_string_bytes = 64u * 1024 * 1024;
    size_t max_array_length = 16u * 1024 * 1024;
    size_t arena_limit_bytes = 2ull * 1024 * 1024 * 1024;
    std::vector<std::string> function_names;  // index = IR function index
    std::vector<std::string> global_names;
    std::vector<RtStructInfo> structs;
    std::vector<RtEnumInfo> enums;
};

struct RtOutcome {
    bool ok = true;
    bool backend_limit = false;  // arena exhausted: not a program error
    std::string message;          // VM-style message, e.g. "integer overflow"
    std::vector<RtTraceEntry> trace;  // innermost first
    uint64_t arena_bytes = 0;
};

using EntryFn = int8_t (*)();

// Runs init then main with traps caught. Not reentrant.
RtOutcome run_entry(const RtConfig& cfg, EntryFn init, EntryFn main);

// Renders like vm::RunResult::render.
std::string render_error(const RtOutcome& r, const std::string& source_name);

// Name -> address of every runtime symbol the generated code may reference.
struct RtSymbol {
    const char* name;
    void* addr;
};
const std::vector<RtSymbol>& runtime_symbols();

}  // namespace ember::native

// Runtime support for code produced by the native (LLVM) backend.
//
// The generated code calls these extern "C" functions for everything that
// is not plain arithmetic: printing, strings, traps. The functions keep the
// exact observable behaviour of the VM (same output text, same runtime
// error messages, same stack-trace format), which the differential tests
// check.
//
// Strings are allocated from an arena that is freed when the run ends.
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

// Layout shared with the generated code: { i64 len, [len+1 x i8] }.
struct EmStr {
    int64_t len;
    char data[1];  // really len + 1 bytes (NUL-terminated for convenience)
};

// Error kinds the generated code can raise directly. The numbering is
// private to the backend; messages match vm::error_kind_name.
enum TrapKind : int32_t {
    kTrapDivZero = 0,
    kTrapOverflow = 1,
    kTrapUnreachable = 2,
    kTrapUninitGlobal = 3,  // arg = global index
    kTrapStackOverflow = 4,
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
    size_t arena_limit_bytes = 2ull * 1024 * 1024 * 1024;
    std::vector<std::string> function_names;  // index = IR function index
    std::vector<std::string> global_names;
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

// The `ember` command-line tool.
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "bc/disasm.h"
#include "bc/format.h"
#include "bc/verify.h"
#include "common/diagnostics.h"
#include "driver/driver.h"
#include "front/lexer.h"
#include "ir/ir.h"
#include "tools/ast_print.h"
#include "tools/debugger.h"
#include "tools/formatter.h"
#include "tools/profiler.h"
#include "tools/repl.h"
#include "tools/version.h"
#include "vm/vm.h"

using namespace ember;

namespace {

enum Exit { kOk = 0, kCompileError = 1, kUsage = 2, kRuntimeError = 3, kIoError = 4, kBadBytecode = 5, kInternal = 70 };

const char* kUsageText =
    "usage: ember <command> [options] [file]\n"
    "\n"
    "commands:\n"
    "  run <file.em|file.embc>     compile (if needed) and run a program\n"
    "  build <file.em> [-o out]    compile to a bytecode file (default: <file>.embc)\n"
    "  check <file.em>             parse and type check only\n"
    "  debug <file.em>             run under the interactive debugger\n"
    "  repl                        interactive session\n"
    "  fmt <file.em> [--write|--check]  format source (prints to stdout by default)\n"
    "  disasm <file.em|file.embc>  show bytecode\n"
    "  verify <file.embc>          decode and verify a bytecode file\n"
    "  ast <file.em>               show the typed syntax tree\n"
    "  ir <file.em>                show the intermediate representation\n"
    "  tokens <file.em>            show the token stream\n"
    "  version                     show the version\n"
    "\n"
    "options:\n"
    "  -O0, -O1                    optimization level (default -O1)\n"
    "  -o <path>                   output file for build\n"
    "  --no-loop-rotation          keep loop tests at the top (for measurements)\n"
    "  --no-block-layout           emit blocks in creation order (for measurements)\n"
    "  --no-superinstructions      do not fuse instruction pairs (for measurements)\n"
    "  --no-debug-info             omit line tables and names from build output\n"
    "  --gc-stress                 collect garbage before every allocation\n"
    "  --max-heap=<bytes>          heap limit (suffixes K, M, G; default 256M)\n"
    "  --max-depth=<n>             call depth limit (default 10000)\n"
    "  --max-stack=<n>             operand stack limit in values (default 1048576)\n"
    "  --max-steps=<n>             instruction budget (default unlimited)\n"
    "  --stats                     print execution statistics to stderr\n"
    "  --profile                   count executed opcodes, opcode pairs and per-function\n"
    "                              instructions; report to stderr (slower run)\n"
    "\n"
    "exit status: 0 success, 1 compile error, 2 usage error, 3 runtime error,\n"
    "             4 I/O error, 5 invalid bytecode file, 70 internal error\n";

constexpr size_t kMaxSource = 16u * 1024 * 1024;
constexpr size_t kMaxBytecode = 64u * 1024 * 1024;

struct Options {
    std::string command;
    std::string file;
    std::string output;
    int opt_level = 1;
    bool rotate_loops = true;
    bool block_layout = true;
    bool superinstructions = true;
    bool debug_info = true;
    bool stats = false;
    bool profile = false;
    bool fmt_write = false;
    bool fmt_check = false;
    vm::VMOptions vm;
};

bool parse_size(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str()) return false;
    std::string suffix(end);
    uint64_t mul = 1;
    if (suffix == "K" || suffix == "k") mul = 1024;
    else if (suffix == "M" || suffix == "m") mul = 1024 * 1024;
    else if (suffix == "G" || suffix == "g") mul = 1024ull * 1024 * 1024;
    else if (!suffix.empty()) return false;
    if (v > UINT64_MAX / mul) return false;
    out = v * mul;
    return true;
}

// Reads at most limit+1 bytes so oversized inputs are detected without
// loading them entirely.
enum class ReadStatus { Ok, Missing, TooLarge };
ReadStatus read_file(const std::string& path, std::string& out, size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return ReadStatus::Missing;
    out.clear();
    char buf[65536];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) {
        out.append(buf, static_cast<size_t>(in.gcount()));
        if (out.size() > limit) return ReadStatus::TooLarge;
    }
    if (in.bad()) return ReadStatus::Missing;
    return ReadStatus::Ok;
}

bool is_bytecode_file(const std::string& data) { return data.size() >= 4 && std::memcmp(data.data(), bc::kMagic, 4) == 0; }

int report_read(ReadStatus st, const std::string& path, size_t limit) {
    if (st == ReadStatus::Missing) std::cerr << "error: cannot read '" << path << "'\n";
    else std::cerr << "error: '" << path << "' is larger than the limit of " << limit << " bytes\n";
    return kIoError;
}

std::unique_ptr<CompileOutput> compile_file(const Options& o, const std::string& text, Stage stage) {
    CompileOptions co;
    co.opt_level = o.opt_level;
    co.rotate_loops = o.rotate_loops;
    co.block_layout = o.block_layout;
    co.superinstructions = o.superinstructions;
    co.debug_info = o.debug_info;
    co.max_source_bytes = kMaxSource;
    return compile(o.file, text, co, stage);
}

int compile_status(const CompileOutput& c) {
    std::cerr << c.diagnostics;
    if (c.internal_error) return kInternal;
    if (!c.ok) return kCompileError;
    return kOk;
}

// Load a module from a source or bytecode file.
int load_module(const Options& o, bc::Module& out, std::unique_ptr<CompileOutput>& keep) {
    std::string data;
    auto st = read_file(o.file, data, kMaxBytecode);
    if (st != ReadStatus::Ok) return report_read(st, o.file, kMaxBytecode);
    if (is_bytecode_file(data)) {
        try {
            out = bc::decode(std::vector<uint8_t>(data.begin(), data.end()), kMaxBytecode);
        } catch (const bc::DecodeError& e) {
            std::cerr << "error: " << o.file << ": " << e.what() << "\n";
            return kBadBytecode;
        }
        auto vr = bc::verify(out);
        if (!vr.ok()) {
            std::cerr << "error: " << o.file << ": bytecode verification failed\n";
            for (const auto& e : vr.errors) std::cerr << "  " << e << "\n";
            return kBadBytecode;
        }
        return kOk;
    }
    if (data.size() > kMaxSource) return report_read(ReadStatus::TooLarge, o.file, kMaxSource);
    keep = compile_file(o, data, Stage::Bytecode);
    int rc = compile_status(*keep);
    if (rc != kOk) return rc;
    out = keep->bytecode;
    return kOk;
}

void print_stats(const vm::VM& m, double seconds) {
    const auto& h = m.heap_stats();
    std::cerr << "--- stats ---\n"
              << "instructions:   " << m.instructions_executed() << "\n"
              << "time:           " << seconds << " s\n"
              << "gc collections: " << h.collections << " (" << h.gc_seconds << " s)\n"
              << "allocated:      " << h.bytes_allocated_total << " bytes in " << h.objects_allocated_total << " objects\n"
              << "freed objects:  " << h.objects_freed_total << "\n"
              << "peak heap:      " << h.peak_bytes << " bytes\n";
}

int cmd_run(const Options& o, bool debug) {
    bc::Module mod;
    std::unique_ptr<CompileOutput> keep;
    int rc = load_module(o, mod, keep);
    if (rc != kOk) return rc;
    std::unique_ptr<vm::VM> machine;
    try {
        machine = std::make_unique<vm::VM>(mod, o.vm);
    } catch (const vm::InvalidModule& e) {
        std::cerr << "error: " << e.what() << "\n";
        return kInternal;
    }
    std::unique_ptr<Debugger> dbg;
    std::unique_ptr<Profiler> prof;
    if (o.profile && !debug) {
        prof = std::make_unique<Profiler>();
        machine->set_debug_hook(prof.get());
    }
    if (debug) {
        if (!keep) {
            std::cerr << "error: the debugger needs the source file, not a bytecode file\n";
            return kUsage;
        }
        dbg = std::make_unique<Debugger>(*keep->source, std::cin, std::cout);
        machine->set_debug_hook(dbg.get());
    }
    auto t0 = std::chrono::steady_clock::now();
    vm::RunResult r;
    try {
        r = machine->run();
    } catch (const DebuggerQuit&) {
        std::cout << "program stopped\n";
        return kOk;
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout.flush();
    if (o.stats) print_stats(*machine, secs);
    if (prof) std::cerr << prof->report(*machine);
    if (!r.ok) {
        std::cerr << r.render(mod.source_name.empty() ? o.file : mod.source_name);
        return kRuntimeError;
    }
    return kOk;
}

int cmd_build(const Options& o) {
    std::string text;
    auto st = read_file(o.file, text, kMaxSource);
    if (st != ReadStatus::Ok) return report_read(st, o.file, kMaxSource);
    auto c = compile_file(o, text, Stage::Bytecode);
    int rc = compile_status(*c);
    if (rc != kOk) return rc;
    std::string out = o.output;
    if (out.empty()) {
        out = o.file;
        if (out.size() > 3 && out.compare(out.size() - 3, 3, ".em") == 0) out.resize(out.size() - 3);
        out += ".embc";
    }
    auto bytes = bc::encode(c->bytecode, o.debug_info);
    std::ofstream f(out, std::ios::binary);
    if (!f || !f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        std::cerr << "error: cannot write '" << out << "'\n";
        return kIoError;
    }
    return kOk;
}

int cmd_source_stage(const Options& o) {
    std::string text;
    auto st = read_file(o.file, text, kMaxSource);
    if (st != ReadStatus::Ok) return report_read(st, o.file, kMaxSource);
    if (o.command == "tokens") {
        SourceFile src(o.file, text);
        Diagnostics diags;
        Lexer lexer(src, diags);
        for (const auto& t : lexer.tokenize()) {
            std::cout << t.span.line << ":" << t.span.column << "\t" << tok_desc(t.kind);
            if (!t.text.empty()) std::cout << "\t" << t.text;
            std::cout << "\n";
        }
        std::cerr << diags.render(src);
        return diags.has_errors() ? kCompileError : kOk;
    }
    if (o.command == "fmt") {
        auto r = format_source(o.file, text);
        if (!r.ok) {
            std::cerr << r.diagnostics;
            return kCompileError;
        }
        if (o.fmt_check) {
            if (r.output != text) {
                std::cerr << o.file << ": not formatted\n";
                return kCompileError;
            }
            return kOk;
        }
        if (o.fmt_write) {
            if (r.output == text) return kOk;
            std::ofstream f(o.file, std::ios::binary);
            if (!f || !(f << r.output)) {
                std::cerr << "error: cannot write '" << o.file << "'\n";
                return kIoError;
            }
            return kOk;
        }
        std::cout << r.output;
        return kOk;
    }
    Stage stage = o.command == "check" || o.command == "ast" ? Stage::Check : o.command == "ir" ? Stage::IR : Stage::Bytecode;
    auto c = compile_file(o, text, stage);
    int rc = compile_status(*c);
    if (rc != kOk) return rc;
    if (o.command == "ast") std::cout << print_ast(c->program);
    else if (o.command == "ir") std::cout << ir::print_module(c->ir);
    return kOk;
}

int cmd_disasm_verify(const Options& o) {
    bc::Module mod;
    std::unique_ptr<CompileOutput> keep;
    int rc = load_module(o, mod, keep);
    if (rc != kOk) return rc;
    if (o.command == "disasm") std::cout << bc::disassemble(mod);
    else std::cout << o.file << ": ok (" << mod.functions.size() << " functions)\n";
    return kOk;
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    Options o;
    std::vector<std::string> args(argv + 1, argv + argc);
    std::vector<std::string> positional;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](const char* name, std::string& v) {
            size_t n = std::strlen(name);
            if (a.compare(0, n, name) == 0 && a.size() > n && a[n] == '=') {
                v = a.substr(n + 1);
                return true;
            }
            return false;
        };
        std::string v;
        uint64_t num = 0;
        if (a == "-h" || a == "--help") {
            std::cout << kUsageText;
            return kOk;
        } else if (a == "--version") {
            std::cout << version_string() << "\n";
            return kOk;
        } else if (a == "-O0") {
            o.opt_level = 0;
        } else if (a == "-O1") {
            o.opt_level = 1;
        } else if (a == "-o") {
            if (i + 1 >= args.size()) {
                std::cerr << "error: -o needs a path\n";
                return kUsage;
            }
            o.output = args[++i];
        } else if (a == "--no-loop-rotation") {
            o.rotate_loops = false;
        } else if (a == "--no-block-layout") {
            o.block_layout = false;
        } else if (a == "--no-superinstructions") {
            o.superinstructions = false;
        } else if (a == "--no-debug-info") {
            o.debug_info = false;
        } else if (a == "--gc-stress") {
            o.vm.gc_stress = true;
        } else if (a == "--stats") {
            o.stats = true;
            o.vm.count_instructions = true;
        } else if (a == "--profile") {
            o.profile = true;
        } else if (a == "--write") {
            o.fmt_write = true;
        } else if (a == "--check") {
            o.fmt_check = true;
        } else if (value("--max-heap", v)) {
            if (!parse_size(v, num) || num < 4096) {
                std::cerr << "error: invalid --max-heap value '" << v << "'\n";
                return kUsage;
            }
            o.vm.max_heap_bytes = num;
        } else if (value("--max-depth", v)) {
            if (!parse_size(v, num) || num == 0 || num > 10'000'000) {
                std::cerr << "error: invalid --max-depth value '" << v << "'\n";
                return kUsage;
            }
            o.vm.max_call_depth = static_cast<uint32_t>(num);
        } else if (value("--max-stack", v)) {
            if (!parse_size(v, num) || num < 16 || num > (1ull << 28)) {
                std::cerr << "error: invalid --max-stack value '" << v << "'\n";
                return kUsage;
            }
            o.vm.max_stack_values = static_cast<uint32_t>(num);
        } else if (value("--max-steps", v)) {
            if (!parse_size(v, num)) {
                std::cerr << "error: invalid --max-steps value '" << v << "'\n";
                return kUsage;
            }
            o.vm.instruction_budget = num;
        } else if (a.size() > 1 && a[0] == '-') {
            std::cerr << "error: unknown option '" << a << "'\n" << kUsageText;
            return kUsage;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.empty()) {
        std::cerr << kUsageText;
        return kUsage;
    }
    o.command = positional[0];
    if (o.command == "help") {
        std::cout << kUsageText;
        return kOk;
    }
    if (o.command == "version") {
        std::cout << version_string() << "\n";
        return kOk;
    }
    if (o.command == "repl") return run_repl(std::cin, std::cout, true);
    static const char* with_file[] = {"run", "build", "check", "debug", "fmt", "disasm", "verify", "ast", "ir", "tokens"};
    bool known = false;
    for (const char* c : with_file) known |= o.command == c;
    if (!known) {
        std::cerr << "error: unknown command '" << o.command << "'\n" << kUsageText;
        return kUsage;
    }
    if (positional.size() != 2) {
        std::cerr << "error: '" << o.command << "' needs exactly one file argument\n";
        return kUsage;
    }
    o.file = positional[1];
    try {
        if (o.command == "run") return cmd_run(o, false);
        if (o.command == "debug") return cmd_run(o, true);
        if (o.command == "build") return cmd_build(o);
        if (o.command == "disasm" || o.command == "verify") return cmd_disasm_verify(o);
        return cmd_source_stage(o);
    } catch (const std::bad_alloc&) {
        std::cerr << "error: out of memory\n";
        return kInternal;
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return kInternal;
    }
}

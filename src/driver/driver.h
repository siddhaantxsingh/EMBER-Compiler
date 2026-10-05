// The compilation pipeline as a library: source text -> tokens -> AST ->
// resolved & typed AST -> IR -> optimized IR -> bytecode (verified).
// Each stage only runs if the previous one succeeded.
#pragma once

#include <memory>
#include <string>

#include "bc/module.h"
#include "common/diagnostics.h"
#include "common/source.h"
#include "front/ast.h"
#include "ir/ir.h"
#include "ir/opt.h"
#include "sema/types.h"

namespace ember {

enum class Stage { Parse, Check, IR, Bytecode };

struct CompileOptions {
    int opt_level = 1;
    bool rotate_loops = true;  // only applies at opt_level >= 1
    bool block_layout = true;  // only applies at opt_level >= 1
    bool superinstructions = true;  // only applies at opt_level >= 1
    bool debug_info = true;
    size_t max_source_bytes = 16u * 1024 * 1024;
    bool validate_ir = true;  // run the IR validator after lowering and after optimizing
};

struct CompileOutput {
    bool ok = false;
    bool internal_error = false;  // a compiler bug (never the user's fault)
    std::string diagnostics;      // rendered, ready to print
    size_t error_count = 0;
    std::unique_ptr<SourceFile> source;
    Diagnostics diags;
    Program program;
    SemaResult sema;
    ir::Module ir;
    ir::OptStats opt_stats;
    bc::Module bytecode;
};

// Never throws for any input; compiler bugs are reported through
// internal_error with a message in diagnostics.
std::unique_ptr<CompileOutput> compile(const std::string& name, const std::string& text, const CompileOptions& opts,
                                       Stage stop_after = Stage::Bytecode);

}  // namespace ember

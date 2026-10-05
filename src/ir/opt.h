#pragma once

#include <string>

#include "ir/ir.h"

namespace ember::ir {

struct OptStats {
    int folded = 0;          // instructions replaced by constants
    int branches_folded = 0; // conditional branches with constant conditions
    int blocks_removed = 0;  // unreachable or merged blocks
    int jumps_threaded = 0;  // edges redirected past empty blocks
    int dead_removed = 0;    // unused pure instructions deleted
    std::string summary() const;
};

// IR-level optimizations. Every pass preserves observable behaviour exactly,
// including traps: an operation that would trap at run time is never folded.
//   level 0: only removes unreachable blocks (required by code generation)
//   level 1: constant folding, branch folding, CFG simplification, DCE
OptStats optimize(Module& m, int level);

}  // namespace ember::ir

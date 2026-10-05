#pragma once

#include <string>
#include <vector>

#include "ir/ir.h"

namespace ember::ir {

// Structural and kind checks for a lowered (or optimized) module. Returns a
// list of human-readable problems; empty means valid. Any problem is a
// compiler bug, never a user error.
std::vector<std::string> validate(const Module& m);

// Remove blocks unreachable from the entry and renumber the rest.
void remove_unreachable_blocks(Function& f);

// Predecessor lists (by block id) over the current CFG.
std::vector<std::vector<BlockId>> predecessors(const Function& f);

}  // namespace ember::ir

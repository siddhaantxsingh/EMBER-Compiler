#pragma once

#include <string>

#include "bc/module.h"

namespace ember::bc {

// Human-readable listing of a module: tables, then each function's code
// with pc, source line, opcode, decoded operands and symbolic names.
// Never crashes on malformed code (it prints "<truncated>"/"<invalid>").
std::string disassemble(const Module& m);
std::string disassemble_function(const Module& m, size_t index);
// One instruction at pc; advances pc. Used by the debugger and tracer.
std::string disassemble_instruction(const Module& m, const Function& f, uint32_t& pc);

}  // namespace ember::bc

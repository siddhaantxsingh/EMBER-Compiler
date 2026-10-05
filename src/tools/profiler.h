// Execution profiler (`ember run --profile`): a VM DebugHook that counts
// executed instructions per opcode, per adjacent opcode pair (to find
// superinstruction candidates) and per function. It uses the instrumented
// interpreter loop, so it measures *what* runs, not how long it takes.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "bc/opcodes.h"
#include "vm/vm.h"

namespace ember {

class Profiler : public vm::DebugHook {
public:
    void before_instruction(vm::VM& vm) override;
    std::string report(const vm::VM& vm, size_t top = 15) const;

private:
    std::array<uint64_t, bc::kOpcodeCount> ops_{};
    std::map<std::pair<uint8_t, uint8_t>, uint64_t> pairs_;
    std::vector<uint64_t> per_function_;
    uint64_t total_ = 0;
    int prev_op_ = -1;
    const void* prev_fn_ = nullptr;
};

}  // namespace ember

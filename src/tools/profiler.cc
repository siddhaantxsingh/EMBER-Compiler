#include "tools/profiler.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace ember {

void Profiler::before_instruction(vm::VM& vm) {
    auto f = vm.frame(0);
    uint8_t op = f.fn->code[f.pc];
    ++ops_[op];
    ++total_;
    if (per_function_.size() <= f.function_index) per_function_.resize(f.function_index + 1);
    ++per_function_[f.function_index];
    // Pairs only within one function activation sequence.
    if (prev_op_ >= 0 && prev_fn_ == f.fn) ++pairs_[{static_cast<uint8_t>(prev_op_), op}];
    prev_op_ = op;
    prev_fn_ = f.fn;
}

std::string Profiler::report(const vm::VM& vm, size_t top) const {
    std::ostringstream o;
    char buf[160];
    auto pct = [&](uint64_t n) { return total_ ? 100.0 * static_cast<double>(n) / static_cast<double>(total_) : 0.0; };
    o << "--- profile: " << total_ << " instructions ---\n";
    std::vector<std::pair<uint64_t, size_t>> ops;
    for (size_t i = 0; i < ops_.size(); ++i)
        if (ops_[i]) ops.push_back({ops_[i], i});
    std::sort(ops.rbegin(), ops.rend());
    o << "top opcodes:\n";
    for (size_t i = 0; i < ops.size() && i < top; ++i) {
        std::snprintf(buf, sizeof buf, "  %-16s %12llu  %5.1f%%\n", bc::opcode_info(static_cast<bc::Opcode>(ops[i].second)).name,
                      static_cast<unsigned long long>(ops[i].first), pct(ops[i].first));
        o << buf;
    }
    std::vector<std::pair<uint64_t, std::pair<uint8_t, uint8_t>>> pairs;
    for (const auto& [k, v] : pairs_) pairs.push_back({v, k});
    std::sort(pairs.rbegin(), pairs.rend());
    o << "top opcode pairs:\n";
    for (size_t i = 0; i < pairs.size() && i < top; ++i) {
        std::string name = std::string(bc::opcode_info(static_cast<bc::Opcode>(pairs[i].second.first)).name) + " > " +
                           bc::opcode_info(static_cast<bc::Opcode>(pairs[i].second.second)).name;
        std::snprintf(buf, sizeof buf, "  %-32s %12llu  %5.1f%%\n", name.c_str(),
                      static_cast<unsigned long long>(pairs[i].first), pct(pairs[i].first));
        o << buf;
    }
    std::vector<std::pair<uint64_t, size_t>> fns;
    for (size_t i = 0; i < per_function_.size(); ++i)
        if (per_function_[i]) fns.push_back({per_function_[i], i});
    std::sort(fns.rbegin(), fns.rend());
    o << "functions:\n";
    for (size_t i = 0; i < fns.size() && i < top; ++i) {
        std::snprintf(buf, sizeof buf, "  %-32s %12llu  %5.1f%%\n", vm.module().functions[fns[i].second].name.c_str(),
                      static_cast<unsigned long long>(fns[i].first), pct(fns[i].first));
        o << buf;
    }
    return o.str();
}

}  // namespace ember

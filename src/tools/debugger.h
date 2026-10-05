// Source-level debugger for `ember debug`. Implemented as a VM DebugHook:
// the VM calls it before each instruction (only when a debugger is attached;
// the normal interpreter loop has no hook check). Commands are read from an
// input stream so sessions can be scripted in tests.
#pragma once

#include <iosfwd>
#include <set>
#include <string>
#include <vector>

#include "common/source.h"
#include "vm/vm.h"

namespace ember {

class Debugger : public vm::DebugHook {
public:
    Debugger(const SourceFile& src, std::istream& in, std::ostream& out) : src_(src), in_(in), out_(out) {}
    void before_instruction(vm::VM& vm) override;

private:
    enum class Mode { StepLine, Next, Finish, Continue, StepInstr };
    void prompt(vm::VM& vm);
    void show_location(vm::VM& vm);
    bool command(vm::VM& vm, const std::string& line);  // returns true to resume
    void print_name(vm::VM& vm, const std::string& name);
    void locals(vm::VM& vm);
    void backtrace(vm::VM& vm);
    void list(uint32_t line);
    uint32_t current_line(vm::VM& vm) const;

    const SourceFile& src_;
    std::istream& in_;
    std::ostream& out_;
    Mode mode_ = Mode::StepLine;
    size_t start_depth_ = 0;
    uint32_t start_line_ = 0;
    const void* start_fn_ = nullptr;
    // Line tracking to detect "arrived at a new line".
    size_t last_depth_ = 0;
    uint32_t last_line_ = 0;
    const void* last_fn_ = nullptr;
    std::set<uint32_t> line_breaks_;
    std::set<std::string> fn_breaks_;
    bool quit_ = false;
};

// Thrown out of the VM when the user quits the debugger.
struct DebuggerQuit {};

}  // namespace ember

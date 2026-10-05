#include "tools/debugger.h"

#include <iostream>
#include <sstream>

#include "bc/disasm.h"

namespace ember {

namespace {

const char* kHelp =
    "commands:\n"
    "  s, step            run to the next source line (entering calls)\n"
    "  n, next            run to the next line in this function (stepping over calls)\n"
    "  finish             run until the current function returns\n"
    "  c, continue        run until a breakpoint or the end\n"
    "  si, stepi          execute one bytecode instruction\n"
    "  b, break LINE|FN   set a breakpoint at a source line or function entry\n"
    "  delete LINE|FN     remove a breakpoint\n"
    "  info breakpoints   list breakpoints\n"
    "  bt, backtrace      show the call stack\n"
    "  locals             show local variables of the current function\n"
    "  p, print NAME      show a local, captured variable or global\n"
    "  l, list [LINE]     show source around the current (or given) line\n"
    "  disasm             show the bytecode of the current function\n"
    "  q, quit            stop the program\n"
    "  h, help            this text\n";

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

uint32_t Debugger::current_line(vm::VM& vm) const {
    auto f = vm.frame(0);
    return f.fn->location(f.pc).line;
}

void Debugger::before_instruction(vm::VM& vm) {
    if (quit_) throw DebuggerQuit{};
    size_t depth = vm.frame_count();
    auto f = vm.frame(0);
    uint32_t line = f.fn->location(f.pc).line;
    bool new_line = line != 0 && (line != last_line_ || depth != last_depth_ || f.fn != last_fn_);
    bool entered_fn = f.pc == 0;
    last_line_ = line;
    last_depth_ = depth;
    last_fn_ = f.fn;

    bool stop = false;
    switch (mode_) {
        case Mode::StepInstr: stop = true; break;
        case Mode::StepLine: stop = new_line && (line != start_line_ || depth != start_depth_ || f.fn != start_fn_); break;
        case Mode::Next: stop = new_line && depth <= start_depth_ && (line != start_line_ || depth != start_depth_); break;
        case Mode::Finish: stop = new_line && depth < start_depth_; break;
        case Mode::Continue: break;
    }
    if (!stop && new_line && line_breaks_.count(line)) stop = true;
    if (!stop && entered_fn && fn_breaks_.count(f.fn->name)) stop = true;
    if (!stop) return;
    show_location(vm);
    prompt(vm);
}

void Debugger::show_location(vm::VM& vm) {
    auto f = vm.frame(0);
    auto loc = f.fn->location(f.pc);
    out_ << f.fn->name << " at " << src_.name() << ":" << loc.line << "\n";
    if (loc.line != 0 && loc.line <= src_.line_count()) out_ << "  " << loc.line << " | " << src_.line_text(loc.line) << "\n";
}

void Debugger::prompt(vm::VM& vm) {
    std::string line;
    for (;;) {
        out_ << "(edb) " << std::flush;
        if (!std::getline(in_, line)) {
            // End of input: let the program run to completion.
            mode_ = Mode::Continue;
            line_breaks_.clear();
            fn_breaks_.clear();
            out_ << "\n";
            return;
        }
        if (command(vm, trim(line))) return;
        if (quit_) throw DebuggerQuit{};
    }
}

bool Debugger::command(vm::VM& vm, const std::string& line) {
    std::istringstream ss(line);
    std::string cmd, arg;
    ss >> cmd;
    std::getline(ss, arg);
    arg = trim(arg);
    auto resume = [&](Mode m) {
        mode_ = m;
        start_depth_ = vm.frame_count();
        start_line_ = current_line(vm);
        start_fn_ = vm.frame(0).fn;
        return true;
    };
    if (cmd.empty()) return false;
    if (cmd == "s" || cmd == "step") return resume(Mode::StepLine);
    if (cmd == "n" || cmd == "next") return resume(Mode::Next);
    if (cmd == "finish") return resume(Mode::Finish);
    if (cmd == "c" || cmd == "continue") return resume(Mode::Continue);
    if (cmd == "si" || cmd == "stepi") return resume(Mode::StepInstr);
    if (cmd == "q" || cmd == "quit") {
        quit_ = true;
        return false;
    }
    if (cmd == "h" || cmd == "help") {
        out_ << kHelp;
        return false;
    }
    if (cmd == "b" || cmd == "break" || cmd == "delete") {
        if (arg.empty()) {
            out_ << "usage: " << cmd << " LINE|FUNCTION\n";
            return false;
        }
        bool is_line = arg.find_first_not_of("0123456789") == std::string::npos;
        if (cmd == "delete") {
            size_t n = is_line ? line_breaks_.erase(static_cast<uint32_t>(std::stoul(arg))) : fn_breaks_.erase(arg);
            out_ << (n ? "deleted breakpoint " : "no breakpoint at ") << arg << "\n";
        } else if (is_line) {
            unsigned long l = std::stoul(arg);
            if (l == 0 || l > src_.line_count()) {
                out_ << "line " << arg << " is outside the file\n";
            } else {
                line_breaks_.insert(static_cast<uint32_t>(l));
                out_ << "breakpoint at line " << l << "\n";
            }
        } else {
            bool found = false;
            for (const auto& f : vm.module().functions) found |= f.name == arg;
            if (!found) {
                out_ << "no function named '" << arg << "'\n";
            } else {
                fn_breaks_.insert(arg);
                out_ << "breakpoint at function " << arg << "\n";
            }
        }
        return false;
    }
    if (cmd == "info") {
        out_ << "breakpoints:";
        for (uint32_t l : line_breaks_) out_ << " line " << l;
        for (const auto& f : fn_breaks_) out_ << " fn " << f;
        if (line_breaks_.empty() && fn_breaks_.empty()) out_ << " none";
        out_ << "\n";
        return false;
    }
    if (cmd == "bt" || cmd == "backtrace") {
        backtrace(vm);
        return false;
    }
    if (cmd == "locals") {
        locals(vm);
        return false;
    }
    if (cmd == "p" || cmd == "print") {
        print_name(vm, arg);
        return false;
    }
    if (cmd == "l" || cmd == "list") {
        uint32_t l = arg.empty() ? current_line(vm) : static_cast<uint32_t>(std::strtoul(arg.c_str(), nullptr, 10));
        list(l);
        return false;
    }
    if (cmd == "disasm") {
        out_ << bc::disassemble_function(vm.module(), vm.frame(0).function_index);
        return false;
    }
    out_ << "unknown command '" << cmd << "' (type 'help')\n";
    return false;
}

void Debugger::backtrace(vm::VM& vm) {
    for (size_t i = 0; i < vm.frame_count(); ++i) {
        auto f = vm.frame(i);
        auto loc = f.fn->location(f.pc);
        out_ << "#" << i << " " << f.fn->name << " (" << src_.name() << ":" << loc.line << ")\n";
    }
}

void Debugger::locals(vm::VM& vm) {
    auto f = vm.frame(0);
    bool any = false;
    for (size_t i = 0; i < f.fn->local_names.size(); ++i) {
        const std::string& n = f.fn->local_names[i];
        if (n.empty()) continue;
        out_ << "  " << n << " = " << vm.format_value(f.locals[i]) << "\n";
        any = true;
    }
    if (f.closure)
        for (size_t i = 0; i < f.fn->upvalues.size(); ++i) {
            out_ << "  " << f.fn->upvalues[i].name << " = " << vm.format_value(*f.closure->upvalues[i]->loc)
                 << "  (captured)\n";
            any = true;
        }
    if (!any) out_ << "  (no locals)\n";
}

void Debugger::print_name(vm::VM& vm, const std::string& name) {
    auto f = vm.frame(0);
    // Prefer the highest slot with that name (the innermost declaration).
    for (size_t i = f.fn->local_names.size(); i-- > 0;)
        if (f.fn->local_names[i] == name) {
            out_ << name << " = " << vm.format_value(f.locals[i]) << "\n";
            return;
        }
    if (f.closure)
        for (size_t i = 0; i < f.fn->upvalues.size(); ++i)
            if (f.fn->upvalues[i].name == name) {
                out_ << name << " = " << vm.format_value(*f.closure->upvalues[i]->loc) << "\n";
                return;
            }
    const auto& gs = vm.module().globals;
    for (size_t i = 0; i < gs.size(); ++i)
        if (gs[i].name == name) {
            out_ << name << " = " << vm.format_value(vm.global(i)) << "\n";
            return;
        }
    out_ << "no variable named '" << name << "' here\n";
}

void Debugger::list(uint32_t line) {
    if (line == 0) line = 1;
    uint32_t from = line > 4 ? line - 4 : 1;
    uint32_t to = std::min(src_.line_count(), line + 4);
    for (uint32_t l = from; l <= to; ++l)
        out_ << (l == line ? "->" : "  ") << " " << l << " | " << src_.line_text(l) << "\n";
}

}  // namespace ember

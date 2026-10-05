#include "tools/repl.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "driver/driver.h"
#include "tools/version.h"
#include "vm/vm.h"

namespace ember {

namespace {

bool starts_with_word(const std::string& s, const char* w) {
    size_t n = std::char_traits<char>::length(w);
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos || s.compare(a, n, w) != 0) return false;
    return a + n == s.size() || s[a + n] == ' ' || s[a + n] == '\t' || s[a + n] == '(';
}

// Brace/bracket/paren balance outside strings and comments.
int balance(const std::string& s) {
    int depth = 0;
    bool in_str = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') ++i;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '{' || c == '(' || c == '[') ++depth;
        else if (c == '}' || c == ')' || c == ']') --depth;
    }
    return depth;
}

struct Session {
    std::vector<std::string> decls;
    std::vector<std::string> body;
    size_t shown = 0;  // bytes of output already printed

    std::string program(const std::vector<std::string>& extra_decls, const std::vector<std::string>& extra_body) const {
        std::string s;
        for (const auto& d : decls) s += d + "\n";
        for (const auto& d : extra_decls) s += d + "\n";
        s += "fn main() {\n";
        for (const auto& b : body) s += b + "\n";
        for (const auto& b : extra_body) s += b + "\n";
        s += "}\n";
        return s;
    }
};

enum class Attempt { Ok, CompileError, RuntimeError };

// Compile and run; prints output not shown before.
Attempt attempt(Session& st, const std::vector<std::string>& d, const std::vector<std::string>& b, std::ostream& out,
             std::string& error) {
    std::string src = st.program(d, b);
    auto c = compile("<repl>", src, CompileOptions{});
    if (!c->ok) {
        error = c->diagnostics;
        return Attempt::CompileError;
    }
    std::ostringstream captured;
    std::istringstream no_input;
    vm::VM machine(c->bytecode);
    machine.set_output(&captured);
    machine.set_input(&no_input);
    auto r = machine.run();
    std::string all = captured.str();
    if (!r.ok) {
        if (all.size() > st.shown) out << all.substr(st.shown);
        error = r.render("<repl>");
        return Attempt::RuntimeError;
    }
    if (all.size() > st.shown) out << all.substr(st.shown);
    st.shown = all.size();
    return Attempt::Ok;
}

}  // namespace

int run_repl(std::istream& in, std::ostream& out, bool interactive) {
    Session st;
    if (interactive) out << version_string() << " — type :help for help, :quit to exit\n";
    std::string pending;
    for (;;) {
        if (interactive) out << (pending.empty() ? ">>> " : "... ") << std::flush;
        std::string line;
        if (!std::getline(in, line)) break;
        if (pending.empty()) {
            if (line == ":quit" || line == ":q") break;
            if (line == ":help") {
                out << "Enter statements, expressions or declarations (fn, struct, enum, const, import, var/let at\n"
                       "top level with ':global'). Expressions are printed. Commands: :reset, :program, :quit.\n";
                continue;
            }
            if (line == ":reset") {
                st = Session{};
                out << "state cleared\n";
                continue;
            }
            if (line == ":program") {
                out << st.program({}, {});
                continue;
            }
        }
        pending += (pending.empty() ? "" : "\n") + line;
        if (balance(pending) > 0) continue;
        std::string input = pending;
        pending.clear();
        if (input.find_first_not_of(" \t\r") == std::string::npos) continue;

        std::string err;
        bool is_decl = starts_with_word(input, "fn") && input.find('(') > input.find_first_not_of(" \t") + 3;
        is_decl = is_decl || starts_with_word(input, "struct") || starts_with_word(input, "enum") ||
                  starts_with_word(input, "const") || starts_with_word(input, "import");
        bool is_global = input.rfind(":global ", 0) == 0;
        if (is_global) input = input.substr(8);
        if (is_decl || is_global) {
            if (attempt(st, {input}, {}, out, err) == Attempt::Ok) st.decls.push_back(input);
            else out << err;
            continue;
        }
        // As statement(s) first; if that fails, as an expression to print.
        Attempt a = attempt(st, {}, {input}, out, err);
        if (a == Attempt::Ok) {
            st.body.push_back(input);
            continue;
        }
        if (a == Attempt::CompileError && input.back() != ';' && input.back() != '}') {
            std::string as_print = "print(" + input + ");";
            std::string expr_err;
            Attempt b = attempt(st, {}, {as_print}, out, expr_err);
            if (b == Attempt::Ok) {
                st.body.push_back(as_print);
                continue;
            }
            // The input was meant as an expression: its errors are the
            // relevant ones (the statement attempt only adds "expected ';'").
            out << expr_err;
            continue;
        }
        out << err;
    }
    if (interactive) out << "\n";
    return 0;
}

}  // namespace ember

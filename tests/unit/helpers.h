#pragma once

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "bc/module.h"
#include "bc/opcodes.h"
#include "driver/driver.h"
#include "vm/vm.h"

namespace ember::vm {
inline std::ostream& operator<<(std::ostream& o, ErrorKind k) { return o << error_kind_name(k); }
}  // namespace ember::vm

namespace th {

struct RunOut {
    bool compiled = false;
    std::string diagnostics;
    std::string out;
    ember::vm::RunResult result;
};

inline RunOut run_source(const std::string& src, ember::vm::VMOptions opts = {}, int opt_level = 1,
                         const std::string& input = "") {
    RunOut r;
    ember::CompileOptions co;
    co.opt_level = opt_level;
    auto c = ember::compile("test.em", src, co);
    r.diagnostics = c->diagnostics;
    if (!c->ok) return r;
    r.compiled = true;
    std::ostringstream out;
    std::istringstream in(input);
    ember::vm::VM machine(c->bytecode, opts);
    machine.set_output(&out);
    machine.set_input(&in);
    r.result = machine.run();
    r.out = out.str();
    return r;
}

inline std::string compile_errors(const std::string& src) {
    auto c = ember::compile("test.em", src, ember::CompileOptions{});
    return c->ok ? std::string() : c->diagnostics;
}

// Tiny assembler for hand-written bytecode in verifier/VM tests.
class Asm {
public:
    std::vector<uint8_t> code;
    Asm& op(ember::bc::Opcode o) {
        code.push_back(static_cast<uint8_t>(o));
        return *this;
    }
    Asm& u8(uint8_t v) {
        code.push_back(v);
        return *this;
    }
    Asm& u16(uint16_t v) { return raw(&v, 2); }
    Asm& u32(uint32_t v) { return raw(&v, 4); }
    Asm& i64(int64_t v) { return raw(&v, 8); }
    Asm& raw(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        code.insert(code.end(), b, b + n);
        return *this;
    }
};

// A module with one entry function (also used as init) running `code`.
inline ember::bc::Module single(std::vector<uint8_t> code, uint32_t max_stack = 8,
                                std::vector<ember::Kind> locals = {}) {
    ember::bc::Module m;
    ember::bc::Function f;
    f.name = "main";
    f.ret_kind = ember::Kind::Unit;
    f.local_kinds = locals;
    f.local_names.assign(locals.size(), "");
    f.max_stack = max_stack;
    f.code = std::move(code);
    m.functions.push_back(f);
    m.init_function = 0;
    m.main_function = 0;
    return m;
}

}  // namespace th

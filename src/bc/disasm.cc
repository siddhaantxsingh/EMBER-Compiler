#include "bc/disasm.h"

#include <cstdio>
#include <sstream>

#include "bc/opcodes.h"
#include "common/natives.h"

namespace ember::bc {

namespace {

std::string quote(const std::string& s, size_t max = 32) {
    std::string out = "\"";
    for (size_t i = 0; i < s.size() && i < max; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c < 32 || c >= 127) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    if (s.size() > max) out += "...";
    return out + "\"";
}

template <typename V>
std::string name_at(const V& v, size_t i) {
    return i < v.size() ? v[i].name : "?";
}

}  // namespace

std::string disassemble_instruction(const Module& m, const Function& f, uint32_t& pc) {
    std::ostringstream o;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%5u  ", pc);
    o << buf;
    if (pc >= f.code.size()) {
        o << "<end>";
        pc = static_cast<uint32_t>(f.code.size()) + 1;
        return o.str();
    }
    const OpcodeInfo* info = opcode_info_checked(f.code[pc]);
    if (!info) {
        o << "<invalid opcode " << static_cast<int>(f.code[pc]) << ">";
        pc += 1;
        return o.str();
    }
    if (pc + info->size > f.code.size()) {
        o << info->name << " <truncated>";
        pc = static_cast<uint32_t>(f.code.size());
        return o.str();
    }
    const uint8_t* p = f.code.data() + pc + 1;
    Opcode op = static_cast<Opcode>(f.code[pc]);
    std::snprintf(buf, sizeof buf, "%-15s", info->name);
    o << buf;
    // Raw operands.
    std::vector<int64_t> ops;
    double fval = 0;
    size_t off = 0;
    for (const char* c = info->format; *c; ++c) {
        switch (*c) {
            case 'b': ops.push_back(p[off]); off += 1; break;
            case 'h': ops.push_back(read_operand<uint16_t>(p + off)); off += 2; break;
            case 'w': ops.push_back(read_operand<uint32_t>(p + off)); off += 4; break;
            case 'q': ops.push_back(read_operand<int64_t>(p + off)); off += 8; break;
            case 'd': fval = read_operand<double>(p + off); ops.push_back(0); off += 8; break;
        }
    }
    for (size_t i = 0; i < ops.size(); ++i) {
        if (op == Opcode::PUSH_FLOAT) {
            char fb[40];
            std::snprintf(fb, sizeof fb, "%.17g", fval);
            o << " " << fb;
        } else {
            o << " " << ops[i];
        }
    }
    // Symbolic comment.
    std::string c;
    auto idx = [&](size_t i) { return static_cast<size_t>(ops[i]); };
    switch (op) {
        case Opcode::PUSH_STR: c = idx(0) < m.strings.size() ? quote(m.strings[idx(0)]) : "?"; break;
        case Opcode::LOAD_LOCAL:
        case Opcode::STORE_LOCAL:
        case Opcode::CLEAR_LOCAL:
        case Opcode::CLOSE_UPVALUES:
            if (idx(0) < f.local_names.size() && !f.local_names[idx(0)].empty()) c = f.local_names[idx(0)];
            break;
        case Opcode::LOAD_LOCAL2:
        case Opcode::STORE_LOAD: {
            auto nm = [&](size_t k) {
                return k < f.local_names.size() && !f.local_names[k].empty() ? f.local_names[k] : "$" + std::to_string(k);
            };
            c = nm(idx(0)) + ", " + nm(idx(1));
            break;
        }
        case Opcode::LOAD_UPVAL:
        case Opcode::STORE_UPVAL:
            if (idx(0) < f.upvalues.size()) c = f.upvalues[idx(0)].name;
            break;
        case Opcode::LOAD_GLOBAL:
        case Opcode::STORE_GLOBAL: c = name_at(m.globals, idx(0)); break;
        case Opcode::CALL:
        case Opcode::CLOSURE: c = name_at(m.functions, idx(0)); break;
        case Opcode::CALL_NATIVE:
            if (idx(0) < natives().size()) {
                const auto& d = natives()[idx(0)];
                c = std::string(d.module[0] ? d.module + std::string(".") : "") + d.name;
            }
            break;
        case Opcode::MAKE_STRUCT: c = name_at(m.structs, idx(0)); break;
        case Opcode::GET_FIELD:
        case Opcode::SET_FIELD:
            if (idx(0) < m.structs.size() && idx(1) < m.structs[idx(0)].field_names.size())
                c = m.structs[idx(0)].name + "." + m.structs[idx(0)].field_names[idx(1)];
            break;
        case Opcode::MAKE_ARRAY:
        case Opcode::ARRAY_REPEAT:
        case Opcode::ARRAY_GET:
        case Opcode::ARRAY_SET:
        case Opcode::ARRAY_PUSH:
        case Opcode::ARRAY_POP:
            if (ops[0] <= static_cast<int64_t>(Kind::Ref)) c = std::string("elem ") + kind_name(static_cast<Kind>(ops[0]));
            break;
        case Opcode::MAKE_ENUM:
        case Opcode::ENUM_FIELD:
            if (idx(0) < m.enums.size() && idx(1) < m.enums[idx(0)].variants.size())
                c = m.enums[idx(0)].name + "::" + m.enums[idx(0)].variants[idx(1)].name;
            break;
        case Opcode::ENUM_TAG: c = name_at(m.enums, idx(0)); break;
        default: break;
    }
    if (!c.empty()) o << "    ; " << c;
    pc += info->size;
    return o.str();
}

std::string disassemble_function(const Module& m, size_t index) {
    std::ostringstream o;
    const Function& f = m.functions[index];
    o << "function " << index << " " << f.name << " (params " << f.num_params << ", locals " << f.local_kinds.size()
      << ", max_stack " << f.max_stack << ", returns " << kind_name(f.ret_kind) << ")\n";
    if (!f.local_kinds.empty()) {
        o << "  locals:";
        for (size_t i = 0; i < f.local_kinds.size(); ++i) {
            o << " " << i << ":" << kind_name(f.local_kinds[i]);
            if (i < f.local_names.size() && !f.local_names[i].empty()) o << "(" << f.local_names[i] << ")";
        }
        o << "\n";
    }
    if (!f.upvalues.empty()) {
        o << "  upvalues:";
        for (size_t i = 0; i < f.upvalues.size(); ++i) {
            const auto& u = f.upvalues[i];
            o << " " << i << ":" << kind_name(u.kind) << "(" << u.name << " <- " << (u.from_local ? "local " : "upvalue ")
              << u.index << ")";
        }
        o << "\n";
    }
    uint32_t last_line = 0;
    for (uint32_t pc = 0; pc < f.code.size();) {
        LineEntry loc = f.location(pc);
        std::string ins = disassemble_instruction(m, f, pc);
        if (loc.line != last_line && loc.line != 0) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%4u ", loc.line);
            o << buf;
            last_line = loc.line;
        } else {
            o << "   | ";
        }
        o << ins << "\n";
    }
    return o.str();
}

std::string disassemble(const Module& m) {
    std::ostringstream o;
    o << "; Ember bytecode";
    if (!m.source_name.empty()) o << " from " << m.source_name;
    o << "\n; entry: " << name_at(m.functions, m.main_function) << ", globals init: "
      << name_at(m.functions, m.init_function) << "\n";
    for (size_t i = 0; i < m.strings.size(); ++i) o << "string " << i << " " << quote(m.strings[i], 60) << "\n";
    for (size_t i = 0; i < m.structs.size(); ++i) {
        o << "struct " << i << " " << m.structs[i].name << " {";
        for (size_t k = 0; k < m.structs[i].field_kinds.size(); ++k)
            o << (k ? ", " : " ") << m.structs[i].field_names[k] << ": " << kind_name(m.structs[i].field_kinds[k]);
        o << " }\n";
    }
    for (size_t i = 0; i < m.enums.size(); ++i) {
        o << "enum " << i << " " << m.enums[i].name << " {";
        for (size_t k = 0; k < m.enums[i].variants.size(); ++k) {
            const auto& v = m.enums[i].variants[k];
            o << (k ? ", " : " ") << v.name;
            if (!v.field_kinds.empty()) {
                o << "(";
                for (size_t j = 0; j < v.field_kinds.size(); ++j) o << (j ? ", " : "") << kind_name(v.field_kinds[j]);
                o << ")";
            }
        }
        o << " }\n";
    }
    for (size_t i = 0; i < m.signatures.size(); ++i) {
        o << "signature " << i << " (";
        for (size_t k = 0; k < m.signatures[i].params.size(); ++k) o << (k ? ", " : "") << kind_name(m.signatures[i].params[k]);
        o << ") -> " << kind_name(m.signatures[i].ret) << "\n";
    }
    for (size_t i = 0; i < m.globals.size(); ++i)
        o << "global " << i << " " << m.globals[i].name << ": " << kind_name(m.globals[i].kind) << "\n";
    for (size_t i = 0; i < m.functions.size(); ++i) o << "\n" << disassemble_function(m, i);
    return o.str();
}

}  // namespace ember::bc

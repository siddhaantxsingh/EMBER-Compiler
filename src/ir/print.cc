#include <sstream>

#include "ir/ir.h"

namespace ember::ir {

const char* op_name(Op op) {
    switch (op) {
#define X(n) \
    case Op::n: return #n;
        EMBER_IR_OPS(X)
#undef X
    }
    return "?";
}

bool is_terminator(Op op) {
    return op == Op::Jump || op == Op::Branch || op == Op::Return || op == Op::Unreachable;
}

bool is_pure(Op op) {
    switch (op) {
        case Op::ConstInt:
        case Op::ConstFloat:
        case Op::ConstBool:
        case Op::ConstString:
        case Op::ConstUnit:
        case Op::LoadLocal:
        case Op::FAdd:
        case Op::FSub:
        case Op::FMul:
        case Op::FDiv:
        case Op::FNeg:
        case Op::Not:
        case Op::I2F:
        case Op::ICmp:
        case Op::FCmp:
        case Op::BCmp:
        case Op::SCmp:
            return true;
        // Closures allocate but have no observable effect when unused.
        case Op::MakeClosure:
            return true;
        default:
            // LoadUpval/LoadGlobal can trap (uninitialized), integer ops can
            // trap, allocation ops can run out of memory, calls have effects.
            return false;
    }
}

const char* cmp_name(CmpOp c) {
    switch (c) {
        case CmpOp::Eq: return "eq";
        case CmpOp::Ne: return "ne";
        case CmpOp::Lt: return "lt";
        case CmpOp::Le: return "le";
        case CmpOp::Gt: return "gt";
        case CmpOp::Ge: return "ge";
    }
    return "?";
}

namespace {

void print_instr(std::ostringstream& o, const Module& m, const Function& f, const Instr& in) {
    o << "    ";
    if (in.dst != kNoTemp)
        o << "%" << in.dst << ":" << kind_name(f.temp_kinds[static_cast<size_t>(in.dst)]) << " = ";
    o << op_name(in.op);
    auto local_name = [&](int64_t slot) {
        std::string s = "$" + std::to_string(slot);
        if (slot >= 0 && static_cast<size_t>(slot) < f.locals.size() && !f.locals[static_cast<size_t>(slot)].name.empty())
            s += "(" + f.locals[static_cast<size_t>(slot)].name + ")";
        return s;
    };
    switch (in.op) {
        case Op::ConstInt: o << " " << in.imm; break;
        case Op::ConstFloat: o << " " << in.fimm; break;
        case Op::ConstBool: o << " " << (in.imm ? "true" : "false"); break;
        case Op::ConstString: {
            o << " #" << in.imm;
            if (in.imm >= 0 && static_cast<size_t>(in.imm) < m.strings.size()) {
                std::string s = m.strings[static_cast<size_t>(in.imm)];
                if (s.size() > 24) s = s.substr(0, 24) + "...";
                o << " \"";
                for (char c : s) {
                    if (c == '\n') o << "\\n";
                    else if (c == '"') o << "\\\"";
                    else if (static_cast<unsigned char>(c) < 32) o << "\\x" << std::hex << (int)(unsigned char)c << std::dec;
                    else o << c;
                }
                o << "\"";
            }
            break;
        }
        case Op::LoadLocal:
        case Op::StoreLocal:
        case Op::ClearLocal:
        case Op::CloseUpvalues: o << " " << local_name(in.imm); break;
        case Op::LoadUpval:
        case Op::StoreUpval:
            o << " ^" << in.imm;
            if (in.imm >= 0 && static_cast<size_t>(in.imm) < f.upvalue_names.size())
                o << "(" << f.upvalue_names[static_cast<size_t>(in.imm)] << ")";
            break;
        case Op::LoadGlobal:
        case Op::StoreGlobal:
            o << " @" << in.imm;
            if (in.imm >= 0 && static_cast<size_t>(in.imm) < m.globals.size())
                o << "(" << m.globals[static_cast<size_t>(in.imm)].name << ")";
            break;
        case Op::ICmp:
        case Op::FCmp:
        case Op::BCmp:
        case Op::SCmp: o << "." << cmp_name(static_cast<CmpOp>(in.imm)); break;
        case Op::Call:
        case Op::MakeClosure:
            o << " fn" << in.imm;
            if (in.imm >= 0 && static_cast<size_t>(in.imm) < m.functions.size())
                o << "(" << m.functions[static_cast<size_t>(in.imm)].name << ")";
            break;
        case Op::CallIndirect: o << " sig" << in.imm; break;
        case Op::CallNative: o << " native" << in.imm; break;
        case Op::MakeStruct: o << " struct" << in.imm; break;
        case Op::GetField:
        case Op::SetField: o << " struct" << in.imm << "." << in.imm2; break;
        case Op::MakeArray:
        case Op::ArrayRepeat:
        case Op::ArrayGet:
        case Op::ArraySet:
        case Op::ArrayPush:
        case Op::ArrayPop: o << "<" << kind_name(in.elem) << ">"; break;
        case Op::MakeEnum: o << " enum" << in.imm << "::" << in.imm2; break;
        case Op::EnumTag: o << " enum" << in.imm; break;
        case Op::EnumField: o << " enum" << in.imm << "::" << in.imm2 << "." << in.imm3; break;
        case Op::Jump: o << " b" << in.target; break;
        case Op::Branch: o << " b" << in.target << ", b" << in.target2; break;
        default: break;
    }
    for (size_t i = 0; i < in.args.size(); ++i) o << (i == 0 ? " (" : ", ") << "%" << in.args[i];
    if (!in.args.empty()) o << ")";
    o << "\n";
}

}  // namespace

std::string print_function(const Module& m, const Function& f) {
    std::ostringstream o;
    o << "fn " << f.name << "(";
    for (uint32_t i = 0; i < f.num_params; ++i) {
        if (i) o << ", ";
        o << f.locals[i].name << ": " << kind_name(f.param_kinds[i]);
    }
    o << ") -> " << kind_name(f.ret_kind) << " {\n";
    if (!f.upvalue_kinds.empty()) {
        o << "  upvalues:";
        for (size_t i = 0; i < f.upvalue_kinds.size(); ++i)
            o << " ^" << i << "(" << f.upvalue_names[i] << ":" << kind_name(f.upvalue_kinds[i]) << " <- "
              << (f.captures[i].from_local ? "$" : "^") << f.captures[i].index << ")";
        o << "\n";
    }
    o << "  locals:";
    for (size_t i = 0; i < f.locals.size(); ++i)
        o << " $" << i << (f.locals[i].name.empty() ? "" : "(" + f.locals[i].name + ")") << ":"
          << kind_name(f.locals[i].kind);
    o << "\n";
    for (const auto& b : f.blocks) {
        o << "  b" << b.id << ":\n";
        for (const auto& in : b.instrs) print_instr(o, m, f, in);
    }
    o << "}\n";
    return o.str();
}

std::string print_module(const Module& m) {
    std::ostringstream o;
    for (size_t i = 0; i < m.structs.size(); ++i) {
        o << "struct" << i << " " << m.structs[i].name << " {";
        for (size_t k = 0; k < m.structs[i].field_names.size(); ++k)
            o << (k ? ", " : " ") << m.structs[i].field_names[k] << ": " << kind_name(m.structs[i].field_kinds[k]);
        o << " }\n";
    }
    for (size_t i = 0; i < m.enums.size(); ++i) {
        o << "enum" << i << " " << m.enums[i].name << " {";
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
    for (size_t i = 0; i < m.globals.size(); ++i)
        o << "global @" << i << " " << m.globals[i].name << ": " << kind_name(m.globals[i].kind) << "\n";
    for (size_t i = 0; i < m.functions.size(); ++i) {
        o << "\n; fn" << i;
        if (i == m.init_function) o << " (global initializers)";
        if (i == m.main_function) o << " (entry)";
        o << "\n" << print_function(m, m.functions[i]);
    }
    return o.str();
}

}  // namespace ember::ir

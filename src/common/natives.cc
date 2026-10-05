#include "common/natives.h"

namespace ember {

const std::vector<NativeDesc>& natives() {
    using K = Kind;
    static const std::vector<NativeDesc> table = {
        {"", "print", {K::Any}, K::Unit},
        {"", "str", {K::Any}, K::Ref},
        {"", "input", {}, K::Ref},
        {"", "parse_int", {K::Ref}, K::Int},
        {"", "panic", {K::Ref}, K::Unit},
        {"", "assert", {K::Bool, K::Ref}, K::Unit},
        // std modules (order must match sema native_table())
        {"math", "abs", {K::Int}, K::Int},
        {"math", "fabs", {K::Float}, K::Float},
        {"math", "min", {K::Int, K::Int}, K::Int},
        {"math", "max", {K::Int, K::Int}, K::Int},
        {"math", "sqrt", {K::Float}, K::Float},
        {"math", "pow", {K::Float, K::Float}, K::Float},
        {"math", "floor", {K::Float}, K::Float},
        {"math", "ceil", {K::Float}, K::Float},
        {"strings", "substr", {K::Ref, K::Int, K::Int}, K::Ref},
        {"strings", "find", {K::Ref, K::Ref}, K::Int},
        {"strings", "upper", {K::Ref}, K::Ref},
        {"strings", "lower", {K::Ref}, K::Ref},
        {"strings", "split", {K::Ref, K::Ref}, K::Ref},
        {"strings", "join", {K::Ref, K::Ref}, K::Ref},
        {"strings", "char_code", {K::Ref, K::Int}, K::Int},
        {"strings", "from_char_code", {K::Int}, K::Ref},
        {"strings", "repeat", {K::Ref, K::Int}, K::Ref},
        {"time", "clock", {}, K::Float},
    };
    return table;
}

}  // namespace ember

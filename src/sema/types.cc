#include "sema/types.h"

namespace ember {

TypeContext::TypeContext() = default;

Type* TypeContext::array(Type* elem) {
    auto& slot = arrays_[elem];
    if (!slot) {
        slot = std::make_unique<Type>(TypeKind::Array);
        slot->elem = elem;
    }
    return slot.get();
}

Type* TypeContext::func(const std::vector<Type*>& params, Type* ret) {
    auto& slot = funcs_[{params, ret}];
    if (!slot) {
        slot = std::make_unique<Type>(TypeKind::Func);
        slot->params = params;
        slot->ret = ret;
    }
    return slot.get();
}

Type* TypeContext::struct_type(StructDecl* d) {
    auto& slot = nominal_[d];
    if (!slot) {
        slot = std::make_unique<Type>(TypeKind::Struct);
        slot->struct_decl = d;
    }
    return slot.get();
}

Type* TypeContext::enum_type(EnumDecl* d) {
    auto& slot = nominal_[d];
    if (!slot) {
        slot = std::make_unique<Type>(TypeKind::Enum);
        slot->enum_decl = d;
    }
    return slot.get();
}

std::string type_name(const Type* t) {
    if (!t) return "?";
    switch (t->kind) {
        case TypeKind::Error: return "<error>";
        case TypeKind::Int: return "Int";
        case TypeKind::Float: return "Float";
        case TypeKind::Bool: return "Bool";
        case TypeKind::String: return "String";
        case TypeKind::Unit: return "Unit";
        case TypeKind::Array: return "[" + type_name(t->elem) + "]";
        case TypeKind::Func: {
            std::string s = "fn(";
            for (size_t i = 0; i < t->params.size(); ++i) {
                if (i) s += ", ";
                s += type_name(t->params[i]);
            }
            s += ")";
            if (t->ret && t->ret->kind != TypeKind::Unit) s += " -> " + type_name(t->ret);
            return s;
        }
        case TypeKind::Struct: return t->struct_decl->name;
        case TypeKind::Enum: return t->enum_decl->name;
    }
    return "?";
}

}  // namespace ember

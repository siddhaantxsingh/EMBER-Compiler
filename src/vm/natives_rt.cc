#include "vm/natives_rt.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <istream>
#include <ostream>

#include "common/natives.h"
#include "common/numfmt.h"
#include "vm/vm.h"

namespace ember::vm {

namespace {

const std::string& str_arg(const Value& v) {
    if (v.tag != Tag::Ref || v.o->type != ObjType::String) VM::fail(ErrorKind::TypeMismatch, "type mismatch: expected a string");
    return static_cast<StringObj*>(v.o)->s;
}

Value make_str(VM& vm, std::string s) { return Value::ref(vm.new_string(std::move(s))); }

enum : uint16_t {
    kAbs = kFirstModuleNative,
    kFabs,
    kMin,
    kMax,
    kSqrt,
    kPow,
    kFloor,
    kCeil,
    kSubstr,
    kFind,
    kUpper,
    kLower,
    kSplit,
    kJoin,
    kCharCode,
    kFromCharCode,
    kRepeat,
    kClock,
};

}  // namespace

std::string VM::format_value(const Value& v) const {
    std::string s;
    format_into(s, v, 0);
    return s;
}

// Formatting stops as soon as the text passes the string limit (it may be
// a huge or cyclic structure); the error does not depend on where it stopped.
[[noreturn]] static void formatted_too_large(size_t limit) {
    VM::fail(ErrorKind::ValueTooLarge,
             "value too large: formatted value exceeds the limit of " + std::to_string(limit) + " bytes");
}

void VM::format_into(std::string& out, const Value& v, int depth) const {
    if (out.size() > opts_.max_string_bytes) formatted_too_large(opts_.max_string_bytes);
    if (depth > 32) {
        out += "...";
        return;
    }
    switch (v.tag) {
        case Tag::Unit: out += "()"; return;
        case Tag::Bool: out += v.b ? "true" : "false"; return;
        case Tag::Int: out += std::to_string(v.i); return;
        case Tag::Float: out += format_float(v.f); return;
        case Tag::Uninit: out += "<uninitialized>"; return;
        case Tag::Ref: break;
    }
    switch (v.o->type) {
        case ObjType::String: out += static_cast<StringObj*>(v.o)->s; break;
        case ObjType::Array: {
            auto* a = static_cast<ArrayObj*>(v.o);
            out += "[";
            for (size_t i = 0; i < a->items.size(); ++i) {
                if (i) out += ", ";
                format_into(out, a->items[i], depth + 1);
                if (out.size() > opts_.max_string_bytes) formatted_too_large(opts_.max_string_bytes);
            }
            out += "]";
            break;
        }
        case ObjType::Struct: {
            auto* s = static_cast<StructObj*>(v.o);
            const auto& def = mod_.structs[s->sid];
            out += def.name;
            if (s->fields.empty()) {
                out += " {}";
                break;
            }
            out += " { ";
            for (size_t i = 0; i < s->fields.size(); ++i) {
                if (i) out += ", ";
                out += def.field_names[i] + ": ";
                format_into(out, s->fields[i], depth + 1);
            }
            out += " }";
            break;
        }
        case ObjType::Enum: {
            auto* e = static_cast<EnumObj*>(v.o);
            const auto& def = mod_.enums[e->eid];
            out += def.name + "::" + def.variants[e->variant].name;
            if (!e->fields.empty()) {
                out += "(";
                for (size_t i = 0; i < e->fields.size(); ++i) {
                    if (i) out += ", ";
                    format_into(out, e->fields[i], depth + 1);
                }
                out += ")";
            }
            break;
        }
        case ObjType::Closure: out += "<fn " + mod_.functions[static_cast<ClosureObj*>(v.o)->fn].name + ">"; break;
        case ObjType::Upvalue: out += "<upvalue>"; break;
    }
    if (out.size() > opts_.max_string_bytes) formatted_too_large(opts_.max_string_bytes);
}

Value call_native(VM& vm, uint16_t id, Value* a) {
    switch (id) {
        case kNativePrint: {
            std::string s = vm.format_value(a[0]);
            s += '\n';
            vm.out().write(s.data(), static_cast<std::streamsize>(s.size()));
            return Value::unit();
        }
        case kNativeStr: return make_str(vm, vm.format_value(a[0]));
        case kNativeInput: {
            vm.out().flush();
            std::string line;
            if (!std::getline(vm.in(), line)) line.clear();
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return make_str(vm, std::move(line));
        }
        case kNativeParseInt: {
            const std::string& s = str_arg(a[0]);
            auto v = parse_int(s);
            if (!v) {
                std::string shown = s.size() > 40 ? s.substr(0, 40) + "..." : s;
                VM::fail(ErrorKind::InvalidConversion, "invalid conversion: \"" + shown + "\" is not an integer");
            }
            return Value::integer(*v);
        }
        case kNativePanic: VM::fail(ErrorKind::Panic, "panic: " + str_arg(a[0]));
        case kNativeAssert:
            if (!a[0].b) VM::fail(ErrorKind::AssertionFailed, "assertion failed: " + str_arg(a[1]));
            return Value::unit();
        case kAbs:
            if (a[0].i == INT64_MIN) VM::fail(ErrorKind::IntegerOverflow);
            return Value::integer(a[0].i < 0 ? -a[0].i : a[0].i);
        case kFabs: return Value::flt(std::fabs(a[0].f));
        case kMin: return Value::integer(std::min(a[0].i, a[1].i));
        case kMax: return Value::integer(std::max(a[0].i, a[1].i));
        case kSqrt: return Value::flt(std::sqrt(a[0].f));
        case kPow: return Value::flt(std::pow(a[0].f, a[1].f));
        case kFloor: return Value::flt(std::floor(a[0].f));
        case kCeil: return Value::flt(std::ceil(a[0].f));
        case kSubstr: {
            const std::string& s = str_arg(a[0]);
            int64_t start = a[1].i, len = a[2].i;
            if (start < 0 || len < 0 || static_cast<uint64_t>(start) > s.size() ||
                static_cast<uint64_t>(len) > s.size() - static_cast<uint64_t>(start))
                VM::fail(ErrorKind::IndexOutOfBounds, "index out of bounds: substr(" + std::to_string(start) + ", " +
                                                          std::to_string(len) + ") of a string of length " +
                                                          std::to_string(s.size()));
            return make_str(vm, s.substr(static_cast<size_t>(start), static_cast<size_t>(len)));
        }
        case kFind: {
            size_t p = str_arg(a[0]).find(str_arg(a[1]));
            return Value::integer(p == std::string::npos ? -1 : static_cast<int64_t>(p));
        }
        case kUpper:
        case kLower: {
            std::string s = str_arg(a[0]);
            for (char& c : s) {
                if (id == kUpper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                if (id == kLower && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return make_str(vm, std::move(s));
        }
        case kSplit: {
            // Copy the inputs: allocation below may collect, but a[] stays
            // rooted on the VM stack, so references would also be fine.
            std::string s = str_arg(a[0]);
            std::string sep = str_arg(a[1]);
            std::vector<std::string> parts;
            if (sep.empty()) {
                for (char c : s) parts.emplace_back(1, c);
            } else {
                size_t pos = 0;
                while (true) {
                    size_t q = s.find(sep, pos);
                    if (q == std::string::npos) {
                        parts.push_back(s.substr(pos));
                        break;
                    }
                    parts.push_back(s.substr(pos, q - pos));
                    pos = q + sep.size();
                }
            }
            ArrayObj* arr = vm.new_array(Kind::Ref, parts.size());
            vm.push_temp_root(Value::ref(arr));
            for (auto& p : parts) arr->items.push_back(make_str(vm, std::move(p)));
            vm.pop_temp_root();
            return Value::ref(arr);
        }
        case kJoin: {
            if (a[0].tag != Tag::Ref || a[0].o->type != ObjType::Array)
                VM::fail(ErrorKind::TypeMismatch, "type mismatch: expected an array");
            auto* arr = static_cast<ArrayObj*>(a[0].o);
            const std::string& sep = str_arg(a[1]);
            std::string out;
            for (size_t i = 0; i < arr->items.size(); ++i) {
                if (i) out += sep;
                out += str_arg(arr->items[i]);
                vm.check_string_size(out.size());
            }
            return make_str(vm, std::move(out));
        }
        case kCharCode: {
            const std::string& s = str_arg(a[0]);
            int64_t i = a[1].i;
            if (i < 0 || static_cast<uint64_t>(i) >= s.size())
                VM::fail(ErrorKind::IndexOutOfBounds, "index out of bounds: index " + std::to_string(i) +
                                                          " for length " + std::to_string(s.size()));
            return Value::integer(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
        }
        case kFromCharCode: {
            int64_t c = a[0].i;
            if (c < 0 || c > 255)
                VM::fail(ErrorKind::InvalidArgument, "invalid argument: character code " + std::to_string(c) + " is not in 0..255");
            return make_str(vm, std::string(1, static_cast<char>(c)));
        }
        case kRepeat: {
            const std::string& s = str_arg(a[0]);
            int64_t n = a[1].i;
            if (n < 0) VM::fail(ErrorKind::InvalidArgument, "invalid argument: negative repeat count " + std::to_string(n));
            if (!s.empty() && static_cast<uint64_t>(n) > vm.max_string_bytes() / s.size())
                VM::fail(ErrorKind::ValueTooLarge, "value too large: repeated string would exceed the limit of " +
                                                       std::to_string(vm.max_string_bytes()) + " bytes");
            std::string out;
            out.reserve(s.size() * static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i) out += s;
            return make_str(vm, std::move(out));
        }
        case kClock: {
            double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            return Value::flt(now - vm.start_time());
        }
        default: VM::fail(ErrorKind::Internal, "internal error: unknown native " + std::to_string(id));
    }
}

}  // namespace ember::vm

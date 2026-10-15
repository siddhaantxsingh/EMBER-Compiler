#include "native/runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <ostream>

#include "common/numfmt.h"

// Frame bookkeeping read and written directly by generated code.
// ember_rt_frame_fn[d] / ember_rt_frame_loc[d] describe the frame at depth d
// (1-based); loc packs (line << 32) | column of the instruction the frame is
// executing (a call site or a trapping instruction).
extern "C" {
int64_t ember_rt_depth = 0;
int64_t ember_rt_max_depth = 0;
int32_t* ember_rt_frame_fn = nullptr;
int64_t* ember_rt_frame_loc = nullptr;
}

namespace ember::native {

namespace {

struct State {
    const RtConfig* cfg = nullptr;
    std::jmp_buf jb;
    RtOutcome outcome;
    std::vector<char*> chunks;
    char* bump = nullptr;
    size_t bump_left = 0;
    uint64_t arena_bytes = 0;
    double start_time = 0;
};

State g;

constexpr size_t kChunk = 1u << 20;

// Records the error and the current trace. Returns normally so that every
// std::string it creates is destroyed before the caller longjmps.
void set_error(const std::string& msg) {
    g.outcome.ok = false;
    g.outcome.message = msg;
    g.outcome.trace.clear();
    for (int64_t d = ember_rt_depth; d >= 1; --d) {
        RtTraceEntry t;
        int32_t fn = ember_rt_frame_fn[d];
        if (fn >= 0 && static_cast<size_t>(fn) < g.cfg->function_names.size()) t.function = g.cfg->function_names[static_cast<size_t>(fn)];
        uint64_t loc = static_cast<uint64_t>(ember_rt_frame_loc[d]);
        t.line = static_cast<uint32_t>(loc >> 32);
        t.column = static_cast<uint32_t>(loc & 0xffffffffu);
        g.outcome.trace.push_back(std::move(t));
    }
}

[[noreturn]] void raise() { std::longjmp(g.jb, 1); }

void check_size(size_t n) {
    if (n > g.cfg->max_string_bytes) {
        set_error("value too large: string of " + std::to_string(n) + " bytes exceeds the limit of " +
                  std::to_string(g.cfg->max_string_bytes));
        raise();
    }
}

void* alloc_bytes(size_t want) {
    size_t bytes = (want + 7) & ~size_t{7};
    if (g.arena_bytes + bytes > g.cfg->arena_limit_bytes) {
        g.outcome.backend_limit = true;
        set_error("native backend limit: string arena exceeds " + std::to_string(g.cfg->arena_limit_bytes) +
                  " bytes (the native backend has no garbage collector)");
        raise();
    }
    char* p;
    if (bytes > kChunk / 4) {
        p = static_cast<char*>(std::malloc(bytes));
        if (!p) std::abort();
        g.chunks.push_back(p);
    } else {
        if (bytes > g.bump_left) {
            g.bump = static_cast<char*>(std::malloc(kChunk));
            if (!g.bump) std::abort();
            g.chunks.push_back(g.bump);
            g.bump_left = kChunk;
        }
        p = g.bump;
        g.bump += bytes;
        g.bump_left -= bytes;
    }
    g.arena_bytes += bytes;
    return p;
}

EmStr* alloc_str(size_t n) {
    check_size(n);
    auto* s = static_cast<EmStr*>(alloc_bytes(2 * sizeof(int64_t) + n + 1));
    s->tag = kTagString;
    s->len = static_cast<int64_t>(n);
    s->data[n] = '\0';
    return s;
}

EmStr* make_str(const char* data, size_t n) {
    EmStr* s = alloc_str(n);
    if (n) std::memcpy(s->data, data, n);
    return s;
}

EmStr* make_str(const std::string& s) { return make_str(s.data(), s.size()); }

void check_array(size_t n) {
    if (n > g.cfg->max_array_length) {
        set_error("value too large: array of " + std::to_string(n) + " elements exceeds the limit of " +
                  std::to_string(g.cfg->max_array_length));
        raise();
    }
}

EmArr* alloc_arr(int64_t elem_kind, size_t n) {
    check_array(n);
    auto* a = static_cast<EmArr*>(alloc_bytes(sizeof(EmArr)));
    a->tag = kTagArray | (elem_kind << 8);
    a->len = static_cast<int64_t>(n);
    a->cap = static_cast<int64_t>(n);
    a->data = n ? static_cast<int64_t*>(alloc_bytes(n * sizeof(int64_t))) : nullptr;
    return a;
}

[[noreturn]] void formatted_too_large() {
    set_error("value too large: formatted value exceeds the limit of " + std::to_string(g.cfg->max_string_bytes) + " bytes");
    raise();
}

// Mirrors VM::format_into for the values the backend supports. Returns
// false (after recording the error) when the text passes the limit, so the
// caller can drop its std::string before raising.
bool format_slot(std::string& out, int64_t kind, int64_t bits, int depth);

bool format_ref(std::string& out, const void* p, int depth) {
    if (out.size() > g.cfg->max_string_bytes) return false;
    if (depth > 32) {
        out += "...";
        return true;
    }
    auto tag = *static_cast<const int64_t*>(p);
    if ((tag & 0xff) == kTagStruct) {
        const auto& def = g.cfg->structs[static_cast<size_t>(tag >> 8)];
        auto* w = static_cast<const int64_t*>(p);
        out += def.name;
        if (def.field_kinds.empty()) {
            out += " {}";
        } else {
            out += " { ";
            for (size_t i = 0; i < def.field_kinds.size(); ++i) {
                if (i) out += ", ";
                out += def.field_names[i] + ": ";
                if (!format_slot(out, def.field_kinds[i], w[1 + i], depth + 1)) return false;
            }
            out += " }";
        }
    } else if ((tag & 0xff) == kTagEnum) {
        const auto& def = g.cfg->enums[static_cast<size_t>(tag >> 8)];
        auto* w = static_cast<const int64_t*>(p);
        auto v = static_cast<size_t>(w[1]);
        out += def.name + "::" + def.variant_names[v];
        const auto& fk = def.field_kinds[v];
        if (!fk.empty()) {
            out += "(";
            for (size_t i = 0; i < fk.size(); ++i) {
                if (i) out += ", ";
                if (!format_slot(out, fk[i], w[2 + i], depth + 1)) return false;
            }
            out += ")";
        }
    } else if ((tag & 0xff) == kTagClosure) {
        auto fi = static_cast<size_t>(tag >> 8);
        out += "<fn " + (fi < g.cfg->function_names.size() ? g.cfg->function_names[fi] : std::string("?")) + ">";
    } else if ((tag & 0xff) == kTagString) {
        auto* s = static_cast<const EmStr*>(p);
        out.append(s->data, static_cast<size_t>(s->len));
    } else {
        auto* a = static_cast<const EmArr*>(p);
        int64_t ek = tag >> 8;
        out += "[";
        for (int64_t i = 0; i < a->len; ++i) {
            if (i) out += ", ";
            if (!format_slot(out, ek, a->data[i], depth + 1)) return false;
            if (out.size() > g.cfg->max_string_bytes) return false;
        }
        out += "]";
    }
    return out.size() <= g.cfg->max_string_bytes;
}

bool format_slot(std::string& out, int64_t kind, int64_t bits, int depth) {
    if (out.size() > g.cfg->max_string_bytes) return false;
    if (depth > 32) {
        out += "...";
        return true;
    }
    switch (kind) {
        case 0: out += "()"; break;
        case 1: out += bits ? "true" : "false"; break;
        case 2: out += std::to_string(bits); break;
        case 3: {
            double d;
            std::memcpy(&d, &bits, sizeof d);
            out += ember::format_float(d);
            break;
        }
        default: return format_ref(out, reinterpret_cast<const void*>(static_cast<intptr_t>(bits)), depth);
    }
    return out.size() <= g.cfg->max_string_bytes;
}

void write_out(const char* p, size_t n) { g.cfg->out->write(p, static_cast<std::streamsize>(n)); }

}  // namespace

}  // namespace ember::native

using ember::native::EmStr;
using namespace ember::native;

extern "C" {

[[noreturn]] void ember_rt_trap(int32_t kind, int64_t arg) {
    switch (kind) {
        case kTrapDivZero: set_error("division by zero"); break;
        case kTrapOverflow: set_error("integer overflow"); break;
        case kTrapUnreachable: set_error("internal error: reached unreachable code"); break;
        case kTrapUninitGlobal: {
            const auto& names = g.cfg->global_names;
            set_error("uninitialized global: '" +
                      (arg >= 0 && static_cast<size_t>(arg) < names.size() ? names[static_cast<size_t>(arg)] : std::string("?")) +
                      "' was read before its initializer ran");
            break;
        }
        case kTrapWrongVariant: set_error("type mismatch: wrong enum variant"); break;
        case kTrapStackOverflow:
            set_error("stack overflow (call depth limit " + std::to_string(g.cfg->max_call_depth) + ")");
            break;
        default: set_error("internal error: unknown trap"); break;
    }
    raise();
}

[[noreturn]] void ember_rt_trap_f2i(double f) {
    set_error("invalid conversion: " + ember::format_float(f) + " does not fit in Int");
    raise();
}

void ember_rt_print_int(int64_t v) {
    char buf[32];
    int n = std::snprintf(buf, sizeof buf, "%lld\n", static_cast<long long>(v));
    write_out(buf, static_cast<size_t>(n));
}

void ember_rt_print_float(double v) {
    std::string s = ember::format_float(v);
    s += '\n';
    write_out(s.data(), s.size());
}

void ember_rt_print_bool(int32_t b) { write_out(b ? "true\n" : "false\n", b ? 5 : 6); }

void ember_rt_print_unit() { write_out("()\n", 3); }

void ember_rt_print_str(const EmStr* s) {
    // Same limit check as VM::format_value (the string already exists, so
    // it is within the limit) followed by the newline.
    write_out(s->data, static_cast<size_t>(s->len));
    write_out("\n", 1);
}

EmStr* ember_rt_str_int(int64_t v) {
    char buf[32];
    int n = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
    return make_str(buf, static_cast<size_t>(n));
}

EmStr* ember_rt_str_float(double v) {
    // Copy into a stack buffer so no std::string is alive if make_str raises.
    char buf[64];
    size_t n;
    {
        std::string s = ember::format_float(v);
        n = std::min(s.size(), sizeof buf);
        std::memcpy(buf, s.data(), n);
    }
    return make_str(buf, n);
}

EmStr* ember_rt_str_bool(int32_t b) { return b ? make_str("true", 4) : make_str("false", 5); }

EmStr* ember_rt_str_unit() { return make_str("()", 2); }

EmStr* ember_rt_concat(const EmStr* a, const EmStr* b) {
    size_t n = static_cast<size_t>(a->len) + static_cast<size_t>(b->len);
    EmStr* r = alloc_str(n);
    std::memcpy(r->data, a->data, static_cast<size_t>(a->len));
    std::memcpy(r->data + a->len, b->data, static_cast<size_t>(b->len));
    return r;
}

static void index_error(int64_t i, size_t len) {
    set_error("index out of bounds: index " + std::to_string(i) + " for length " + std::to_string(len));
}

EmStr* ember_rt_sindex(const EmStr* s, int64_t i) {
    if (i < 0 || i >= s->len) {
        index_error(i, static_cast<size_t>(s->len));
        raise();
    }
    return make_str(s->data + i, 1);
}

int32_t ember_rt_scmp(const EmStr* a, const EmStr* b) {
    size_t n = static_cast<size_t>(std::min(a->len, b->len));
    int c = n ? std::memcmp(a->data, b->data, n) : 0;
    if (c != 0) return c < 0 ? -1 : 1;
    if (a->len == b->len) return 0;
    return a->len < b->len ? -1 : 1;
}

EmStr* ember_rt_input() {
    g.cfg->out->flush();
    EmStr* r;
    {
        std::string line;
        if (!std::getline(*g.cfg->in, line)) line.clear();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > g.cfg->max_string_bytes) {
            set_error("value too large: string of " + std::to_string(line.size()) + " bytes exceeds the limit of " +
                      std::to_string(g.cfg->max_string_bytes));
            line.clear();
            line.shrink_to_fit();
            r = nullptr;
        } else {
            r = make_str(line);
        }
    }
    if (!r) raise();
    return r;
}

int64_t ember_rt_parse_int(const EmStr* s) {
    auto v = ember::parse_int(std::string_view(s->data, static_cast<size_t>(s->len)));
    if (!v) {
        {
            std::string str(s->data, static_cast<size_t>(s->len));
            std::string shown = str.size() > 40 ? str.substr(0, 40) + "..." : str;
            set_error("invalid conversion: \"" + shown + "\" is not an integer");
        }
        raise();
    }
    return *v;
}

[[noreturn]] void ember_rt_panic(const EmStr* s) {
    set_error("panic: " + std::string(s->data, static_cast<size_t>(s->len)));
    raise();
}

void ember_rt_assert(int32_t ok, const EmStr* s) {
    if (ok) return;
    set_error("assertion failed: " + std::string(s->data, static_cast<size_t>(s->len)));
    raise();
}

void ember_rt_print_ref(const void* p) {
    if (*static_cast<const int64_t*>(p) == kTagString) {
        ember_rt_print_str(static_cast<const EmStr*>(p));
        return;
    }
    bool ok;
    {
        std::string s;
        ok = format_ref(s, p, 0);
        if (ok) {
            s += '\n';
            write_out(s.data(), s.size());
        }
    }
    if (!ok) formatted_too_large();
}

const void* ember_rt_str_ref(const void* p) {
    if (*static_cast<const int64_t*>(p) == kTagString) return p;
    std::string* s = new std::string();
    bool ok = format_ref(*s, p, 0);
    EmStr* r = nullptr;
    if (ok) {
        // Copy out before make_str can raise, so nothing leaks.
        size_t n = s->size();
        char* tmp = static_cast<char*>(alloc_bytes(n + 1));
        std::memcpy(tmp, s->data(), n);
        delete s;
        return make_str(tmp, n);
    }
    delete s;
    formatted_too_large();
    return r;
}

void* ember_rt_alloc_obj(int64_t tag, int64_t words) {
    auto* p = static_cast<int64_t*>(alloc_bytes(static_cast<size_t>(words) * sizeof(int64_t)));
    p[0] = tag;
    return p;
}

void* ember_rt_alloc_cell(int64_t bits, int64_t init) {
    auto* p = static_cast<int64_t*>(alloc_bytes(2 * sizeof(int64_t)));
    p[0] = bits;
    p[1] = init;
    return p;
}

[[noreturn]] void ember_rt_trap_uninit(const char* name) {
    set_error(std::string("uninitialized variable: '") + name + "' was used before its declaration ran");
    raise();
}

void* ember_rt_array_new(int64_t elem_kind, int64_t n) { return alloc_arr(elem_kind, static_cast<size_t>(n)); }

void* ember_rt_array_repeat(int64_t elem_kind, int64_t bits, int64_t n) {
    if (n < 0) {
        set_error("invalid argument: negative array length " + std::to_string(n));
        raise();
    }
    EmArr* a = alloc_arr(elem_kind, static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) a->data[i] = bits;
    return a;
}

void ember_rt_index_error(int64_t i, int64_t len) {
    index_error(i, static_cast<size_t>(len));
    raise();
}

void ember_rt_array_push(EmArr* a, int64_t bits) {
    check_array(static_cast<size_t>(a->len) + 1);
    if (a->len == a->cap) {
        int64_t ncap = a->cap < 4 ? 4 : a->cap * 2;
        auto* nd = static_cast<int64_t*>(alloc_bytes(static_cast<size_t>(ncap) * sizeof(int64_t)));
        if (a->len) std::memcpy(nd, a->data, static_cast<size_t>(a->len) * sizeof(int64_t));
        a->data = nd;
        a->cap = ncap;
    }
    a->data[a->len++] = bits;
}

int64_t ember_rt_array_pop(EmArr* a) {
    if (a->len == 0) {
        set_error("empty array: pop from an empty array");
        raise();
    }
    return a->data[--a->len];
}

void* ember_rt_split(const EmStr* s, const EmStr* sep) {
    std::string_view str(s->data, static_cast<size_t>(s->len)), sp(sep->data, static_cast<size_t>(sep->len));
    // Count first, then allocate: no std containers alive across a raise.
    size_t count = 0;
    if (sp.empty()) count = str.size();
    else {
        size_t pos = 0;
        while (true) {
            size_t q = str.find(sp, pos);
            ++count;
            if (q == std::string_view::npos) break;
            pos = q + sp.size();
        }
    }
    EmArr* a = alloc_arr(4, count);
    if (sp.empty()) {
        for (size_t i = 0; i < count; ++i) a->data[i] = reinterpret_cast<intptr_t>(make_str(str.data() + i, 1));
    } else {
        size_t pos = 0, k = 0;
        while (true) {
            size_t q = str.find(sp, pos);
            size_t end = q == std::string_view::npos ? str.size() : q;
            a->data[k++] = reinterpret_cast<intptr_t>(make_str(str.data() + pos, end - pos));
            if (q == std::string_view::npos) break;
            pos = q + sp.size();
        }
    }
    return a;
}

EmStr* ember_rt_join(const EmArr* a, const EmStr* sep) {
    size_t total = 0;
    for (int64_t i = 0; i < a->len; ++i) {
        if (i) total += static_cast<size_t>(sep->len);
        total += static_cast<size_t>(reinterpret_cast<const EmStr*>(static_cast<intptr_t>(a->data[i]))->len);
        check_size(total);
    }
    EmStr* r = alloc_str(total);
    size_t at = 0;
    for (int64_t i = 0; i < a->len; ++i) {
        if (i) {
            std::memcpy(r->data + at, sep->data, static_cast<size_t>(sep->len));
            at += static_cast<size_t>(sep->len);
        }
        auto* e = reinterpret_cast<const EmStr*>(static_cast<intptr_t>(a->data[i]));
        std::memcpy(r->data + at, e->data, static_cast<size_t>(e->len));
        at += static_cast<size_t>(e->len);
    }
    return r;
}

double ember_rt_pow(double a, double b) { return std::pow(a, b); }
double ember_rt_floor(double a) { return std::floor(a); }
double ember_rt_ceil(double a) { return std::ceil(a); }
double ember_rt_trunc(double a) { return std::trunc(a); }
double ember_rt_sqrt(double a) { return std::sqrt(a); }

double ember_rt_clock() {
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return now - g.start_time;
}

EmStr* ember_rt_substr(const EmStr* s, int64_t start, int64_t len) {
    uint64_t size = static_cast<uint64_t>(s->len);
    if (start < 0 || len < 0 || static_cast<uint64_t>(start) > size || static_cast<uint64_t>(len) > size - static_cast<uint64_t>(start)) {
        set_error("index out of bounds: substr(" + std::to_string(start) + ", " + std::to_string(len) +
                  ") of a string of length " + std::to_string(size));
        raise();
    }
    return make_str(s->data + start, static_cast<size_t>(len));
}

int64_t ember_rt_find(const EmStr* s, const EmStr* sub) {
    std::string_view a(s->data, static_cast<size_t>(s->len)), b(sub->data, static_cast<size_t>(sub->len));
    size_t p = a.find(b);
    return p == std::string_view::npos ? -1 : static_cast<int64_t>(p);
}

static EmStr* map_case(const EmStr* s, bool upper) {
    EmStr* r = alloc_str(static_cast<size_t>(s->len));
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[i];
        if (upper && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (!upper && c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        r->data[i] = c;
    }
    return r;
}

EmStr* ember_rt_upper(const EmStr* s) { return map_case(s, true); }
EmStr* ember_rt_lower(const EmStr* s) { return map_case(s, false); }

int64_t ember_rt_char_code(const EmStr* s, int64_t i) {
    if (i < 0 || i >= s->len) {
        index_error(i, static_cast<size_t>(s->len));
        raise();
    }
    return static_cast<unsigned char>(s->data[i]);
}

EmStr* ember_rt_from_char_code(int64_t c) {
    if (c < 0 || c > 255) {
        set_error("invalid argument: character code " + std::to_string(c) + " is not in 0..255");
        raise();
    }
    char ch = static_cast<char>(c);
    return make_str(&ch, 1);
}

EmStr* ember_rt_repeat(const EmStr* s, int64_t n) {
    if (n < 0) {
        set_error("invalid argument: negative repeat count " + std::to_string(n));
        raise();
    }
    size_t len = static_cast<size_t>(s->len);
    if (len != 0 && static_cast<uint64_t>(n) > g.cfg->max_string_bytes / len) {
        set_error("value too large: repeated string would exceed the limit of " + std::to_string(g.cfg->max_string_bytes) +
                  " bytes");
        raise();
    }
    EmStr* r = alloc_str(len * static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) std::memcpy(r->data + static_cast<size_t>(i) * len, s->data, len);
    return r;
}

}  // extern "C"

namespace ember::native {

RtOutcome run_entry(const RtConfig& cfg, EntryFn init, EntryFn main_fn) {
    g.cfg = &cfg;
    g.outcome = RtOutcome{};
    g.chunks.clear();
    g.bump = nullptr;
    g.bump_left = 0;
    g.arena_bytes = 0;
    g.start_time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::vector<int32_t> fns(static_cast<size_t>(cfg.max_call_depth) + 2, -1);
    std::vector<int64_t> locs(static_cast<size_t>(cfg.max_call_depth) + 2, 0);
    ember_rt_depth = 0;
    ember_rt_max_depth = cfg.max_call_depth;
    ember_rt_frame_fn = fns.data();
    ember_rt_frame_loc = locs.data();
    if (setjmp(g.jb) == 0) {
        init();
        main_fn();
    }
    cfg.out->flush();
    for (char* c : g.chunks) std::free(c);
    g.chunks.clear();
    g.outcome.arena_bytes = g.arena_bytes;
    ember_rt_frame_fn = nullptr;
    ember_rt_frame_loc = nullptr;
    ember_rt_depth = 0;
    RtOutcome r = std::move(g.outcome);
    g.outcome = RtOutcome{};
    g.cfg = nullptr;
    return r;
}

std::string render_error(const RtOutcome& r, const std::string& source_name) {
    if (r.ok) return "";
    std::string s = (r.backend_limit ? "NativeBackendError: " : "RuntimeError: ") + r.message + "\n";
    auto line = [&](const RtTraceEntry& t) {
        s += "  at " + t.function + " (" + source_name;
        if (t.line) s += ":" + std::to_string(t.line) + ":" + std::to_string(t.column);
        s += ")\n";
    };
    const auto& trace = r.trace;
    if (trace.size() <= 20) {
        for (const auto& t : trace) line(t);
    } else {
        for (size_t i = 0; i < 10; ++i) line(trace[i]);
        s += "  ... " + std::to_string(trace.size() - 15) + " more frames ...\n";
        for (size_t i = trace.size() - 5; i < trace.size(); ++i) line(trace[i]);
    }
    return s;
}

const std::vector<RtSymbol>& runtime_symbols() {
#define SYM(n) RtSymbol{#n, reinterpret_cast<void*>(&n)}
    static const std::vector<RtSymbol> syms = {
        SYM(ember_rt_depth),        SYM(ember_rt_max_depth),  SYM(ember_rt_frame_fn),   SYM(ember_rt_frame_loc),
        SYM(ember_rt_trap),         SYM(ember_rt_trap_f2i),   SYM(ember_rt_print_int),  SYM(ember_rt_print_float),
        SYM(ember_rt_print_bool),   SYM(ember_rt_print_unit), SYM(ember_rt_print_str),  SYM(ember_rt_str_int),
        SYM(ember_rt_str_float),    SYM(ember_rt_str_bool),   SYM(ember_rt_str_unit),   SYM(ember_rt_concat),
        SYM(ember_rt_sindex),       SYM(ember_rt_scmp),       SYM(ember_rt_input),      SYM(ember_rt_parse_int),
        SYM(ember_rt_panic),        SYM(ember_rt_assert),     SYM(ember_rt_pow),        SYM(ember_rt_floor),
        SYM(ember_rt_ceil),         SYM(ember_rt_trunc),      SYM(ember_rt_sqrt),       SYM(ember_rt_clock),
        SYM(ember_rt_substr),       SYM(ember_rt_find),       SYM(ember_rt_upper),      SYM(ember_rt_lower),
        SYM(ember_rt_char_code),    SYM(ember_rt_from_char_code), SYM(ember_rt_repeat),
        SYM(ember_rt_print_ref),    SYM(ember_rt_str_ref),    SYM(ember_rt_array_new),  SYM(ember_rt_array_repeat),
        SYM(ember_rt_index_error),  SYM(ember_rt_array_push), SYM(ember_rt_array_pop),  SYM(ember_rt_split),
        SYM(ember_rt_join),         SYM(ember_rt_alloc_obj),   SYM(ember_rt_alloc_cell), SYM(ember_rt_trap_uninit),
    };
#undef SYM
    return syms;
}

}  // namespace ember::native

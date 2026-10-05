#include "bc/format.h"

#include <array>
#include <bit>
#include <cstring>

namespace ember::bc {

static_assert(std::endian::native == std::endian::little,
              "the bytecode encoder and VM operand reads assume a little-endian host");

uint32_t crc32(const uint8_t* data, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

namespace {

class Writer {
public:
    std::vector<uint8_t> out;
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { raw(&v, 2); }
    void u32(uint32_t v) { raw(&v, 4); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    void kind(Kind k) { u8(static_cast<uint8_t>(k)); }
    void raw(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    }
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint8_t u8() { need(1); return p_[pos_++]; }
    uint16_t u16() { need(2); uint16_t v; std::memcpy(&v, p_ + pos_, 2); pos_ += 2; return v; }
    uint32_t u32() { need(4); uint32_t v; std::memcpy(&v, p_ + pos_, 4); pos_ += 4; return v; }
    std::string str(const char* what) {
        uint32_t len = u32();
        if (len > remaining()) fail(std::string(what) + " length exceeds the file");
        std::string s(reinterpret_cast<const char*>(p_ + pos_), len);
        pos_ += len;
        return s;
    }
    Kind kind(bool allow_any = false) {
        uint8_t k = u8();
        if (k > static_cast<uint8_t>(Kind::Ref) && !(allow_any && k == static_cast<uint8_t>(Kind::Any)))
            fail("invalid value kind " + std::to_string(k));
        return static_cast<Kind>(k);
    }
    // A count of items each at least min_bytes long must fit in the rest of
    // the file: this bounds allocations by the input size.
    uint32_t count(const char* what, size_t min_bytes) {
        uint32_t c = u32();
        if (min_bytes > 0 && c > remaining() / min_bytes) fail(std::string("too many ") + what);
        return c;
    }
    size_t remaining() const { return n_ - pos_; }
    size_t pos() const { return pos_; }
    const uint8_t* here() const { return p_ + pos_; }
    void skip(size_t n) { need(n); pos_ += n; }
    [[noreturn]] void fail(const std::string& msg) const {
        throw DecodeError("invalid bytecode at offset " + std::to_string(pos_ + kHeaderSize) + ": " + msg);
    }

private:
    void need(size_t k) {
        if (k > remaining()) fail("unexpected end of file");
    }
    const uint8_t* p_;
    size_t n_;
    size_t pos_ = 0;
};

}  // namespace

std::vector<uint8_t> encode(const Module& m, bool dbg) {
    Writer w;
    w.u32(static_cast<uint32_t>(m.strings.size()));
    for (const auto& s : m.strings) w.str(s);
    w.u32(static_cast<uint32_t>(m.structs.size()));
    for (const auto& s : m.structs) {
        w.str(s.name);
        w.u16(static_cast<uint16_t>(s.field_kinds.size()));
        for (size_t i = 0; i < s.field_kinds.size(); ++i) {
            w.str(s.field_names[i]);
            w.kind(s.field_kinds[i]);
        }
    }
    w.u32(static_cast<uint32_t>(m.enums.size()));
    for (const auto& e : m.enums) {
        w.str(e.name);
        w.u16(static_cast<uint16_t>(e.variants.size()));
        for (const auto& v : e.variants) {
            w.str(v.name);
            w.u16(static_cast<uint16_t>(v.field_kinds.size()));
            for (Kind k : v.field_kinds) w.kind(k);
        }
    }
    w.u32(static_cast<uint32_t>(m.signatures.size()));
    for (const auto& s : m.signatures) {
        w.u16(static_cast<uint16_t>(s.params.size()));
        for (Kind k : s.params) w.kind(k);
        w.kind(s.ret);
    }
    w.u32(static_cast<uint32_t>(m.globals.size()));
    for (const auto& g : m.globals) {
        w.str(g.name);
        w.kind(g.kind);
    }
    w.u32(static_cast<uint32_t>(m.functions.size()));
    for (const auto& f : m.functions) {
        w.str(f.name);
        w.u16(f.num_params);
        w.kind(f.ret_kind);
        w.u16(static_cast<uint16_t>(f.local_kinds.size()));
        for (Kind k : f.local_kinds) w.kind(k);
        w.u16(static_cast<uint16_t>(f.upvalues.size()));
        for (const auto& u : f.upvalues) {
            w.kind(u.kind);
            w.u8(u.from_local ? 1 : 0);
            w.u16(u.index);
        }
        w.u32(f.max_stack);
        w.u32(static_cast<uint32_t>(f.code.size()));
        w.raw(f.code.data(), f.code.size());
        if (dbg) {
            w.u32(f.decl_line);
            w.u32(static_cast<uint32_t>(f.lines.size()));
            for (const auto& e : f.lines) {
                w.u32(e.pc);
                w.u32(e.line);
                w.u32(e.column);
            }
            w.u16(static_cast<uint16_t>(f.local_names.size()));
            for (const auto& n : f.local_names) w.str(n);
            for (const auto& u : f.upvalues) w.str(u.name);
        }
    }
    w.u32(m.init_function);
    w.u32(m.main_function);
    w.str(dbg ? m.source_name : std::string());

    Writer h;
    h.raw(kMagic, 4);
    h.u16(kVersionMajor);
    h.u16(kVersionMinor);
    h.u32(dbg ? kFlagDebugInfo : 0);
    h.u32(static_cast<uint32_t>(w.out.size()));
    h.u32(crc32(w.out.data(), w.out.size()));
    h.out.insert(h.out.end(), w.out.begin(), w.out.end());
    return h.out;
}

Module decode(const std::vector<uint8_t>& bytes, size_t max_size) {
    if (bytes.size() > max_size)
        throw DecodeError("bytecode file too large (" + std::to_string(bytes.size()) + " bytes, limit " +
                          std::to_string(max_size) + ")");
    if (bytes.size() < kHeaderSize) throw DecodeError("not an Ember bytecode file (too short)");
    if (std::memcmp(bytes.data(), kMagic, 4) != 0) throw DecodeError("not an Ember bytecode file (bad magic)");
    uint16_t major, minor;
    uint32_t flags, size, crc;
    std::memcpy(&major, bytes.data() + 4, 2);
    std::memcpy(&minor, bytes.data() + 6, 2);
    std::memcpy(&flags, bytes.data() + 8, 4);
    std::memcpy(&size, bytes.data() + 12, 4);
    std::memcpy(&crc, bytes.data() + 16, 4);
    if (major != kVersionMajor)
        throw DecodeError("unsupported bytecode version " + std::to_string(major) + "." + std::to_string(minor) +
                          " (this VM reads " + std::to_string(kVersionMajor) + ".x)");
    if (minor > kVersionMinor)
        throw DecodeError("bytecode version " + std::to_string(major) + "." + std::to_string(minor) +
                          " is newer than this VM (" + std::to_string(kVersionMajor) + "." +
                          std::to_string(kVersionMinor) + ")");
    if (flags & ~kFlagDebugInfo) throw DecodeError("unknown flags in bytecode header");
    if (size != bytes.size() - kHeaderSize) throw DecodeError("payload size does not match the file size");
    const uint8_t* payload = bytes.data() + kHeaderSize;
    if (crc32(payload, size) != crc) throw DecodeError("checksum mismatch (file is corrupted)");
    bool dbg = flags & kFlagDebugInfo;

    Reader r(payload, size);
    Module m;
    m.has_debug_info = dbg;
    uint32_t n = r.count("strings", 4);
    m.strings.reserve(n);
    for (uint32_t i = 0; i < n; ++i) m.strings.push_back(r.str("string"));
    n = r.count("structs", 6);
    for (uint32_t i = 0; i < n; ++i) {
        StructDef s;
        s.name = r.str("struct name");
        uint16_t nf = r.u16();
        for (uint16_t k = 0; k < nf; ++k) {
            s.field_names.push_back(r.str("field name"));
            s.field_kinds.push_back(r.kind());
        }
        m.structs.push_back(std::move(s));
    }
    n = r.count("enums", 6);
    for (uint32_t i = 0; i < n; ++i) {
        EnumDef e;
        e.name = r.str("enum name");
        uint16_t nv = r.u16();
        if (nv == 0) r.fail("enum without variants");
        for (uint16_t k = 0; k < nv; ++k) {
            VariantDef v;
            v.name = r.str("variant name");
            uint16_t nf = r.u16();
            for (uint16_t j = 0; j < nf; ++j) v.field_kinds.push_back(r.kind());
            e.variants.push_back(std::move(v));
        }
        m.enums.push_back(std::move(e));
    }
    n = r.count("signatures", 3);
    for (uint32_t i = 0; i < n; ++i) {
        Signature s;
        uint16_t np = r.u16();
        for (uint16_t k = 0; k < np; ++k) s.params.push_back(r.kind());
        s.ret = r.kind();
        m.signatures.push_back(std::move(s));
    }
    n = r.count("globals", 5);
    for (uint32_t i = 0; i < n; ++i) {
        GlobalDef g;
        g.name = r.str("global name");
        g.kind = r.kind();
        m.globals.push_back(std::move(g));
    }
    if (m.globals.size() > 65536) r.fail("too many globals");
    n = r.count("functions", 20);
    if (n == 0) r.fail("no functions");
    for (uint32_t i = 0; i < n; ++i) {
        Function f;
        f.name = r.str("function name");
        f.num_params = r.u16();
        f.ret_kind = r.kind();
        uint16_t nl = r.u16();
        if (nl < f.num_params) r.fail("function '" + f.name + "' has fewer locals than parameters");
        for (uint16_t k = 0; k < nl; ++k) f.local_kinds.push_back(r.kind());
        uint16_t nu = r.u16();
        for (uint16_t k = 0; k < nu; ++k) {
            UpvalueDef u;
            u.kind = r.kind();
            uint8_t fl = r.u8();
            if (fl > 1) r.fail("invalid upvalue flag");
            u.from_local = fl == 1;
            u.index = r.u16();
            f.upvalues.push_back(u);
        }
        f.max_stack = r.u32();
        uint32_t code_len = r.u32();
        if (code_len > r.remaining()) r.fail("code length exceeds the file");
        f.code.assign(r.here(), r.here() + code_len);
        r.skip(code_len);
        if (dbg) {
            f.decl_line = r.u32();
            uint32_t nlines = r.count("line entries", 12);
            uint32_t last_pc = 0;
            for (uint32_t k = 0; k < nlines; ++k) {
                LineEntry e{r.u32(), r.u32(), r.u32()};
                if (e.pc >= code_len || (k > 0 && e.pc < last_pc)) r.fail("invalid line table");
                last_pc = e.pc;
                f.lines.push_back(e);
            }
            uint16_t nn = r.u16();
            if (nn != nl) r.fail("local name count does not match locals");
            for (uint16_t k = 0; k < nn; ++k) f.local_names.push_back(r.str("local name"));
            for (auto& u : f.upvalues) u.name = r.str("upvalue name");
        } else {
            f.local_names.assign(nl, "");
        }
        m.functions.push_back(std::move(f));
    }
    m.init_function = r.u32();
    m.main_function = r.u32();
    m.source_name = r.str("source name");
    if (r.remaining() != 0) r.fail("trailing bytes after the module");
    return m;
}

}  // namespace ember::bc

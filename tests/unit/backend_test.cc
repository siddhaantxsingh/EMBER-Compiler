// IR, optimizer, bytecode format, verifier, VM and GC tests.
#include <random>

#include "bc/disasm.h"
#include "bc/format.h"
#include "bc/verify.h"
#include "helpers.h"
#include "ir/opt.h"
#include "ir/validate.h"
#include "test.h"

using namespace ember;
using bc::Opcode;

namespace {

std::string first_error(const bc::Module& m) {
    auto r = bc::verify(m);
    return r.ok() ? std::string() : r.errors[0];
}

const char* kSample = R"(
struct P { x: Int, y: Float }
enum E { A(Int), B }
var g = 3;
fn f(a: Int) -> Int { return a * g; }
fn main() {
    let p = P { x: f(2), y: 1.5 };
    var xs = [1, 2, 3];
    push(xs, p.x);
    let c = fn(k: Int) -> Int { return k + xs[0]; };
    print(c(10));
    print(match E::A(4) { E::A(v) => v, E::B => 0 });
    print(p);
}
)";

}  // namespace

// ------------------------------------------------------------ IR

TEST(ir_validator_accepts_lowered_programs) {
    auto c = compile("t.em", kSample, CompileOptions{}, Stage::IR);
    CHECK(c->ok);
    CHECK(ir::validate(c->ir).empty());
}

TEST(ir_validator_rejects_broken_ir) {
    auto c = compile("t.em", kSample, CompileOptions{}, Stage::IR);
    ir::Module m = c->ir;
    // Drop the terminator of main's entry block.
    auto& f = m.functions[m.main_function];
    f.blocks[0].instrs.pop_back();
    auto errs = ir::validate(m);
    CHECK(!errs.empty());
    ir::Module m2 = c->ir;
    // Use a temp before it is defined.
    auto& f2 = m2.functions[m2.main_function];
    ir::Instr bad;
    bad.op = ir::Op::IAdd;
    bad.dst = f2.new_temp(Kind::Int);
    bad.args = {bad.dst, bad.dst};
    f2.blocks[0].instrs.insert(f2.blocks[0].instrs.begin(), bad);
    CHECK(!ir::validate(m2).empty());
}

TEST(ir_optimizer_folds_but_never_folds_traps) {
    auto src = "fn main() { print(2 + 3 * 4); print(9223372036854775807 + 1); }";
    auto c = compile("t.em", src, CompileOptions{}, Stage::IR);
    CHECK(c->ok);
    std::string text = ir::print_module(c->ir);
    CHECK_CONTAINS(text, "ConstInt 14");
    CHECK_CONTAINS(text, "IAdd");  // the overflowing addition stays
    auto r = th::run_source(src);
    CHECK_EQ(r.out, std::string("14\n"));
    CHECK_EQ(r.result.message, std::string("integer overflow"));
}

TEST(ir_optimizer_removes_dead_branches) {
    auto c = compile("t.em", "fn main() { if 1 > 2 { print(1); } else { print(2); } }", CompileOptions{}, Stage::IR);
    std::string text = ir::print_module(c->ir);
    CHECK(text.find("Branch") == std::string::npos);
    CHECK(c->opt_stats.branches_folded >= 1);
}

TEST(opt_levels_agree_on_sample) {
    auto a = th::run_source(kSample, {}, 0);
    auto b = th::run_source(kSample, {}, 1);
    CHECK(a.compiled && b.compiled);
    CHECK_EQ(a.out, b.out);
    CHECK_EQ(a.out, std::string("11\n4\nP { x: 6, y: 1.5 }\n"));
}

// ------------------------------------------------------------ format

TEST(bytecode_roundtrip) {
    auto c = compile("t.em", kSample, CompileOptions{});
    auto bytes = bc::encode(c->bytecode);
    bc::Module d = bc::decode(bytes);
    CHECK_EQ(bc::disassemble(d), bc::disassemble(c->bytecode));
    CHECK(bc::encode(d) == bytes);
    auto nodbg = bc::encode(c->bytecode, false);
    CHECK(nodbg.size() < bytes.size());
    bc::Module d2 = bc::decode(nodbg);
    CHECK(!d2.has_debug_info);
    CHECK(bc::verify(d2).ok());
}

TEST(decoder_rejects_every_truncation) {
    auto c = compile("t.em", kSample, CompileOptions{});
    auto bytes = bc::encode(c->bytecode);
    for (size_t n = 0; n < bytes.size(); ++n) {
        std::vector<uint8_t> t(bytes.begin(), bytes.begin() + static_cast<long>(n));
        bool threw = false;
        try {
            bc::decode(t);
        } catch (const bc::DecodeError&) {
            threw = true;
        }
        CHECK(threw);
    }
}

TEST(decoder_detects_bit_flips) {
    auto c = compile("t.em", kSample, CompileOptions{});
    auto bytes = bc::encode(c->bytecode);
    std::mt19937 rng(1);
    for (int i = 0; i < 500; ++i) {
        auto b = bytes;
        size_t pos = rng() % b.size();
        b[pos] ^= static_cast<uint8_t>(1u << (rng() % 8));
        bool threw = false;
        try {
            bc::decode(b);
        } catch (const bc::DecodeError&) {
            threw = true;
        }
        CHECK(threw);  // header fields are checked, the payload is covered by CRC32
    }
}

TEST(decoder_survives_garbage_with_valid_checksum) {
    // Random payloads with a correct header and checksum exercise the
    // structural checks rather than the CRC.
    std::mt19937 rng(2);
    for (int i = 0; i < 2000; ++i) {
        std::vector<uint8_t> payload(rng() % 300);
        for (auto& x : payload) x = static_cast<uint8_t>(rng() % 4 == 0 ? rng() : rng() % 4);
        std::vector<uint8_t> file = {'E', 'M', 'B', 'C', 1, 0, 0, 0, 1, 0, 0, 0};
        uint32_t size = static_cast<uint32_t>(payload.size()), crc = bc::crc32(payload.data(), payload.size());
        file.insert(file.end(), reinterpret_cast<uint8_t*>(&size), reinterpret_cast<uint8_t*>(&size) + 4);
        file.insert(file.end(), reinterpret_cast<uint8_t*>(&crc), reinterpret_cast<uint8_t*>(&crc) + 4);
        file.insert(file.end(), payload.begin(), payload.end());
        try {
            bc::Module m = bc::decode(file);
            bc::verify(m);  // must not crash either
        } catch (const bc::DecodeError&) {
        }
    }
}

TEST(decoder_rejects_oversized_counts_without_allocating) {
    std::vector<uint8_t> payload = {0xFF, 0xFF, 0xFF, 0x7F};  // 2^31 strings
    std::vector<uint8_t> file = {'E', 'M', 'B', 'C', 1, 0, 0, 0, 0, 0, 0, 0};
    uint32_t size = 4, crc = bc::crc32(payload.data(), 4);
    file.insert(file.end(), reinterpret_cast<uint8_t*>(&size), reinterpret_cast<uint8_t*>(&size) + 4);
    file.insert(file.end(), reinterpret_cast<uint8_t*>(&crc), reinterpret_cast<uint8_t*>(&crc) + 4);
    file.insert(file.end(), payload.begin(), payload.end());
    bool threw = false;
    try {
        bc::decode(file);
    } catch (const bc::DecodeError& e) {
        threw = std::string(e.what()).find("too many") != std::string::npos;
    }
    CHECK(threw);
}

// ------------------------------------------------------------ verifier

TEST(verifier_accepts_minimal) {
    th::Asm a;
    a.op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_EQ(first_error(th::single(a.code)), std::string());
}

TEST(verifier_rejects_underflow) {
    th::Asm a;
    a.op(Opcode::IADD).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(a.code)), "underflow");
}

TEST(verifier_rejects_kind_errors) {
    th::Asm a;
    a.op(Opcode::PUSH_INT).i64(1).op(Opcode::PUSH_TRUE).op(Opcode::IADD).op(Opcode::POP).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(a.code)), "expected int");
}

TEST(verifier_rejects_jump_into_instruction) {
    th::Asm a;
    a.op(Opcode::JUMP).u32(2).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(a.code)), "not an instruction");
}

TEST(verifier_rejects_falling_off_end) {
    th::Asm a;
    a.op(Opcode::NOP);
    CHECK_CONTAINS(first_error(th::single(a.code)), "fall off");
}

TEST(verifier_rejects_inconsistent_merge) {
    // Path 1 pushes an Int, path 2 pushes nothing, both reach pc of RETURN.
    th::Asm a;
    a.op(Opcode::PUSH_TRUE);                      // 0
    a.op(Opcode::JUMP_IF_TRUE).u32(15);           // 1..5: jump with depth 0
    a.op(Opcode::PUSH_INT).i64(5);                // 6..14: fall through with depth 1
    a.op(Opcode::PUSH_UNIT);                      // 15: merge point
    a.op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(a.code)), "inconsistent operand stack");
}

TEST(verifier_rejects_stack_over_declared_max) {
    th::Asm a;
    for (int i = 0; i < 5; ++i) a.op(Opcode::PUSH_UNIT);
    for (int i = 0; i < 5; ++i) a.op(Opcode::POP);
    a.op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(a.code, 3)), "exceeds max_stack");
}

TEST(verifier_rejects_bad_indices) {
    th::Asm a;
    a.op(Opcode::LOAD_LOCAL).u16(3).op(Opcode::POP).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(a.code)), "local slot out of range");
    th::Asm b;
    b.op(Opcode::PUSH_STR).u32(0).op(Opcode::POP).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(b.code)), "string index");
    th::Asm c;
    c.op(Opcode::CALL_NATIVE).u16(999).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(c.code)), "native");
    th::Asm d;
    d.op(static_cast<Opcode>(250));
    CHECK_CONTAINS(first_error(th::single(d.code)), "invalid opcode");
}

TEST(verifier_rejects_return_with_leftovers) {
    th::Asm a;
    a.op(Opcode::PUSH_INT).i64(1).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(a.code)), "not empty at return");
}

TEST(verifier_rejects_bad_entry_point) {
    th::Asm a;
    a.op(Opcode::PUSH_INT).i64(1).op(Opcode::RETURN);
    auto m = th::single(a.code);
    m.functions[0].ret_kind = Kind::Int;
    CHECK_CONTAINS(first_error(m), "entry point");
}

TEST(verifier_accepts_all_compiled_programs) {
    auto c = compile("t.em", kSample, CompileOptions{});
    CHECK(bc::verify(c->bytecode).ok());
}

TEST(vm_refuses_unverified_module) {
    th::Asm a;
    a.op(Opcode::IADD);
    bool threw = false;
    try {
        vm::VM m(th::single(a.code));
    } catch (const vm::InvalidModule&) {
        threw = true;
    }
    CHECK(threw);
}

// ------------------------------------------------------------ VM runtime checks

namespace {
vm::RunResult run_module(const bc::Module& m, std::string* out = nullptr) {
    std::ostringstream os;
    vm::VM machine(m);
    machine.set_output(&os);
    auto r = machine.run();
    if (out) *out = os.str();
    return r;
}
}  // namespace

TEST(vm_type_confusion_is_an_error_not_a_crash) {
    // A string where an array is expected: both are Ref, so the verifier
    // accepts it; the VM must report a type mismatch.
    auto m = th::single({});
    m.strings.push_back("abc");
    th::Asm a;
    a.op(Opcode::PUSH_STR).u32(0).op(Opcode::ARRAY_LEN).op(Opcode::POP).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    m.functions[0].code = a.code;
    auto r = run_module(m);
    CHECK(!r.ok);
    CHECK_EQ(r.kind, vm::ErrorKind::TypeMismatch);
}

TEST(vm_wrong_struct_is_detected) {
    auto m = th::single({});
    m.structs.push_back(bc::StructDef{"A", {"x"}, {Kind::Int}});
    m.structs.push_back(bc::StructDef{"B", {"y"}, {Kind::Int}});
    th::Asm a;
    a.op(Opcode::PUSH_INT).i64(1).op(Opcode::MAKE_STRUCT).u16(0);   // an A
    a.op(Opcode::GET_FIELD).u16(1).u16(0);                           // read as B
    a.op(Opcode::POP).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    m.functions[0].code = a.code;
    auto r = run_module(m);
    CHECK_EQ(r.kind, vm::ErrorKind::TypeMismatch);
}

TEST(vm_uninitialized_ref_local_is_detected) {
    auto m = th::single({}, 8, {Kind::Ref});
    th::Asm a;
    a.op(Opcode::LOAD_LOCAL).u16(0).op(Opcode::SLEN).op(Opcode::POP).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    m.functions[0].code = a.code;
    auto r = run_module(m);
    CHECK_EQ(r.kind, vm::ErrorKind::TypeMismatch);
}

TEST(vm_indirect_call_signature_checked) {
    auto m = th::single({});
    bc::Function callee;
    callee.name = "takes_int";
    callee.num_params = 1;
    callee.local_kinds = {Kind::Int};
    callee.local_names = {"x"};
    callee.ret_kind = Kind::Unit;
    callee.max_stack = 1;
    th::Asm c;
    c.op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    callee.code = c.code;
    m.functions.push_back(callee);
    m.signatures.push_back(bc::Signature{{Kind::Float}, Kind::Unit});  // wrong
    th::Asm a;
    double one = 1.0;
    a.op(Opcode::CLOSURE).u32(1).op(Opcode::PUSH_FLOAT).raw(&one, 8).op(Opcode::CALL_INDIRECT).u16(0);
    a.op(Opcode::POP).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    m.functions[0].code = a.code;
    auto r = run_module(m);
    CHECK_EQ(r.kind, vm::ErrorKind::TypeMismatch);
}

TEST(vm_stack_limit) {
    vm::VMOptions o;
    o.max_stack_values = 64;
    auto r = th::run_source("fn d(n: Int) -> Int { return d(n + 1); }\nfn main() { print(d(0)); }", o);
    CHECK_EQ(r.result.kind, vm::ErrorKind::StackOverflow);
}

TEST(vm_instruction_budget_is_exact) {
    // PUSH_UNIT, RETURN in init + same in main = 4 instructions.
    th::Asm a;
    a.op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    vm::VMOptions o;
    o.instruction_budget = 4;
    {
        vm::VM m(th::single(a.code), o);
        CHECK(m.run().ok);
    }
    o.instruction_budget = 3;
    vm::VM m(th::single(a.code), o);
    CHECK_EQ(m.run().kind, vm::ErrorKind::InstructionLimit);
}

TEST(vm_run_twice_is_rejected) {
    th::Asm a;
    a.op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    vm::VM m(th::single(a.code));
    m.run();
    bool threw = false;
    try {
        m.run();
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

// ------------------------------------------------------------ GC

TEST(gc_frees_unreachable_cycles) {
    auto c = compile("t.em",
                     "struct N { next: [N], data: [Int] }\nfn main() { for i in 0..5000 { let a = N { next: [], data: [i; 50] };"
                     " push(a.next, a); } }",
                     CompileOptions{});
    vm::VMOptions o;
    o.max_heap_bytes = 2u << 20;  // far below 5000 * 50 * 16 bytes
    vm::VM m(c->bytecode, o);
    std::ostringstream out;
    m.set_output(&out);
    auto r = m.run();
    CHECK(r.ok);
    CHECK(m.heap_stats().collections > 0);
    CHECK(m.heap_stats().objects_freed_total > 10000);
}

TEST(gc_stress_matches_normal_run) {
    vm::VMOptions stress;
    stress.gc_stress = true;
    auto a = th::run_source(kSample);
    auto b = th::run_source(kSample, stress);
    CHECK_EQ(a.out, b.out);
    CHECK(b.result.ok);
}

TEST(gc_keeps_values_live_across_allocation_sites) {
    // Every allocating instruction under stress mode with values on the stack.
    vm::VMOptions stress;
    stress.gc_stress = true;
    auto r = th::run_source(R"(
import strings;
struct S { a: String, b: [String] }
enum E { V(String, String) }
fn main() {
    let s = S { a: "x" + "y", b: ["p" + "q", str(12), strings.upper("r")] };
    let e = E::V(s.a + "!", strings.join(s.b, "-"));
    let parts = strings.split("a,b,c", ",");
    let f = fn(t: String) -> String { return t + s.a; };
    print(match e { E::V(l, r) => f(l) + r });
    print(parts);
    print([s.a; 3]);
}
)", stress);
    CHECK(r.result.ok);
    CHECK_EQ(r.out, std::string("xy!xypq-12-R\n[a, b, c]\n[xy, xy, xy]\n"));
}

TEST(heap_limit_reports_out_of_memory) {
    vm::VMOptions o;
    o.max_heap_bytes = 1u << 20;
    auto r = th::run_source("fn main() { var xs: [[Int]] = []; while true { push(xs, [0; 1000]); } }", o);
    CHECK_EQ(r.result.kind, vm::ErrorKind::OutOfMemory);
}

// ------------------------------------------------------------ measured optimizations

TEST(superinstructions_emitted_at_O1_only_and_equivalent) {
    const char* src = "fn main() { var t = 0; var i = 0; while i < 100 { if i % 3 == 0 { t += i; } i += 1; } print(t); }";
    CompileOptions o1;
    auto c1 = compile("t.em", src, o1);
    CompileOptions o0;
    o0.opt_level = 0;
    auto c0 = compile("t.em", src, o0);
    std::string d1 = bc::disassemble(c1->bytecode), d0 = bc::disassemble(c0->bytecode);
    CHECK(d1.find("IJ") != std::string::npos);
    CHECK(d0.find("IJ") == std::string::npos);
    CHECK(d0.find("LOAD_LOCAL2") == std::string::npos && d0.find("STORE_LOAD") == std::string::npos);
    auto a = th::run_source(src, {}, 0);
    auto b = th::run_source(src, {}, 1);
    CHECK_EQ(a.out, b.out);
    CHECK_EQ(b.out, std::string("1683\n"));
}

TEST(verifier_checks_superinstructions) {
    th::Asm a;
    a.op(Opcode::LOAD_LOCAL2).u16(0).u16(9).op(Opcode::POP).op(Opcode::POP).op(Opcode::PUSH_UNIT).op(Opcode::RETURN);
    CHECK_CONTAINS(first_error(th::single(a.code, 8, {Kind::Int})), "local slot out of range");
    th::Asm b;  // IJLT on a Bool
    b.op(Opcode::PUSH_TRUE).op(Opcode::PUSH_INT).i64(1).op(Opcode::IJLT).u32(0);
    CHECK_CONTAINS(first_error(th::single(b.code)), "expected int");
}

TEST(branch_on_not_is_simplified) {
    auto c = compile("t.em", "fn main() { let b = len(\"x\") == 1; if !b { print(1); } else { print(2); } }", CompileOptions{},
                     Stage::IR);
    CHECK(ir::print_module(c->ir).find("Not") == std::string::npos);
    CHECK_EQ(th::run_source("fn main() { let b = len(\"x\") == 1; if !b { print(1); } else { print(2); } }").out,
             std::string("2\n"));
}

TEST(instruction_counting_only_when_requested) {
    const char* src = "fn main() { var i = 0; while i < 10 { i += 1; } }";
    auto c = compile("t.em", src, CompileOptions{});
    {
        vm::VM m(c->bytecode);
        CHECK(m.run().ok);
        CHECK_EQ(m.instructions_executed(), uint64_t(0));
    }
    vm::VMOptions o;
    o.count_instructions = true;
    vm::VM m(c->bytecode, o);
    CHECK(m.run().ok);
    CHECK(m.instructions_executed() > 30);
}

// libFuzzer target: start from a valid compiled module and let the fuzzer
// overwrite code bytes of its functions. This reaches the verifier and VM
// with nearly-valid code much faster than raw bytes do.
#include <cstddef>
#include <cstdint>
#include <sstream>

#include "bc/verify.h"
#include "driver/driver.h"
#include "vm/vm.h"

namespace {
const char* kSeedProgram = R"(
struct P { x: Int, s: String }
enum E { A(Int), B(String, Float), C }
var g = [1, 2, 3];
fn f(n: Int) -> Int { if n < 2 { return n; } return f(n - 1) + f(n - 2); }
fn main() {
    let p = P { x: f(5), s: "hi" };
    let c = fn(k: Int) -> Int { return k + p.x + len(g); };
    push(g, c(1));
    let e = E::B(p.s + "!", 2.5);
    let m = match e { E::A(v) => str(v), E::B(s, fl) => s + str(fl), E::C => "c" };
    print(m);
    for i in 0..3 { print(g[i] * 2); }
}
)";
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static auto compiled = ember::compile("seed.em", kSeedProgram, ember::CompileOptions{});
    if (size < 4) return 0;
    ember::bc::Module m = compiled->bytecode;
    // data: [function index][offset hi][offset lo][bytes...]
    auto& fn = m.functions[data[0] % m.functions.size()];
    size_t off = ((static_cast<size_t>(data[1]) << 8) | data[2]) % fn.code.size();
    for (size_t i = 3; i < size && off < fn.code.size(); ++i, ++off) fn.code[off] = data[i];
    if (!ember::bc::verify(m).ok()) return 0;
    ember::vm::VMOptions o;
    o.instruction_budget = 100000;
    o.max_heap_bytes = 8u << 20;
    o.max_call_depth = 200;
    std::ostringstream out;
    std::istringstream in;
    ember::vm::VM vm(m, o);
    vm.set_output(&out);
    vm.set_input(&in);
    vm.run();
    return 0;
}

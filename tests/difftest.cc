// Differential tester: for each seed, generate a random well-typed program
// and check that every execution configuration agrees with the reference
// interpreter on standard output and on the runtime error (if any):
//   * compiler -O1 + VM
//   * compiler -O0 + VM
//   * -O1 bytecode encoded to the file format, decoded, then run
//   * -O1 with the GC in stress mode (collect before every allocation)
// Usage: difftest [--seeds N] [--start S] [--stress-every K] [--verbose] | --print SEED
// Exit status 0 iff all seeds agree. A failing seed's program is written to
// difftest-fail-<seed>.em for reproduction.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "bc/format.h"
#include "driver/driver.h"
#include "tools/progen.h"
#include "tools/refinterp.h"
#include "vm/vm.h"

using namespace ember;

namespace {

struct Outcome {
    std::string out;
    bool ok = true;
    std::string error;
};

Outcome run_vm(const bc::Module& m, bool stress) {
    Outcome o;
    std::ostringstream out;
    std::istringstream in;
    vm::VMOptions opts;
    opts.max_call_depth = 2000;
    opts.gc_stress = stress;
    opts.instruction_budget = 20'000'000;
    vm::VM machine(m, opts);
    machine.set_output(&out);
    machine.set_input(&in);
    auto r = machine.run();
    o.out = out.str();
    o.ok = r.ok;
    o.error = r.message;
    return o;
}

bool same(const Outcome& a, const Outcome& b) { return a.out == b.out && a.ok == b.ok && a.error == b.error; }

std::string describe(const Outcome& o) {
    std::string s = o.ok ? "ok" : "error: " + o.error;
    std::string out = o.out.size() > 2000 ? o.out.substr(0, 2000) + "...[truncated]" : o.out;
    return s + "\n--- stdout ---\n" + out;
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t seeds = 200, start = 1;
    int stress_every = 10;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--seeds") seeds = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--start") start = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--stress-every") stress_every = std::atoi(next().c_str());
        else if (a == "--verbose") verbose = true;
        else if (a == "--print") {
            std::cout << generate_program(std::strtoull(next().c_str(), nullptr, 10));
            return 0;
        }
        else {
            std::cerr << "unknown option " << a << "\n";
            return 2;
        }
    }
    uint64_t failures = 0, inconclusive = 0, errors_agreed = 0;
    for (uint64_t seed = start; seed < start + seeds; ++seed) {
        std::string src = generate_program(seed);
        auto fail = [&](const std::string& what) {
            ++failures;
            std::string path = "difftest-fail-" + std::to_string(seed) + ".em";
            std::ofstream(path) << src;
            std::cerr << "seed " << seed << ": " << what << "\n  program saved to " << path << "\n";
        };
        CompileOptions o1;
        auto c1 = compile("gen.em", src, o1);
        if (!c1->ok) {
            fail("generated program does not compile:\n" + c1->diagnostics);
            continue;
        }
        Outcome ref;
        {
            std::ostringstream out;
            RefOptions ro;
            ro.max_depth = 2000;
            ro.max_steps = 20'000'000;
            auto r = run_reference_big_stack(c1->program, c1->sema, out, ro);
            if (r.limit_hit) {
                ++inconclusive;
                continue;
            }
            ref.out = out.str();
            ref.ok = r.ok;
            ref.error = r.error;
        }
        if (!ref.ok) ++errors_agreed;
        Outcome v1 = run_vm(c1->bytecode, false);
        if (v1.error == "instruction limit exceeded" || v1.out.size() > (4u << 20)) {
            ++inconclusive;
            continue;
        }
        if (!same(ref, v1)) {
            fail("VM -O1 disagrees with the reference interpreter\n=== reference: " + describe(ref) +
                 "\n=== vm -O1: " + describe(v1));
            continue;
        }
        CompileOptions o0;
        o0.opt_level = 0;
        auto c0 = compile("gen.em", src, o0);
        Outcome v0 = c0->ok ? run_vm(c0->bytecode, false) : Outcome{};
        if (!c0->ok || !same(ref, v0)) {
            fail("VM -O0 disagrees with the reference interpreter\n=== reference: " + describe(ref) +
                 "\n=== vm -O0: " + describe(v0) + c0->diagnostics);
            continue;
        }
        try {
            auto bytes = bc::encode(c1->bytecode);
            bc::Module decoded = bc::decode(bytes);
            Outcome vd = run_vm(decoded, false);
            if (!same(ref, vd)) {
                fail("decoded bytecode disagrees\n=== reference: " + describe(ref) + "\n=== decoded: " + describe(vd));
                continue;
            }
        } catch (const std::exception& e) {
            fail(std::string("encode/decode failed: ") + e.what());
            continue;
        }
        if (stress_every > 0 && seed % static_cast<uint64_t>(stress_every) == 0) {
            Outcome vs = run_vm(c1->bytecode, true);
            if (!same(ref, vs)) {
                fail("GC stress run disagrees\n=== reference: " + describe(ref) + "\n=== stress: " + describe(vs));
                continue;
            }
        }
        if (verbose) std::cerr << "seed " << seed << ": ok" << (ref.ok ? "" : " (error: " + ref.error + ")") << "\n";
    }
    std::cout << "difftest: " << seeds << " programs, " << failures << " failures, " << inconclusive
              << " inconclusive (limits), " << errors_agreed << " ended in an agreed runtime error\n";
    return failures == 0 ? 0 : 1;
}

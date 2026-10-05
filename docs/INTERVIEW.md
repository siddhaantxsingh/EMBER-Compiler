# Interview notes — Ember

## 30-second pitch
I built a statically typed language end to end in C++20: lexer, Pratt parser
with error recovery, type checker with closure-capture analysis, a typed CFG
IR with a validator and optimizer, a stack-bytecode code generator, a
versioned bytecode file format with a verifier, and a separate VM with a
precise mark-and-sweep GC. I tested it with a reference interpreter and a
random program generator (20,000 programs, zero disagreements), five fuzzers
under ASan/UBSan, and made it 0–40% faster with profile-driven changes that I
measured with interleaved A/B runs.

## Questions I expect, and my answers

**Why a stack VM?** Simpler code generation and a much simpler verifier;
register VMs execute fewer instructions but need allocation and liveness. I
recovered part of the difference with superinstructions chosen from an
opcode-pair profile.

**How do you know the VM is memory safe on hostile bytecode?** Three layers:
a decoder that bounds-checks every read and every count before allocating; a
verifier that proves stack depth, operand kinds and index ranges by abstract
interpretation; runtime checks for what the verifier can't prove (object type
behind a reference, indirect call signature, uninitialized variables). The
raw-bytecode and mutated-bytecode fuzzers run all three under ASan.

**How do closures work?** Lua-style upvalues. The resolver computes, per
function, which variables are captured and from where (parent local or
parent upvalue). While the frame is alive the upvalue points into the stack;
leaving the scope closes it (copies the value into the upvalue). Loop
variables are closed each iteration so every iteration's closure sees its own
value.

**Hardest bug?** A superinstruction (fused compare-and-jump) made loops 30%
slower. I isolated it by toggling each fusion, then found the handler let the
compiler emit a conditional move feeding the indirect dispatch jump, which
killed branch prediction. Dispatching separately on the taken and
fall-through paths fixed it, and it became a 2–18% win.

**What did the differential tester find?** On its first run, match bindings
inside global initializers had no frame slots — a lowering bug no unit test
had hit. Later it found mismatched error messages between the VM and the
reference interpreter, and that the generator itself produced different
programs under GCC and Clang because C++ doesn't sequence `+` operands.

**GC correctness?** Every allocating instruction keeps its operands on the VM
stack until the new object exists. Stress mode collects before every
allocation, which turns any missed root into a use-after-free that ASan
reports; the differential tester runs every 10th program in stress mode.

**What would you do next?** Inline object storage plus a size-class allocator
(callgrind shows half of the allocation benchmark is malloc/free), debug info
with variable scopes, and user modules.

## Numbers to remember
~12.6k lines of C++ implementation; 61 unit tests, 90 golden programs;
20,000-program differential runs; fib(32) in 0.17 s; ~0.5 G bytecode
instructions/s on a 2-vCPU VM; 0–40% speedup from measured optimizations.

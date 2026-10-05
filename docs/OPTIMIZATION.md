# Optimization

Rule followed throughout: **profile first, change one thing, measure with
interleaved A/B runs, keep it only if it is correct and helps.** Every
optimization below is checked by the differential tester (thousands of
generated programs, -O0 vs -O1 vs reference interpreter) and the golden
tests.

Measurement setup for all numbers in this document: 2-vCPU cloud VM
(Intel Xeon @ 2.80 GHz), Ubuntu 24.04, GCC 13.3 `-O3` (CMake Release),
wall-clock time of `ember run program.em` including compilation (≤ 5 ms),
median of 5–7 interleaved runs (`bench/run.py --ember A --ember-b B`).
Run-to-run noise on this machine is about ±4%; changes smaller than that
are reported as neutral. Raw results: `bench/results/*.json`.

## 1. IR optimizations (`src/ir/opt.cc`, `-O1`)

Correctness-preserving cleanups, not tuned by measurement:

| Pass | What | Never does |
| --- | --- | --- |
| constant folding | evaluates Int/Float/Bool operations on constants with VM semantics | fold an operation that would trap (overflow, division by zero, bad `int()`) — the trap must still happen at run time |
| branch folding | `Branch` on a constant becomes `Jump` | |
| branch on `Not` | `Branch(Not c)` becomes `Branch(c)` with swapped targets | |
| jump threading | edges to blocks containing only `Jump X` go to `X` | |
| block merging | `A: ...; Jump B` with `B` having one predecessor is merged | |
| unreachable blocks | removed (also at `-O0`; code generation requires it) | |
| dead code | unused results of pure instructions removed | remove anything that can trap or has effects |

Unit test `ir_optimizer_folds_but_never_folds_traps` checks that
`9223372036854775807 + 1` is not folded.

## 2. Profiling

`ember run --profile prog.em` runs the program on the instrumented loop and
reports executed instructions per opcode, per adjacent opcode pair and per
function. Across the ten benchmarks (before the changes below) the
dominant opcodes were `LOAD_LOCAL` (25–43% of all instructions) and
`PUSH_INT`; the most frequent pairs were `LOAD_LOCAL→PUSH_INT`,
`STORE_LOCAL→LOAD_LOCAL`, `LOAD_LOCAL→LOAD_LOCAL` and integer compare →
conditional jump. `valgrind --tool=callgrind` was used for the
allocation-heavy benchmark (section 7).

## 3. Dispatch: computed goto (kept)

Replace the `switch` in the interpreter loop with direct threading (each
handler ends with `goto *labels[*ip++]`, a GCC/Clang extension; the
`switch` loop remains as the portable fallback, `-DEMBER_COMPUTED_GOTO=0`).

| benchmark | switch | computed goto | change |
| --- | ---: | ---: | ---: |
| binary_trees | 0.339 | 0.333 | -2.0% |
| closures | 0.347 | 0.331 | -4.7% |
| enum_dispatch | 1.122 | 0.829 | -26.1% |
| fib | 0.224 | 0.188 | -16.1% |
| loop_sum | 1.034 | 0.823 | -20.4% |
| mandelbrot | 0.517 | 0.402 | -22.3% |
| matrix | 0.453 | 0.440 | -2.9% |
| quicksort | 1.137 | 1.130 | -0.6% |
| sieve | 1.476 | 1.494 | +1.2% |
| strings | 0.255 | 0.228 | -10.9% |

Median seconds, 7 interleaved runs (`bench/results/ab-dispatch.json`).

## 4. Instruction counting only when needed (kept)

The instruction budget was charged on every instruction. Measuring a build
without the check showed it costs 1–15% (largest on `enum_dispatch`,
`matrix`, `quicksort`). The loop is now a template on `<kDebug, kCount>`;
the counting variant is used only with `--max-steps`, `--stats` or the
debugger, so the budget semantics (exactly N instructions) are unchanged.

## 5. Loop rotation and block layout (kept, neutral)

* Loop rotation (lowering): test the loop condition at the bottom, with one
  test before entry, so each iteration runs one `JUMP` less.
* Block layout (code generator): greedy traces, so the common successor
  falls through.

Executed instructions drop by ~5% on loop-heavy code (`loop_sum`:
406.7 M → 373.3 M), but wall time did not change measurably:

| benchmark | neither | rotation + layout | change |
| --- | ---: | ---: | ---: |
| binary_trees | 0.334 | 0.322 | -3.5% |
| closures | 0.289 | 0.298 | +3.0% |
| enum_dispatch | 0.851 | 0.779 | -8.4% |
| fib | 0.191 | 0.196 | +2.5% |
| loop_sum | 0.769 | 0.756 | -1.7% |
| mandelbrot | 0.342 | 0.355 | +3.9% |
| matrix | 0.414 | 0.394 | -4.8% |
| quicksort | 0.934 | 0.928 | -0.6% |
| sieve | 1.316 | 1.359 | +3.2% |
| strings | 0.205 | 0.213 | +3.5% |

(`bench/results/ab-rotation-layout.json`.) Only `enum_dispatch` is outside
the noise band (median; its minimum is unchanged). They are kept because
they reduce instruction count and make the superinstructions below
applicable more often, but **no speedup is claimed for them**. They can be
switched off with `--no-loop-rotation` / `--no-block-layout`.

## 6. Superinstructions (kept, after a failed first attempt)

From the pair profile: `LOAD_LOCAL2 a b`, `STORE_LOAD a b`, and fused
integer compare-and-jump `IJEQ/IJNE/IJLT/IJLE/IJGT/IJGE` (a jump-if-false
after `ILT` becomes `IJGE`; exact for integers, so floats are never fused).
They are emitted by a peephole in the code generator, never across a block
boundary (block starts are the only jump targets), and verified like any
other instruction (bytecode format 1.1).

**First measurement: slower** (loop_sum +29%, matrix +14%). Isolating the
two kinds showed the fused compare-and-jump was responsible. Cause: the
handler `if (cond) ip = target; dispatch` let the compiler emit a
conditional move, so the following indirect jump depended on the
comparison and could not be predicted. Fix: dispatch separately on each
path (`if (cond) { ip = target; NEXT(); } NEXT();`), applied to all
conditional jumps. Second measurement (`bench/results/ab-superinstructions.json`):

| benchmark | without | with | change |
| --- | ---: | ---: | ---: |
| binary_trees | 0.358 | 0.341 | -4.6% |
| closures | 0.303 | 0.298 | -1.6% |
| enum_dispatch | 0.776 | 0.633 | -18.5% |
| fib | 0.194 | 0.168 | -13.1% |
| loop_sum | 0.749 | 0.706 | -5.8% |
| mandelbrot | 0.343 | 0.311 | -9.3% |
| matrix | 0.384 | 0.362 | -5.8% |
| quicksort | 0.950 | 0.816 | -14.1% |
| sieve | 1.322 | 1.281 | -3.2% |
| strings | 0.205 | 0.207 | +0.9% |

`--no-superinstructions` disables them.

## 7. Overall result

Baseline = commit `644ad06` (switch dispatch, per-instruction budget,
no rotation/layout/superinstructions), same machine, interleaved, 7 runs
(`bench/results/ab-overall.json`):

| benchmark | baseline | optimized | change |
| --- | ---: | ---: | ---: |
| binary_trees | 0.332 | 0.331 | -0.2% |
| closures | 0.356 | 0.284 | -20.3% |
| enum_dispatch | 1.055 | 0.635 | -39.8% |
| fib | 0.217 | 0.166 | -23.6% |
| loop_sum | 1.003 | 0.709 | -29.3% |
| mandelbrot | 0.503 | 0.310 | -38.3% |
| matrix | 0.441 | 0.371 | -15.7% |
| quicksort | 1.119 | 0.809 | -27.7% |
| sieve | 1.341 | 1.250 | -6.8% |
| strings | 0.217 | 0.200 | -8.0% |

## 8. Not done (yet), with evidence

* **Allocation.** `binary_trees` did not improve: callgrind shows ~50% of
  its instructions in `malloc`/`free` (each struct and array is a C++ object
  plus a separate `std::vector` buffer). Next steps: inline field storage
  for structs/enums/closures (one allocation instead of two) and a
  size-class free-list allocator in the heap.
* **Register VM / JIT**: out of scope (ADR-002).
* **Constant operands** (`IADD_IMM`, `ILT_IMM`): `LOAD_LOCAL→PUSH_INT` is
  the most frequent remaining pair; a candidate for the next round, to be
  measured the same way.
* **Store/load forwarding in the IR**: would create multi-use temporaries,
  which the stack code generator turns into slot stores and loads — likely
  neutral or negative for this VM; not attempted.

# Testing

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build
ctest --test-dir build --output-on-failure        # unit, golden, cli, difftest (300 seeds)
python3 tests/run_golden.py build/ember           # golden programs only
build/tests/difftest --seeds 20000 --start 1000   # long differential run
```

## Test suites

| Suite | Location | What it checks |
| --- | --- | --- |
| Unit tests (61) | `tests/unit/*.cc`, own minimal framework | lexer (numbers, escapes, error recovery, comments), parser (precedence, recovery, depth limit, random input), semantic errors and categories, constant folding vs VM, exhaustiveness, return paths, IR validator (accepts lowered code, rejects broken IR), optimizer (folds without folding traps, removes dead branches, branch-on-not), -O0 vs -O1, bytecode round trip, decoder (every truncation, 500 bit flips, garbage with valid checksum, forged counts), verifier (underflow, kinds, jump into instruction, fall-through, inconsistent merge, max_stack, indices, return with leftovers, entry points, superinstructions), VM runtime checks with hand-written ill-typed bytecode (type mismatch instead of memory errors), stack and instruction limits, GC (cycles freed under a small heap, stress mode equivalence, every allocation site under stress, out of memory), formatter (comments, normalization, parentheses, idempotence and behaviour preservation on 60 generated programs), reference interpreter, driver limits |
| Golden programs (90) | `tests/programs/{run,compile_errors,runtime_errors}` | full CLI output: stdout for 29 programs covering every language feature; exact diagnostics (with source excerpt and caret) for 35 compile errors; exact runtime error messages, partial output and stack traces for 26 runtime errors (including stack overflow, out of memory, instruction limit, uninitialized global/variable) |
| CLI tests (31 checks) | `tests/cli_tests.py` | exit codes (usage, I/O, compile, runtime, bad bytecode), build → run of `.embc`, `--no-debug-info`, corrupted / truncated / newer-version files, inspection commands, formatter idempotence and behaviour preservation on every golden program, `fmt --check/--write`, scripted debugger sessions, REPL session, stdin, `--stats` |
| Differential testing | `tests/difftest.cc`, `src/tools/progen.cc`, `src/tools/refinterp.cc` | random well-typed programs; stdout and runtime error must agree between the reference interpreter, VM at -O1, VM at -O0, encoded+decoded bytecode, and (every 10th program) GC stress mode |
| Fuzzing | `fuzz/` (libFuzzer + ASan + UBSan) | lexer; whole compiler (no crash, no internal error); compile + run with tight limits (compiled code may never hit `type mismatch` or internal errors); raw bytecode files through decoder, verifier, disassembler and VM; mutated code bytes of a valid module |
| Sanitizers | CI job `clang-asan-ubsan` | all of the above test suites under AddressSanitizer (with leak detection) and UndefinedBehaviorSanitizer |
| Layering | `scripts/check_layering.sh` | VM does not include compiler headers and vice versa |
| Benchmarks smoke | `bench/run.py --check-only` | benchmark programs produce their expected output |

## Test matrix

Rows: components. Columns: kinds of evidence (U unit, G golden, C CLI,
D differential, F fuzz, S sanitizers).

| Component | U | G | C | D | F | S |
| --- | :-: | :-: | :-: | :-: | :-: | :-: |
| Lexer | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Parser + recovery + limits | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Resolver / captures | ✓ | ✓ | | ✓ | ✓ | ✓ |
| Type checker | ✓ | ✓ | | ✓ | ✓ | ✓ |
| IR lowering + validator | ✓ | ✓ | | ✓ | ✓ | ✓ |
| Optimizer | ✓ | ✓ | | ✓ (-O0 vs -O1) | ✓ | ✓ |
| Code generator | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Bytecode format | ✓ | | ✓ | ✓ (round trip) | ✓ | ✓ |
| Verifier | ✓ | | ✓ | ✓ | ✓ | ✓ |
| VM + natives | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| GC | ✓ | ✓ | | ✓ (stress) | ✓ (stress) | ✓ |
| Formatter | ✓ | | ✓ | | | ✓ |
| Debugger | | | ✓ | | | ✓ |
| REPL | | | ✓ | | | ✓ |
| CLI | | ✓ | ✓ | | | ✓ |

## Recorded runs (2026-10-05, 2-vCPU VM)

| Run | Result |
| --- | --- |
| `ctest` Debug (GCC 13) | 4/4 suites pass |
| `ctest` RelWithDebInfo with ASan + UBSan (Clang 18), `detect_leaks=1` | 4/4 suites pass |
| difftest, 20,000 seeds from 200,000, Release, final code | 0 failures; 11 inconclusive (instruction or step limit); 7,492 programs ended in an identical runtime error |
| difftest, 3,000 seeds under ASan + UBSan, final code | 0 failures |
| Fuzzing round 1 (before the optimizations), all 5 targets concurrently on 2 cores | lexer 231,147 runs / 300 s; compile 100,135 / 600 s; compile+run 218,088 / 600 s; bytecode 1,872,352 / 600 s; mutated bytecode 363,434 / 600 s — no crashes |
| Fuzzing round 2 (after computed goto, superinstructions, rotation) | bytecode 1,134,944 / 180 s; mutated bytecode 305,004 / 180 s; compile+run 177,485 / 180 s; compile 83,857 / 180 s — no crashes |

## Bugs found by the test infrastructure

| Found by | Bug | Fix |
| --- | --- | --- |
| difftest (first run) | match bindings in a global initializer had no frame slot (internal compiler error) | the `<globals>` function now maps its locals like any other function; regression test `global_match_binding.em` |
| unit test | the VM held a reference to a temporary module (crash) | the VM owns a copy of its module |
| difftest (20k seeds) | VM and reference interpreter disagreed on the text of `value too large` errors | one message format for each limit |
| difftest on GCC vs Clang | generated programs depended on the compiler (unsequenced `+` operands in the generator) | generator sequences its random choices; 300 seeds compared identical |
| LeakSanitizer | reference interpreter leaked reference cycles | containers registered weakly and emptied at exit |
| Clang | computed goto left the scope of a `std::string` (GCC accepted it silently) | inner scope in the `SCONCAT` handler |
| benchmarks + profile | fused compare-and-jump made loops 30% slower (conditional move before the indirect jump) | separate dispatch on both branch paths |

## Limitations of the test suite

* The program generator covers Int, Bool, Float, String, `[Int]`, one
  struct, one enum and `fn(Int) -> Int` closures; nested functions declared
  in blocks and deeper generic shapes are covered only by hand-written
  tests.
* Debugger and REPL are tested through scripted sessions, not exhaustively.
* Fuzzing runs were minutes long on a 2-core machine; longer campaigns
  (hours, with corpus minimization) have not been done.

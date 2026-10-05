# Final audit — Ember 0.1

Date: 2026-10-05. Scores are self-assessed against the project brief (0–10),
with the evidence each score rests on. Nothing here is claimed without a
test, a measurement, or a pointer to code.

## Final verification

| Check | Result | Evidence |
| --- | --- | --- |
| Debug build, GCC 13, `-Werror` | PASS | `cmake --build build` |
| Release build | PASS | `build-release` |
| ASan + UBSan build, Clang 18 | PASS | `build-asan` |
| Unit tests (61) | PASS | `ctest` |
| Golden programs (90) | PASS | `tests/run_golden.py` |
| CLI tests (31 checks) | PASS | `tests/cli_tests.py` |
| Full suite under ASan + UBSan (leak detection on) | PASS | `ctest` in `build-asan` |
| Differential test, 20,000 seeds, final code | PASS (0 failures, 11 inconclusive) | docs/TESTING.md |
| Fuzzing (5 targets, two rounds, minutes each) | PASS (no crashes) | docs/TESTING.md |
| Layering check (VM independent of compiler) | PASS | `scripts/check_layering.sh` |
| Benchmarks produce expected output | PASS | `bench/run.py --check-only` |
| Formatter check on examples | PASS | `ember fmt --check examples/*.em` |
| GitHub Actions CI | NOT RUN | workflow written (`.github/workflows/ci.yml`); no remote run in this environment |
| Long fuzzing campaigns (hours) | NOT RUN | only minute-scale runs were performed |
| Big-endian / Windows / macOS builds | NOT RUN | static_assert rejects big-endian; others untested |

## Scores

| Area | Score | Reasoning |
| --- | :-: | --- |
| Language design & spec | 8 | Complete written spec; closures, enums, match, checked arithmetic. No generics/modules (documented). |
| Lexer & parser | 9 | Error recovery, depth limit, fuzzed, exact diagnostics tested. |
| Semantic analysis | 8 | Captures, exhaustiveness, definite return, const folding; name errors stop type checking (no mixed-phase error reports). |
| IR & optimizer | 8 | Validator with dominance + kinds after every pass; trap-preserving folding. Optimizer is local, no SSA over variables. |
| Bytecode format & verifier | 9 | Versioned, checksummed, bounds-checked decoder, abstract-interpretation verifier, fuzzed with raw and mutated input. |
| VM | 8 | Independent library, runtime checks, limits, traces; computed goto. Single-threaded interpreter only. |
| GC | 7 | Precise, stress mode, no recursion; stop-the-world, allocation-bound workloads not optimized. |
| Tooling (debugger, REPL, formatter, profiler) | 7 | All working and tested by scripts; debugger lacks scope ranges and conditional breakpoints; REPL replays. |
| Testing | 9 | Unit, golden, CLI, differential vs independent interpreter, fuzzing, sanitizers; found real bugs. |
| Performance work | 8 | Profile-driven, A/B measured, failures documented. Absolute speed is that of a simple interpreter. |
| Documentation | 9 | All requested documents, ADRs, Mermaid diagram, honest limitations. |
| Security posture | 7 | Untrusted input handled defensively and fuzzed; explicitly not a sandbox. |

## Known limitations / future work

1. Allocation cost (inline storage for structs/enums/closures, size-class allocator).
2. Debug info with variable scope ranges; conditional breakpoints.
3. User modules, generics.
4. Generational or incremental GC.
5. Longer fuzzing campaigns with corpus minimization; CI run on GitHub.

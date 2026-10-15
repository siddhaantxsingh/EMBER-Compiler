# Requirements matrix (2026-10-14)

Statuses: VERIFIED / IMPLEMENTED BUT UNVERIFIED / PARTIAL / FAILED / BLOCKED / NOT IMPLEMENTED.
Evidence paths are under `docs/evidence/2026-10-14/`.

| # | Requirement | Baseline | Final | Evidence / notes |
|---|---|---|---|---|
| 1 | Clean CMake build | VERIFIED | VERIFIED | `baseline-build.log`; final build in `final-ctest-release.log` |
| 2 | Full test suite passes | VERIFIED (4/4) | VERIFIED (6/6 incl. 2 native tests) | `baseline-ctest.log`, `final-ctest-release.log` |
| 3 | Whole suite under ASan+UBSan | VERIFIED after installing libclang-rt-18-dev | VERIFIED (LLVM backend not built in this config) | `baseline-asan.log`, `final-asan-gcstress.log` |
| 4 | Native LLVM backend: typed IR -> LLVM IR, JIT | NOT IMPLEMENTED | VERIFIED | `src/native/`, `ember native`, ORC LLJIT, LLVM 18 |
| 5 | Backend subset: ints/floats/bools, arithmetic, control flow, calls | NOT IMPLEMENTED | VERIFIED | `native-difftest-final.log` |
| 6 | Backend: strings, arrays, structs, enums, closures | NOT IMPLEMENTED | VERIFIED | same; whole IR language is covered |
| 7 | Backend: garbage collection | NOT IMPLEMENTED | NOT IMPLEMENTED | heap is an arena freed at exit; 2 GiB arena limit -> exit 6 |
| 8 | Backend: unsupported programs rejected, not miscompiled | NOT IMPLEMENTED | VERIFIED (partial scope) | `check_support` + CLI rejects `--max-steps/--max-heap/--gc-stress/--profile` with exit 6; 5 test programs rejected this way |
| 9 | Backend differential vs VM on all test programs | NOT IMPLEMENTED | VERIFIED: 68 of 73 run/runtime-error/bench/example programs supported, 68/68 identical stdout, stderr (incl. stack traces) and exit code | `native-difftest-final.log` |
| 10 | Backend differential on generated programs | NOT IMPLEMENTED | VERIFIED: 3000 seeds, 0 failures, 1 inconclusive (both -O1 and -O0 IR) | `native-difftest-3000-seeds.log` |
| 11 | Optimization equivalence -O0 vs -O1 on all programs | PARTIAL (generated programs only, in difftest) | VERIFIED: 72/73 identical; the one difference is `instruction_limit.em`, where the instruction count depends on optimization by design | `opt-equivalence-O0.log` |
| 12 | GC stress mode over whole suite under ASan | PARTIAL (one test program + every 10th difftest seed) | VERIFIED: golden 91/91 with `--gc-stress` under ASan+UBSan | `final-asan-gcstress.log` |
| 13 | Upvalue capture semantics | VERIFIED (golden tests) | VERIFIED on VM and native; mutation "CloseUpvalues no-op" in the backend is caught | `native-mutation.log` |
| 14 | libFuzzer: lexer, parser/checker (fuzz_compile), run, bytecode loader+verifier | IMPLEMENTED BUT UNVERIFIED (README claim only) | VERIFIED: 60 s each, no crashes; exec counts recorded | `fuzz.log` |
| 15 | Fuzzing the native backend | NOT IMPLEMENTED | NOT IMPLEMENTED | covered only by the program generator |
| 16 | Benchmarks VM vs native | NOT IMPLEMENTED | VERIFIED (shared machine, median of 3) | `bench-native.log` |
| 17 | Benchmarks vs Python/Lua reference | NOT IMPLEMENTED | NOT IMPLEMENTED | Lua not installed; Python ports not written |
| 18 | Native backend under ASan | NOT IMPLEMENTED | NOT IMPLEMENTED | ASan config builds with `-DEMBER_LLVM=OFF` |
| 19 | AOT (object file / executable) output | NOT IMPLEMENTED | NOT IMPLEMENTED | only JIT and `--emit-llvm` text |
| 20 | CI runs the new tests | n/a | IMPLEMENTED BUT UNVERIFIED | `.github/workflows/ci.yml` updated; see final report for run status |

Counts (final): VERIFIED 14, IMPLEMENTED BUT UNVERIFIED 1, NOT IMPLEMENTED 5 (rows 7, 15, 17, 18, 19). See `docs/AUDIT_2026-10-14.md`.

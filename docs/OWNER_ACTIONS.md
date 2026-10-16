# Owner actions

1. **CI** was green on run 38740180634 (commit a7a19b3). Re-check after later pushes on the branch `ccr-a6e6c567-y4w7k8` at
   https://github.com/siddhaantxsingh/EMBER-Compiler/actions. Success: all
   jobs green (gcc-debug, gcc-release incl. native difftest, clang-asan-ubsan
   incl. GC stress suite, fuzz-smoke, benchmarks-smoke).
2. **Local prerequisites** (Ubuntu 24.04):
   `sudo apt-get install -y ninja-build llvm-18-dev clang libclang-rt-18-dev`.
   Without `llvm-18-dev` the build still works; `ember native` then exits 6.
3. **Merge**: review and open a PR from `ccr-a6e6c567-y4w7k8` into `main`
   yourself (no PR was created).
4. **Longer fuzzing** (optional): `cmake -S . -B build-fuzz -DEMBER_FUZZ=ON
   -DEMBER_ASAN=ON -DEMBER_UBSAN=ON -DEMBER_LLVM=OFF` with clang, then run each
   `build-fuzz/fuzz/fuzz_*` with `-max_total_time=3600`. Success: no `crash-*` files.
5. **Benchmarks on a quiet machine**: `python3 scripts/bench_native.py build/ember 5`.
   The recorded numbers were taken with load average ~11 on 4 vCPU.

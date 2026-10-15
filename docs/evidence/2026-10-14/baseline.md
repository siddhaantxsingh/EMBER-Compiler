# Baseline (2026-10-14, commit 776cc01, before any change)

Machine: Linux cloud container, 4 vCPU, 15 GiB RAM, shared with other jobs.

| Step | Command | Result | Log |
|---|---|---|---|
| Release build | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4` | OK, 44 s | `baseline-build.log` |
| Test suite | `ctest --test-dir build --output-on-failure` | 4/4 pass (difftest 300 seeds, unit, golden 90, cli) | `baseline-ctest.log` |
| ASan+UBSan (clang) | `-DEMBER_ASAN=ON -DEMBER_UBSAN=ON`, then ctest | first link failed: `libclang-rt-18-dev` missing; after `apt-get install libclang-rt-18-dev`, 4/4 pass | `baseline-asan.log` |
| LLVM | `llvm-config --version` | 18.1.3, but no headers; `apt-get install llvm-18-dev` worked through the proxy | `toolchain.log` |
| Docker | `docker info` | daemon running | `toolchain.log` |

Notes:
- The task brief said "~200 .em test programs". The repo has 90 under
  `tests/programs` (29 run, 26 runtime-error, 35 compile-error), plus 10 bench
  programs and 7 examples (107 in total).
- No native/LLVM backend existed (README listed "JIT compilation" as not provided).
- `scripts/check_layering.sh` was committed without the executable bit, so the
  CI step `./scripts/check_layering.sh` would fail with "Permission denied".
  Fixed in this branch.

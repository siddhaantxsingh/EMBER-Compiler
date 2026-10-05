# Progress log

Status: **version 0.1 complete** (2026-10-05). See docs/FINAL_AUDIT.md for
the verification table and limitations.

Build: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build`.

Done: spec and audit; front end; IR, validator, optimizer, loop rotation;
codegen with trace layout and superinstructions; bytecode format 1.1,
verifier, disassembler; VM (computed goto, counting-free loop), GC; natives;
CLI, debugger, REPL, formatter, profiler; unit/golden/CLI/differential tests,
fuzzers, sanitizer runs; benchmarks and A/B results; all documentation.

Next (future work): allocation performance, debug scopes, user modules.

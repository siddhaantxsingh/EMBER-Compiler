# Phase 0 — Architecture audit

Date of audit: 2026-10-04. Performed before any implementation code was written.

## Starting state

The repository was empty: no source, no build system, no tests, no
documentation. There was therefore nothing to preserve, no existing tests that
could be broken, and no legacy design constraints. Every component listed
below had to be designed and built from scratch.

| Area | State found | Risk |
| --- | --- | --- |
| Language definition | none | High: every later stage depends on precise semantics |
| Lexer / parser | none | Medium |
| Semantic analysis | none | High: unsound typing would let the VM receive ill-typed code |
| Intermediate representation | none | Medium |
| Bytecode format | none | High: untrusted input surface (files) |
| Virtual machine | none | High: memory safety, UB, resource exhaustion |
| Garbage collector | none | High: use-after-free / missed roots are silent |
| Tests | none | High: nothing to catch regressions |
| Build / CI | none | Low |
| Documentation | none | Low |

## Decisions taken in the audit

1. **Define the language first** (`docs/LANGUAGE_SPEC.md`) before writing the
   lexer, so every stage implements one written contract. Ember is small but
   non-trivial: static types, structs, enums with payloads and exhaustive
   `match`, first-class closures with by-reference capture, arrays, strings,
   integer overflow traps.
2. **Strict stage separation.** Front end (lexer, parser, resolver, type
   checker) → IR (CFG, typed temporaries) → optimizer → code generator →
   bytecode module → verifier → VM. The VM is a separate library that links
   only the bytecode and common libraries (`ember_vm` → `ember_bc` →
   `ember_common`); it cannot include compiler headers
   (`scripts/check_layering.sh`).
3. **Never trust bytecode.** Bytecode can come from a file, so the decoder
   bounds-checks every read and the verifier proves stack/kind safety by
   abstract interpretation before the VM will run it. The VM constructor
   refuses unverified modules. Remaining dynamic properties (object identity
   of references, initialization of globals/captured variables, indirect call
   signatures) are checked at run time.
4. **A reference interpreter for differential testing.** A tree-walking
   interpreter over the typed AST, sharing nothing after type checking with
   the compiler/VM, plus a random well-typed program generator. This is the
   main correctness oracle for the back end.
5. **Precise mark & sweep GC** with an explicit gray stack (no recursion), a
   stress mode that collects on every allocation, and all allocation sites
   written so that live values are on the VM stack before allocating.
6. **Bounded everything**: parser nesting depth, error count, source and
   bytecode size, call depth, operand stack, heap, string and array sizes,
   optional instruction budget. Each limit produces a deterministic error.
7. **Sanitizers and fuzzing from the start**: ASan/UBSan build types and
   libFuzzer targets for the lexer, parser, full pipeline, bytecode decoder,
   verifier and VM.
8. **Measure before optimizing**: optimizations beyond basic IR cleanup are
   only kept if benchmarks (≥3 runs, same machine, interleaved) show a gain.

## Out of scope for version 0.1 (documented as future work)

User-defined multi-file modules, generics, a JIT, incremental/generational
GC, Unicode-aware strings, exceptions that can be caught.

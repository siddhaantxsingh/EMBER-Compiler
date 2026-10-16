<div align="center">
<h1>Ember</h1>

<h3>A statically typed programming language, optimizing compiler, verified bytecode format, and standalone virtual machine --- built from scratch in C++20.</h3>

<p>
<img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white">
<img src="https://img.shields.io/badge/Compiler-From%20Scratch-7B68EE?style=for-the-badge">
<img src="https://img.shields.io/badge/VM-Stack%20Machine-2E8B57?style=for-the-badge">
<img src="https://img.shields.io/badge/Bytecode-Verified-FF8C00?style=for-the-badge">
<img src="https://img.shields.io/badge/Testing-Fuzz%20%2B%20Differential-DC143C?style=for-the-badge">
</p>
**Lexer → Parser → Resolver → Type Checker → Typed CFG IR → Optimizer →
Bytecode → Verifier → VM → GC**
</div>

------------------------------------------------------------------------

## ✦ Why Ember?

Most language projects stop at a parser and interpreter.

**Ember builds the entire execution stack.**

It has a statically typed source language, a multi-stage compiler, a
typed control-flow IR, optimization passes, a versioned/checksummed
bytecode format, an abstract-interpretation bytecode verifier, a
separate VM, a precise mark-and-sweep collector, and developer tooling.

The project is intentionally engineered around **explicit invariants,
adversarial inputs, measured performance, and documented trade-offs**.

> **The compiler produces code. The verifier decides whether the VM is
> allowed to execute it.**

------------------------------------------------------------------------

## 🧭 System map

``` mermaid
flowchart LR
    S[".em source"] --> L["Lexer"]
    L --> P["Parser"]
    P --> R["Name Resolver"]
    R --> T["Type Checker"]
    T --> I["Typed CFG IR"]
    I --> O["Optimizer"]
    O --> G["Bytecode Generator"]
    G --> F["Versioned .embc"]
    F --> V["Bytecode Verifier"]
    V --> VM["Standalone VM"]
    VM --> GC["Precise Mark-Sweep GC"]

    VM --> DBG["Debugger / REPL / Profiler"]
    G --> DIS["Disassembler"]
```

### Trust boundary

``` mermaid
flowchart TB
    A["Bytecode file<br/>possibly untrusted"] --> B["Header + version checks"]
    B --> C["Bounds-checked decoder"]
    C --> D["CRC-32 validation"]
    D --> E["Abstract-interpretation verifier"]
    E -->|valid| F["VM execution"]
    E -->|invalid| G["Reject before execution"]
```

The VM is a separate library and cannot directly depend on compiler
internals.

------------------------------------------------------------------------

## ✨ Language

``` ember
fn make_counter() -> fn() -> Int {
    var count = 0;

    return fn() -> Int {
        count += 1;
        return count;
    };
}

enum Shape {
    Circle(Float),
    Rect(Float, Float)
}

fn area(s: Shape) -> Float {
    return match s {
        Shape::Circle(r) => 3.14159 * r * r,
        Shape::Rect(w, h) => w * h,
    };
}

fn main() {
    let next = make_counter();

    next();
    print(next());                       // 2
    print(area(Shape::Rect(2.0, 3.0))); // 6.0
}
```

### Current language surface

-   static types with local inference
-   checked 64-bit integers
-   structs
-   enums with payloads
-   exhaustive `match`
-   first-class closures with captured variables
-   arrays
-   strings
-   `math`, `strings`, and `time` standard modules

------------------------------------------------------------------------

## 🧩 Compiler pipeline

``` mermaid
flowchart TB
    A["Source text"] --> B["Tokens"]
    B --> C["AST"]
    C --> D["Resolved AST"]
    D --> E["Typed AST"]
    E --> F["Typed CFG"]
    F --> G["Validated IR"]
    G --> H["Optimized IR"]
    H --> I["Stack bytecode"]

    subgraph Frontend
      B
      C
      D
      E
    end

    subgraph Middleend
      F
      G
      H
    end

    subgraph Backend
      I
    end
```

The semantic layer includes:

-   closure-capture analysis
-   bidirectional type checking
-   constant folding
-   exhaustiveness checking
-   definite-return analysis
-   nesting/depth limits
-   source-aware diagnostics

The IR validator checks structural invariants after optimization passes,
including control-flow and dominance-related constraints.

------------------------------------------------------------------------

## ⚙️ Optimization

The optimizer currently performs transformations including:

-   constant folding that preserves traps
-   branch folding
-   jump threading
-   basic-block merging
-   dead-code elimination
-   loop rotation
-   trace-oriented block layout
-   profile-selected superinstructions

The project uses **A/B measurements**, including regressions, rather
than claiming every optimization helped.

``` mermaid
flowchart LR
    A["Baseline VM"] --> B["Profile"]
    B --> C["Hypothesis"]
    C --> D["Implement one change"]
    D --> E["Benchmark suite"]
    E --> F{"Improvement?"}
    F -->|Yes| G["Keep + document"]
    F -->|No| H["Revert / document regression"]
```

Recorded work included computed-goto dispatch, counting-free default
loops, and superinstructions.

------------------------------------------------------------------------

## 🔬 Bytecode format

`.embc` is a versioned binary format with:

-   magic/version metadata
-   CRC-32 integrity checking
-   optional debug information
-   bounds-checked decoding
-   explicit validation before execution

The verifier performs abstract interpretation over bytecode rather than
relying only on compiler provenance.

This creates a useful systems boundary:

``` text
Compiler correctness
        ≠
Bytecode validity
        ≠
VM runtime safety
```

Each layer has its own checks.

------------------------------------------------------------------------

## 🧠 Runtime & memory

The VM provides:

-   direct-threaded dispatch
-   stack frames
-   closures
-   Lua-style upvalues
-   runtime checks for properties not proven statically
-   stack traces
-   execution/resource limits
-   configurable heap limits
-   string/array size limits
-   instruction budgets

Memory management uses a **precise mark-and-sweep collector** with an
explicit gray stack and GC stress mode.

------------------------------------------------------------------------

## 🛠️ Tooling

``` text
ember run examples/shapes.em
ember build prog.em -o prog.embc
ember run prog.embc
ember check prog.em
ember debug prog.em
ember repl
ember fmt --write prog.em
ember disasm prog.em
ember ir prog.em
ember ast prog.em
ember run --profile prog.em
ember native prog.em             # LLVM backend: JIT-compile and run
ember native --emit-llvm prog.em # print the optimized LLVM IR
```

### Diagnostics

``` text
error[type]: mismatched types in argument 1
  --> wrong_arg_type.em:3:13
  |
3 |     print(f(true));
  |             ^^^^
  = expected: Int
  = found: Bool
```

Runtime failures retain source-level stack information:

``` text
RuntimeError: division by zero
  at average (stats.em:4:14)
  at main (stats.em:9:11)
```

------------------------------------------------------------------------

## ⚡ Native backend (LLVM)

`ember native file.em` lowers the optimized typed IR to LLVM IR, runs
LLVM's O2 pipeline and JIT-compiles it with ORC (LLVM 18, optional at
build time). It covers the whole language, including strings, arrays,
structs, enums and closures. Runtime errors, messages and stack traces
match the VM exactly. It has no garbage collector, and VM-only options
(`--max-steps`, `--max-heap`, `--gc-stress`, `--profile`) make it exit
with status 6 instead of running. Details: `src/native/backend.h`,
[`docs/AUDIT_2026-10-14.md`](docs/AUDIT_2026-10-14.md).

Measured on 2026-10-14 (shared 4-vCPU container, load average ~11,
median of 3 runs, JIT compile time included): 2.2x to 10.8x faster than
the VM on the 10 bench programs (`docs/evidence/2026-10-14/bench-native.log`).

------------------------------------------------------------------------

## 🧪 Verification & testing

Ember uses several independent testing layers.

  Layer                                                         Recorded result
  ----------------------------- -----------------------------------------------
  Unit tests                                                   **61 / 61 PASS**
  Golden programs                                                   **91 PASS**
  CLI checks                                                        **31 PASS**
  Differential testing            **20,000 seeds, 0 failures, 11 inconclusive**
  libFuzzer targets                  **5 targets, no crashes in recorded runs**
  ASan + UBSan                                                         **PASS**
  Formatter checks                                                     **PASS**
  Layering check                                                       **PASS**
  Benchmark output validation                                          **PASS**

Re-run on 2026-10-14 (`docs/evidence/2026-10-14/`): ctest 6/6 (with the
native backend), ASan+UBSan 4/4, golden suite with `--gc-stress` under
ASan 91/91, VM -O0 vs -O1 identical on 72/73 programs (the other is the
instruction-budget test, which depends on optimization by design),
native backend identical to the VM on 68/68 supported programs and on
3000 generated programs, 5 fuzzers x 60 s with no crashes.

The original final audit also records what was **not** run: remote GitHub
Actions, hour-scale fuzzing, and cross-platform builds.

### Test the verifier

The test suite feeds malformed and mutated bytecode to the verifier.

### Test the compiler

The differential suite compares Ember execution against an independent
reference interpreter.

### Test the runtime

Sanitizers and GC stress mode exercise memory-management paths.

------------------------------------------------------------------------

## 📈 Performance

Measured on a 2-vCPU cloud VM; methodology is documented in
[`docs/PERFORMANCE.md`](docs/PERFORMANCE.md).

  Workload                                                          Result
  ----------------------------------- ------------------------------------
  `fib(32)`                                                   **\~0.17 s**
  20M-iteration integer loop                                  **\~0.71 s**
  Approx. bytecode instruction rate                           **\~0.5B/s**
  Profile-guided benchmark changes      **0--40% faster** across the suite

These are **project measurements**, not a claim that Ember approaches
optimized native-code compilers.

------------------------------------------------------------------------

## 🏗️ Build

Requirements:

-   CMake ≥ 3.20
-   C++20 compiler (GCC 12+ or Clang 15+)
-   Python 3
-   Ninja optional
-   optional: LLVM 18 dev files (`llvm-18-dev`) for `ember native`
-   for clang sanitizer/fuzzer builds: `libclang-rt-18-dev`

``` bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Sanitizers:

``` bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DEMBER_ASAN=ON \
  -DEMBER_UBSAN=ON

cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

------------------------------------------------------------------------

## 📚 Documentation map

  Area           Documentation
  -------------- --------------------------------------------------
  Language       [`docs/LANGUAGE_SPEC.md`](docs/LANGUAGE_SPEC.md)
  Architecture   [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
  Lexer          [`docs/LEXER.md`](docs/LEXER.md)
  Parser         [`docs/PARSER.md`](docs/PARSER.md)
  Type system    [`docs/TYPE_SYSTEM.md`](docs/TYPE_SYSTEM.md)
  IR             [`docs/IR.md`](docs/IR.md)
  Bytecode       [`docs/BYTECODE.md`](docs/BYTECODE.md)
  VM             [`docs/VM.md`](docs/VM.md)
  GC             [`docs/GC.md`](docs/GC.md)
  Debugger       [`docs/DEBUGGER.md`](docs/DEBUGGER.md)
  Testing        [`docs/TESTING.md`](docs/TESTING.md)
  Optimization   [`docs/OPTIMIZATION.md`](docs/OPTIMIZATION.md)
  Security       [`docs/SECURITY.md`](docs/SECURITY.md)
  ADRs           [`docs/adr/`](docs/adr/)
  Final audit    [`docs/FINAL_AUDIT.md`](docs/FINAL_AUDIT.md)

------------------------------------------------------------------------

## 🚧 Scope & limitations

Version **0.1**.

Not currently provided:

-   user-defined multi-file modules
-   generics
-   catchable exceptions
-   Unicode-aware string operations
-   incremental compilation
-   generational/incremental GC
-   garbage collection in the native backend (it uses an arena freed at exit)
-   ahead-of-time native executables (the LLVM backend is JIT only)

**Security:** Ember is not a hostile-code sandbox. Use an OS/container
boundary for untrusted programs.

------------------------------------------------------------------------

## Scorecard (2026-10-14 self-assessment)

| Area | Score | Why not higher |
|---|---|---|
| Fresher-portfolio strength | 8/10 | Strong, tested end-to-end stack, but a single author's evidence; no external users. |
| Engineering maturity | 7/10 | Good differential/fuzz/sanitizer coverage; native backend has no GC and is not run under ASan. |
| Deployment readiness | 5/10 | CI is green, but there are no releases, packages or install docs beyond a source build. |
| Completion of intended scope | 7/10 | Backend, opt-equivalence, GC stress and fuzzing done; native GC, AOT, reference-language benchmarks missing. |

<div align="center">
**A language project becomes a systems project when correctness, trust
boundaries, memory, performance, and failure modes are treated as
first-class design problems.**
</div>

# Name resolution and type system

`src/sema/resolver.{h,cc}`, `src/sema/checker.{h,cc}`, types and symbols in
`src/sema/types.{h,cc}`. The normative rules are in
[LANGUAGE_SPEC.md](LANGUAGE_SPEC.md); this document explains the
implementation.

## Resolver

Runs first, never looks at types.

* Scopes: builtins → globals → one scope per block / function body / `for`
  body / match arm. Redeclaring a name in the same scope is an error;
  shadowing an outer scope is allowed.
* Top-level structs, enums, functions, constants and globals are declared
  before any body is resolved, so declaration order of functions does not
  matter. Global *initializers* may only read globals declared earlier
  (`global 'b' is used before its declaration`); reading a later global
  indirectly through a function call is caught at run time
  (`uninitialized global`).
* Nested `fn` declarations in a block are bound at block entry so that they
  can be mutually recursive. Their bodies are resolved at their textual
  position, so they can only see `let`s declared before them.
* **Captures.** When an identifier resolves to a local of an enclosing
  function, the resolver marks the symbol `captured` and appends it to the
  capture list of every function between the use and the owner (outermost
  first). Each entry records whether the closure takes it from the parent's
  local slot or from the parent's own upvalue. This is exactly the
  information the code generator needs to build closures.
* Each function (top-level, nested, lambda, plus the synthetic `<globals>`
  initializer function) gets an `FnInfo` with its locals in slot order
  (parameters first) and its captures.
* Mutability: only `var` bindings can be assigned; parameters and `let`
  are immutable. Fields of structs and elements of arrays are always
  assignable (the containers are mutable reference types).

## Type checker

* Types are interned (`TypeContext`), so type equality is pointer equality.
  Struct and enum types are nominal.
* **Bidirectional checking**: `check_expr(e, expected)` passes the expected
  type down. This is what types `[]`, lambdas in argument position, and
  `match` arms.
* **Error type**: an ill-typed subexpression gets the `Error` type, which is
  compatible with everything, so one mistake does not produce a cascade.
* Calls: builtins have hand-written rules (`print(any)`, `len([T] | String)`,
  `push([T], T)` ...); std-module functions have signatures in the native
  table; user functions and closures are checked against their function type.
* **Constants** are folded at compile time with exactly the runtime
  semantics (overflow and division by zero are compile errors in a
  constant). The checker's evaluator and the IR constant folder are
  separate implementations; unit tests compare both against the VM.
* **Definite return**: a function with a non-Unit result must end in a
  `return`, an `if/else` whose branches all return, or a `while true` loop
  without `break` (conservative; documented in the spec).
* **Match**: arm types must agree; enum matches must be exhaustive, Int and
  String matches need `_`, Bool matches need both values or `_`; arms after a
  `_` or repeated variants are reported as unreachable.
* `Unit`-typed variables are rejected (`let u = f();` where `f` returns Unit)
  because they are almost always a mistake.

## Soundness argument (informal)

Every expression has a static type, and lowering maps each type to a value
*kind* (`Int`, `Float`, `Bool`, `Unit`, `Ref`). The IR validator re-checks
kinds after lowering and after optimization; the bytecode verifier re-checks
them again on the final code. The deeper structure of `Ref` values (which
struct, which element type) is not tracked by the verifier, so the VM checks
it at each use (see [VM.md](VM.md)); for compiled programs these checks never
fail, which the fuzz target `fuzz_run` asserts (`TypeMismatch` from compiled
code is treated as a crash).

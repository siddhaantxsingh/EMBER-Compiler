# Intermediate representation

`src/ir/ir.h` (data structures), `lower.cc` (AST → IR), `validate.cc`,
`opt.cc`, `print.cc` (`ember ir file.em` shows it).

## Shape

* A `Module` holds functions, struct/enum/global tables, a string pool and
  a signature pool, plus the indices of the global-initializer function and
  of `main`.
* A `Function` is a control-flow graph of basic blocks. Block 0 is the entry.
  Every block ends with exactly one terminator: `Jump`, `Branch`, `Return`
  or `Unreachable`.
* **Temporaries** are typed by kind and defined exactly once; a definition
  dominates all its uses (SSA without φ-nodes).
* **Locals** are mutable frame slots (parameters first, then declared
  variables, then hidden compiler locals). Reading a variable is an explicit
  `LoadLocal` into a temp, which makes evaluation order explicit: for
  `x + f()` the load of `x` happens before the call even if `f` assigns `x`
  through a closure.
* Values that flow across control flow (the result of `&&`, `||`, `match`)
  go through hidden locals rather than φ-nodes. This keeps the IR easy to
  validate and maps directly onto a stack VM.

Example (`ember ir`):

```
fn main() -> unit {
  locals: $0(x):int
  b0:
    %0:int = ConstInt 2
    %1:int = ConstInt 3
    %2:int = IAdd (%0, %1)
    StoreLocal $0(x) (%2)
    ...
}
```

## Lowering highlights

* `a && b` stores `a` into a hidden local, branches, evaluates `b` only on
  one path, and reloads the local at the join.
* Compound assignment `p OP= e` evaluates the place's subexpressions once:
  for `a[i] += e` the order is `a`, `i`, load `a[i]`, `e`, operation, store.
* `for i in a..b` keeps the counter and the limit in hidden locals; the loop
  variable is a fresh local per iteration, and if a closure captured it the
  `inc` block emits `CloseUpvalues` so each iteration's closures see their
  own value.
* Leaving a scope that declared captured variables emits `CloseUpvalues`
  with the lowest captured slot; `break` and `continue` close the scopes
  they leave.
* Nested functions are created at block entry. Captured `let`s of that
  block are first reset with `ClearLocal`, so a closure that runs too early
  observes "uninitialized" (a runtime error) instead of a stale value.
* `match` stores the scrutinee (and the enum tag) in hidden locals and
  tests arms in order; the final fall-through block is `Unreachable`
  because the checker proved exhaustiveness.

## Validator

`ir::validate(module)` returns a list of problems (each a compiler bug):
missing or misplaced terminators, out-of-range block targets, temps defined
twice or used before definition, **definitions that do not dominate their
uses** (dominators via Cooper–Harvey–Kennedy), operand and result kinds of
every instruction, local/upvalue/global/function/struct/enum/field/native
indices, call signatures, capture descriptors. The driver runs it after
lowering and after optimization; failures become `internal compiler error`.

## Optimizer

See [OPTIMIZATION.md](OPTIMIZATION.md). Passes: constant folding (never
folds an operation that would trap), branch folding, jump threading, block
merging, unreachable-block removal, dead-code elimination of pure
instructions.

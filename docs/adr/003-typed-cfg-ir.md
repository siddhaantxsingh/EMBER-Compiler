# ADR-003: Typed CFG IR with single-assignment temps and mutable locals

Status: accepted (2026-10).

## Context
Code generation directly from the AST mixes language semantics with stack
management; a full SSA IR with φ-nodes is more than a stack VM needs.

## Decision
A CFG of basic blocks; temporaries are typed by kind and assigned once;
variables are mutable frame slots accessed with explicit loads/stores;
values that merge (`&&`, `||`, `match`) go through hidden locals.

## Consequences
+ Evaluation order and control flow are explicit; the IR is easy to
  validate (dominance + kinds) and to optimize locally.
+ Maps 1:1 to frame slots in bytecode.
- Optimizations that need SSA over variables (global value numbering,
  register promotion) would require converting locals to SSA first.

# ADR-010: REPL by replaying the session

Status: accepted (2026-10).

## Context
A REPL needs to accept declarations and statements incrementally, but the
compiler is a whole-program compiler and the VM runs a whole module.

## Decision
Keep the accepted declarations and statements as text; compile them plus
the new input as a complete program, run it on a fresh VM, and print only
output beyond what was already shown. Rejected inputs are discarded.
A bare expression is retried as `print(expr);`.

## Consequences
+ No special compiler or VM mode; the REPL can never diverge from the
  normal semantics.
- Side effects are replayed (including `input()`, which is fed nothing),
  and each input costs a full compile + run of the session so far.

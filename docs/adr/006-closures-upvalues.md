# ADR-006: Closures capture variables by reference using upvalues

Status: accepted (2026-10).

## Context
Closures must share mutable captured variables with their creator and with
each other, and outlive the creating frame.

## Decision
Lua-style upvalues: a captured variable lives in its stack slot while the
frame is active ("open" upvalue pointing into the stack) and is moved into
the upvalue object when its scope ends (`CLOSE_UPVALUES`) or the frame
returns. The resolver computes per-function capture lists; nested closures
capture from the parent's upvalues.

## Consequences
+ Uncaptured variables cost nothing; captured ones stay in fast slots while
  the frame lives.
+ Loop variables are closed at the end of each iteration, so each
  iteration's closures see their own value.
- Nested functions bound at block entry can run before a captured `let` has
  executed; handled with `CLEAR_LOCAL` + a runtime `uninitialized variable`
  check instead of rejecting such programs statically.

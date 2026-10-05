# ADR-002: Stack-based bytecode VM

Status: accepted (2026-10).

## Context
Options: tree-walking interpreter, stack bytecode VM, register bytecode VM.

## Decision
A stack VM with typed instructions, frame-local slots, and per-function
`max_stack`; `switch` dispatch.

## Alternatives
* Tree-walking: simplest, but slow, and no separable compiled artifact to
  verify or store. (Kept as the *reference interpreter* for testing.)
* Register VM: executes fewer (but larger) instructions (Shi, Casey, Ertl
  and Gregg, "Virtual Machine Showdown: Stack Versus Registers", 2005/2008),
  but needs a more complex code generator and verifier (register
  allocation, liveness).

## Consequences
+ Compact encoding, simple verifier (abstract interpretation over a stack of
  kinds), simple code generator (stackifier with temp slots).
- More instructions per operation than a register VM; mitigated where
  profiling shows it matters (see OPTIMIZATION.md).

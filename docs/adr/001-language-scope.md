# ADR-001: Language scope: small, statically typed, expression-rich

Status: accepted (2026-10).

## Context
The project needs a language big enough to exercise a real compiler and VM
(closures, aggregates, pattern matching, a GC) but small enough to specify
precisely and test exhaustively.

## Decision
Ember: static types with local inference; Int (checked 64-bit), Float,
Bool, String, Unit, arrays, nominal structs (mutable, by reference), enums
with payloads and exhaustive `match`, first-class closures with
by-reference capture, `let`/`var` mutability, a few std modules. No
generics, no user modules, no exceptions in v0.1.

## Consequences
+ Every value has a statically known kind, which lets the bytecode be typed
  and verifiable (ADR-004).
+ Exhaustive matching and checked arithmetic make runtime errors explicit.
- No generics means std functions like `push` are builtins with special
  typing rules in the checker.
- Multi-file programs are future work.

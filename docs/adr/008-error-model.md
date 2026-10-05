# ADR-008: Error model: no catchable exceptions in the language

Status: accepted (2026-10).

## Decision
Runtime errors terminate the program with a message and stack trace (exit
status 3). Recoverable failures are modelled with enums
(`enum Parsed { Ok(Int), Err(String) }`). Inside the implementation, user
errors are diagnostics, malformed bytecode is a decode/verify error, runtime
errors are a VM exception converted at the `run()` boundary, and compiler
bugs are reported as internal errors (exit 70) instead of crashes.

## Consequences
+ No unwinding semantics to specify or implement in the VM; deterministic
  output for tests.
- Programs cannot recover from, e.g., an out-of-bounds index; they must
  check first.

# ADR-009: Differential testing against a reference interpreter

Status: accepted (2026-10).

## Context
Unit and golden tests only cover the cases someone thought of; the back end
(lowering, optimizer, code generator, verifier, VM, GC) has many
interacting parts.

## Decision
Maintain a deliberately simple tree-walking interpreter over the typed AST
(sharing nothing with the back end) and a generator of random, well-typed,
terminating programs. For each seed, compare stdout and runtime errors of:
the reference interpreter, VM at -O1, VM at -O0, decoded bytecode, and VM in
GC stress mode.

## Consequences
+ Found a real lowering bug on the first run (match bindings in global
  initializers had no frame slots).
+ Gives confidence to change the optimizer and VM.
- The generator only produces the constructs it knows about; the reference
  interpreter must be kept in sync with the spec.

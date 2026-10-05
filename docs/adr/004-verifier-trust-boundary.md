# ADR-004: The bytecode verifier is the trust boundary

Status: accepted (2026-10).

## Context
Bytecode can be loaded from files, so the VM cannot assume it came from the
compiler.

## Decision
Every module is verified before execution (the VM constructor enforces it).
The verifier proves stack discipline, operand kinds and index ranges by
abstract interpretation. Properties it cannot prove cheaply (object identity
of references, initialization of globals/captures, indirect call
signatures) are checked by the VM at run time and produce `type mismatch` /
`uninitialized ...` errors.

## Alternatives
* Trust compiled bytecode only (sign or checksum it): a checksum does not
  stop a crafted file.
* Fully typed verification of references (struct ids, element types):
  requires richer type information in the format; more complex. Run-time
  checks cost one compare per reference operation.

## Consequences
+ No crafted file can make the VM misread memory (fuzzed).
- A small runtime cost on reference operations.

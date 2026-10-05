# Security

## Threat model

| Input | Trusted? | Defence |
| --- | --- | --- |
| Source files | No (may be malformed or adversarial) | lexer/parser never crash; size limit 16 MiB; nesting limit 256; error count limit 50; fuzzed |
| Bytecode files (`.embc`) | No | bounds-checked decoder with size limit 64 MiB and count checks; CRC32 against accidental corruption; **verifier** before any execution; runtime checks for what the verifier cannot prove; fuzzed (raw bytes and mutated valid modules) |
| Program behaviour at run time | No (may loop, recurse, allocate without bound) | call-depth, operand-stack, heap, string, array limits; optional instruction budget; deterministic errors instead of crashes |
| CLI arguments | Yes (the user) | validated; bad values are usage errors |

## What is guaranteed

For any source text and any bytecode file, the implementation either
reports an error or executes the program with the language semantics; it
does not crash, read or write out of bounds, or exhibit undefined behaviour.
Evidence:

* All fuzz targets (`fuzz/`) built with AddressSanitizer and
  UndefinedBehaviorSanitizer; see `docs/TESTING.md` for the runs performed
  and their results.
* The full test suite runs under ASan + UBSan in CI.
* Unit tests that feed truncated files at every length, random bit flips,
  forged counts, and hand-written ill-typed bytecode (which must produce
  `type mismatch` runtime errors, not memory errors).

## What is NOT claimed: Ember is not a sandbox

Ember programs cannot open files, sockets or processes — the only I/O is
standard input (`input()`), standard output (`print`) and the clock
(`time.clock`). That makes the language *capability-poor*, but the
implementation has **not** been hardened or audited as a security sandbox
for hostile code: there is no OS-level isolation, CPU time is only bounded
if `--max-steps` is given, memory is bounded by the accounted heap limit
(an estimate, plus the fixed-size value stack and frame vector), and output
size is unbounded. Do not run untrusted programs with access to anything
you care about without an external sandbox (container, seccomp, rlimits).

## Side channels and denial of service

* `time.clock()` exposes a monotonic clock.
* Without `--max-steps`, a program can run forever.
* Deep recursion is bounded by `--max-depth`, but each frame also uses
  operand-stack space bounded by `--max-stack`.
* The garbage collector is stop-the-world: a large live heap means long
  pauses (bounded by the heap limit).

## Reporting

This is a portfolio project; report issues through the repository's issue
tracker.

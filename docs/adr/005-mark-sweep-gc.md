# ADR-005: Precise stop-the-world mark & sweep GC

Status: accepted (2026-10).

## Context
Ember has cyclic data (structs referring to each other, closures capturing
themselves), so reference counting alone leaks.

## Decision
Precise, non-moving mark & sweep with an explicit gray stack, roots supplied
by the VM, threshold = 2 × live (min 1 MiB), stress mode that collects on
every allocation.

## Alternatives
* Reference counting + cycle collection: more complex, write barriers on
  every store.
* Copying/compacting: faster allocation, but moving objects requires
  updating every pointer (including open upvalues and native temporaries).
* Generational/incremental: lower pause times, needs write barriers.

## Consequences
+ Simple and verifiable; stress mode + ASan catch missing roots.
- Pause time proportional to the live heap; no compaction.

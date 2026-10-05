# Memory management

Two separate concerns: memory safety of the implementation (C++), and
memory management of Ember programs (the garbage-collected heap, see
[GC.md](GC.md)).

## Implementation memory safety

* **Ownership.** The AST is a tree of `unique_ptr`. Semantic objects
  (symbols, types, function infos) are owned by `SemaResult` containers of
  `unique_ptr`, so the raw pointers stored as AST annotations stay valid
  for the lifetime of the compilation. `CompileOutput` is heap-allocated and
  never moved because `TypeContext` hands out pointers to its members.
* **The VM owns a copy of its module.** An earlier version held a
  reference; a unit test that passed a temporary module crashed under the
  debugger, so the API was changed to take the module by value.
* **No unchecked indexing on untrusted data.** The bytecode decoder checks
  every read; the verifier proves every index operand in range before
  execution; the VM checks every array/string index and object type at
  run time.
* **No recursion on untrusted depth.** The parser's recursion is bounded at
  256; the GC marks with an explicit gray stack; value printing stops at
  depth 32 and at the string-size limit (cyclic structures print `...`).
  The VM itself does not recurse for Ember calls (frames live in a vector).
* **No undefined behaviour in arithmetic.** Integer operations use
  `__builtin_*_overflow`; `INT_MIN / -1` and `% -1` are special-cased;
  float→int conversion is range-checked before the cast; literals are
  overflow-checked while lexing.
* **Unaligned operand reads** use `memcpy` (`bc::read_operand`).
* **Sanitizers.** The whole test suite (unit, golden, CLI, differential) runs
  under AddressSanitizer + UndefinedBehaviorSanitizer (CI job
  `clang-asan-ubsan`), and all fuzz targets are built with both.

## Program memory

* All Ember reference values live on the GC heap; the VM's value stack is a
  single preallocated array (stable addresses, required by open upvalues).
* Every allocation is accounted (object header + payload estimate) against
  the heap limit; array growth is accounted on each `push`; exceeding the
  limit after a full collection is the runtime error `out of memory`.
* Host allocation failure (`std::bad_alloc`) is also reported as
  `out of memory` rather than crashing.

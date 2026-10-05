# Garbage collector

`src/vm/heap.{h,cc}`. A precise, non-moving, stop-the-world mark & sweep
collector (decision record: ADR-005).

## Allocation

`Heap::alloc<T>(payload_bytes, ctor args...)`:

1. `reserve(bytes)`: collect if in stress mode or if the accounted live size
   would pass the threshold; if the heap limit would still be exceeded,
   collect once more and otherwise throw `OutOfMemory`.
2. Construct the object, link it into the all-objects list, account its
   size.

`Heap::grow(obj, bytes)` accounts payload growth (array `push`) and may
collect; `shrink` gives bytes back (array `pop`).

## Roots

Supplied by the VM through a callback:

* the value stack `[stack_base, sp)`;
* globals;
* string constants of the module (allocated once at start-up);
* the closure of every active frame;
* open upvalues;
* temporary roots registered by natives while building several objects.

## Marking and sweeping

* Marking pushes newly marked objects on an explicit **gray stack** and
  traces them iteratively: arrays/structs/enums trace their values, closures
  their upvalues, closed upvalues their value. Deep lists or cycles never
  recurse on the C++ stack.
* Sweeping walks the all-objects list, frees unmarked objects and clears the
  mark on survivors.
* The next threshold is `max(1 MiB, 2 × live bytes)`, so collection work is
  proportional to allocation (amortized O(1) per allocated byte).

## The allocation-site invariant

A collection may happen at any allocation. Therefore every VM instruction
that allocates keeps its operands **on the VM stack** until the new object
exists, and writes the stack pointer back before allocating. Examples:

* `SCONCAT` builds the new `std::string` (plain C++ memory) first, then
  allocates the string object while both operands are still on the stack.
* `CLOSURE` pushes the new closure onto the stack *before* capturing
  upvalues, because creating an upvalue object allocates.
* `strings.split` allocates the result array, registers it as a temporary
  root, then allocates each piece.

## Testing the collector

* **Stress mode** (`--gc-stress`, `VMOptions::gc_stress`) collects before
  *every* allocation, turning any missing root into an immediate
  use-after-free that AddressSanitizer reports. The differential tester runs
  every 10th generated program in stress mode and compares output with the
  reference interpreter; unit test `gc_keeps_values_live_across_allocation_sites`
  touches every allocating instruction and native under stress.
* `gc_frees_unreachable_cycles`: 5,000 self-referencing structs in a 2 MiB
  heap must succeed and free >10,000 objects.
* Golden tests `gc_churn`, `gc_cycles`, `gc_stress_closures` and the
  `out_of_memory` runtime-error test.

## Limitations

Stop-the-world pauses grow with the live heap (no incremental or
generational collection). The size accounting is an estimate (it counts
object headers and payload capacity but not allocator overhead). Both are
listed as future work.

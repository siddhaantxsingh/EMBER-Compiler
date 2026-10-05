# Virtual machine

`src/vm/vm.{h,cc}`, values and objects in `value.h`, heap in `heap.{h,cc}`,
natives in `natives_rt.cc`.

## Independence

The VM links only `ember_bc` and `ember_common`. It accepts a `bc::Module`
(compiled in-process or decoded from a file), takes its own copy, and
verifies it in the constructor; an invalid module raises `InvalidModule` and
nothing runs.

## Values

`Value` is a 16-byte tagged union: `Unit`, `Bool`, `Int` (i64), `Float`
(f64), `Ref` (pointer to a heap object), and `Uninit` (a global or
captured variable that has not been assigned yet). It is trivially
constructible, so the preallocated value stack is not touched until used.

Heap objects: `StringObj`, `ArrayObj` (element kind + vector of values),
`StructObj` (struct id + fields), `EnumObj` (enum id, variant, payload),
`ClosureObj` (function index + upvalues), `UpvalueObj`.

## Interpreter loop

* Direct-threaded dispatch (computed goto, GCC/Clang) with a portable
  `switch` fallback (`-DEMBER_COMPUTED_GOTO=0`). The loop is a template on
  `<debugger attached, counting instructions>`; the default instantiation
  has neither the hook call nor the budget check (measured in
  [OPTIMIZATION.md](OPTIMIZATION.md)).
* Conditional jumps dispatch separately on the taken and fall-through paths
  so that the compiler cannot turn them into a conditional move feeding the
  indirect jump (that cost up to 30%, measured).
* The instruction pointer, frame base and stack pointer are kept in locals;
  the stack pointer is written back to the VM (`SYNC`) before anything that
  can allocate, call, or throw, because the GC scans `[stack, sp)`.
* Calls push a `Frame {function, ip, base, return slot, closure}`; the
  callee's arguments are already in place as its first locals; remaining
  locals are initialized to the default of their kind (`Uninit` for Ref
  slots). Call depth and stack space are checked on every call.
* `RETURN` closes open upvalues of the frame, writes the result into the
  return slot (the callee slot for indirect calls) and pops the frame.

## Runtime checks

| Check | Error |
| --- | --- |
| integer overflow (add, sub, mul, neg, `INT_MIN / -1`, `abs`) | `integer overflow` |
| division / remainder by zero | `division by zero` |
| array / string index, `substr`, `char_code` bounds | `index out of bounds` |
| `int()` of NaN or out-of-range float, `parse_int` | `invalid conversion` |
| `pop` on empty array | `empty array` |
| call depth, operand stack space | `stack overflow` |
| heap limit | `out of memory` |
| string length, array length | `value too large` |
| negative `[v; n]`, `repeat` count, `from_char_code` range | `invalid argument` |
| global read before its initializer ran | `uninitialized global` |
| captured variable read before its declaration ran | `uninitialized variable` |
| instruction budget | `instruction limit exceeded` |
| object type of a `Ref` operand, indirect-call signature | `type mismatch` (unreachable from compiled code) |

`type mismatch` exists so that bytecode that passes the verifier but was
not produced by the compiler (a hand-written or fuzzed file) fails safely
instead of misinterpreting memory.

## Errors and stack traces

A runtime error throws `VM::Error` inside the loop; the handler records the
faulting instruction of the top frame, and `run()` converts it into a
`RunResult` with the innermost-first list of `(function, line, column)`
derived from line tables. Traces longer than 20 frames are abbreviated
(innermost 10, outermost 5). Exit status 3.

## Natives

`CALL_NATIVE id` calls into a table that matches `common/natives.h`:
`print str input parse_int panic assert`, then the std modules
(`math`, `strings`, `time`). Native arguments stay on the VM stack during the
call, so allocations inside natives are safe; natives that build several
objects (`strings.split`) root intermediate results with temporary roots.

## Limits (all configurable from the CLI)

| Limit | Default | Flag |
| --- | --- | --- |
| call depth | 10,000 frames | `--max-depth` |
| operand stack | 1,048,576 values | `--max-stack` |
| heap | 256 MiB | `--max-heap` |
| string length | 64 MiB | (API) |
| array length | 16,777,216 | (API) |
| instruction budget | unlimited | `--max-steps` |

## Debugger hook

`set_debug_hook(DebugHook*)` selects the instrumented loop, which calls
`before_instruction(vm)` with the frame state synchronized; introspection
APIs (`frame()`, `global()`, `format_value()`) let the debugger read but
not modify state. See [DEBUGGER.md](DEBUGGER.md).

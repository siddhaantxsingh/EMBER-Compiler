# Debugger

`ember debug program.em` runs the program under a source-level debugger
(`src/tools/debugger.{h,cc}`), implemented as a `vm::DebugHook`. The VM
uses a separately instantiated interpreter loop when a hook is attached, so
normal execution pays nothing for the debugger.

The debugger stops at the first source line that executes (a global
initializer if there is one, otherwise the first line of `main`) and then
accepts commands:

| Command | Effect |
| --- | --- |
| `s`, `step` | run until a different source line (entering calls) |
| `n`, `next` | run until the next line in the current function or a caller |
| `finish` | run until the current function returns |
| `c`, `continue` | run until a breakpoint or the end |
| `si`, `stepi` | execute one bytecode instruction |
| `b LINE`, `break FN` | breakpoint at a line or at a function's entry |
| `delete LINE/FN`, `info breakpoints` | manage breakpoints |
| `bt` | call stack with source lines |
| `locals` | named locals and captured variables of the current frame |
| `p NAME` | a local, captured variable or global |
| `l [LINE]` | source around a line |
| `disasm` | bytecode of the current function |
| `q` | stop the program |

Commands are read from standard input, so sessions can be scripted (the CLI
tests do this). At end of input the program runs to completion.

```
$ printf 'break 4\ncontinue\nlocals\nbt\nquit\n' | ember debug stats.em
main at stats.em:8
  8 |     print(average([1, 2, 3]));
(edb) breakpoint at line 4
(edb) average at stats.em:4
  4 |     return s / len(xs);
(edb)   xs = [1, 2, 3]
  s = 6
  x = 3
(edb) #0 average (stats.em:4)
#1 main (stats.em:8)
(edb) program stopped
```

## How it works

* Line tables map each pc to a source line and column; local names come
  from the debug info in the bytecode (compile without `--no-debug-info`).
* A stop is triggered when execution reaches the first instruction of a new
  line (line changed, or a different frame) and the current mode says so;
  breakpoints are checked at the same points.

## Limitations

* Locals are shown for every named slot of the function, including
  variables whose scope has not started yet (they show their default
  value) — the debug info has no scope ranges yet.
* No conditional breakpoints, watchpoints or expression evaluation.
* The debugger needs the source file (it refuses `.embc` files).

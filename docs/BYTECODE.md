# Bytecode

`src/bc/opcodes.h` (instruction set), `module.h` (in-memory module),
`format.cc` (file encoding), `verify.cc` (verifier), `disasm.cc`
(`ember disasm`).

## Execution model

A stack machine with a frame per call:

```
| caller's operand stack ... | [closure] | arg0 .. argN-1 | locals ... | operand stack (≤ max_stack) |
                                          ^ frame base
```

Arguments become the first locals. Each function declares its number of
local slots and the maximum operand-stack depth it needs, so the VM checks
for stack space once per call. Locals and operands are tagged 16-byte
values.

## Instruction encoding

One opcode byte followed by fixed-size little-endian operands. Formats:
`b` u8, `h` u16, `w` u32, `q` i64, `d` f64. The table in `opcodes.h` is the
single source of truth used by the encoder, verifier, disassembler and VM.
Instructions are **typed**: integer, float, bool and string operations are
separate opcodes, so the VM never inspects tags for arithmetic.

| Group | Opcodes |
| --- | --- |
| stack | `NOP POP PUSH_INT q PUSH_FLOAT d PUSH_TRUE PUSH_FALSE PUSH_UNIT PUSH_STR w` |
| variables | `LOAD_LOCAL h STORE_LOCAL h CLEAR_LOCAL h LOAD_UPVAL h STORE_UPVAL h LOAD_GLOBAL h STORE_GLOBAL h` |
| integer | `IADD ISUB IMUL IDIV IMOD INEG` (trap on overflow / division by zero) |
| float | `FADD FSUB FMUL FDIV FNEG` |
| conversion | `NOT I2F F2I` |
| comparison | `IEQ INE ILT ILE IGT IGE`, `FEQ ... FGE`, `BEQ BNE`, `SEQ ... SGE` |
| strings | `SCONCAT SLEN SINDEX` |
| calls | `CALL w` (function), `CALL_INDIRECT h` (signature), `CALL_NATIVE h` (native id), `RETURN` |
| closures | `CLOSURE w` (function; captures described in its metadata), `CLOSE_UPVALUES h` (lowest slot) |
| structs | `MAKE_STRUCT h GET_FIELD hh SET_FIELD hh` |
| arrays | `MAKE_ARRAY bw ARRAY_REPEAT b ARRAY_GET b ARRAY_SET b ARRAY_LEN ARRAY_PUSH b ARRAY_POP b` (b = element kind) |
| enums | `MAKE_ENUM hh ENUM_TAG h ENUM_FIELD hhh` |
| control | `JUMP w JUMP_IF_FALSE w JUMP_IF_TRUE w UNREACHABLE` (absolute pc within the function) |
| superinstructions (1.1) | `LOAD_LOCAL2 hh` (push two locals), `STORE_LOAD hh` (store one local, push another), `IJEQ IJNE IJLT IJLE IJGT IJGE w` (pop two Ints, jump if the comparison holds) |

Opcode numbers are part of the file format: new opcodes are appended and
bump the minor version. Version 1.1 added the superinstructions, chosen
from opcode-pair profiles and kept because they measured faster
([OPTIMIZATION.md](OPTIMIZATION.md)); the code generator emits them only at
`-O1` and never across a block boundary.

## Code generation (IR → bytecode)

`src/codegen/codegen.cc` maps IR temporaries onto the operand stack:

* a temp used once, in its defining block, stays on the stack when it is
  consumed in stack order;
* constants are re-materialized at each use (no slot, no stack lifetime);
* any other temp gets its own frame slot (`STORE_LOCAL` after definition,
  `LOAD_LOCAL` at uses);
* at `-O1`, blocks are ordered by greedy traces so the common successor falls
  through, and adjacent load/load, store/load and int-compare/branch pairs
  are fused into superinstructions;
* if an instruction's operands are on the stack in the wrong order, the
  pending values are spilled to slots.

The operand stack is empty at every block boundary. A `Branch` whose true
target is the next block becomes `JUMP_IF_FALSE`, one whose false target is
next becomes `JUMP_IF_TRUE`, otherwise a conditional plus an unconditional
jump. The code generator records a line table
(pc → line, column) and local names for traces and the debugger.

## File format (`.embc`)

All integers little-endian.

```
header (20 bytes)
  magic        "EMBC"
  u16          major version (1)   -- a different major is rejected
  u16          minor version (1)   -- a newer minor than the VM is rejected
  u32          flags (bit 0: debug info present; other bits must be 0)
  u32          payload size (must equal file size - 20)
  u32          CRC-32 (IEEE) of the payload
payload
  strings      u32 count, each: u32 length + bytes
  structs      u32 count, each: str name, u16 nfields, (str name, u8 kind)*
  enums        u32 count, each: str name, u16 nvariants, (str name, u16 n, u8 kind*)*
  signatures   u32 count, each: u16 nparams, u8 kind*, u8 result kind
  globals      u32 count, each: str name, u8 kind
  functions    u32 count, each:
                 str name, u16 nparams, u8 result kind,
                 u16 nlocals, u8 kind*  (parameters first),
                 u16 nupvalues, (u8 kind, u8 from_local, u16 index)*,
                 u32 max_stack, u32 code length, code bytes,
                 [debug] u32 decl line, u32 nlines, (u32 pc, u32 line, u32 col)*,
                         u16 nlocals, str local name*, str upvalue name*
  u32 init function, u32 main function, str source name ("" without debug info)
```

**Decoding is defensive**: every read is bounds-checked; every count is
checked against the bytes remaining before anything is allocated (so a
forged count of 2^31 strings fails immediately with `too many strings`);
kinds must be valid; line tables must be sorted and inside the code;
trailing bytes are rejected; the file size is limited (64 MiB default). The
checksum catches accidental corruption (unit test: 500 random bit flips are
all detected); it is not a security mechanism — the verifier is.

## Verifier

`bc::verify(module)` must succeed before the VM runs anything (the VM
constructor calls it and refuses invalid modules). Per function:

1. Decode instruction boundaries; reject unknown opcodes and truncated
   operands.
2. Abstract interpretation over a stack of kinds, starting with an empty
   stack at pc 0, using a worklist. At every instruction it checks operand
   kinds, index ranges (locals, upvalues, globals, strings, functions,
   structs/fields, enums/variants/fields, signatures, natives), element kinds,
   and capture descriptors of `CLOSURE` against the creating function's
   locals/upvalues.
3. Every jump target must be an instruction start; all paths into a pc must
   agree on depth and kinds; depth never exceeds the declared `max_stack`;
   `RETURN` needs exactly the result kind on the stack; execution may not
   fall off the end.

Module level: entry functions (`main`, initializer) take no parameters,
return Unit and have no upvalues; `CALL` may not target a function that
needs upvalues.

What the verifier **does not** prove, and the VM checks at run time instead:
which object a `Ref` points to (string / array with element kind / struct id /
enum id and variant / closure), initialization of globals and captured
variables, and the signature of the closure in `CALL_INDIRECT`.

# Ember language specification (version 0.1)

Ember is a small, statically typed, garbage-collected language with
first-class closures, structs, enums with payloads and pattern matching. A
program is compiled to verified bytecode and run on the Ember VM. This
document is normative: the reference interpreter and the VM must agree with
it, and the differential tests check that they agree with each other.

## 1. Lexical structure

Source files are UTF-8. The lexer works on bytes; non-ASCII bytes may appear
only inside string literals and comments.

| Token | Rule |
| --- | --- |
| Whitespace | space, tab, `\r`, `\n` (separators only) |
| Line comment | `//` to end of line |
| Block comment | `/* ... */`, not nested; unterminated is an error |
| Identifier | `[A-Za-z_][A-Za-z0-9_]*`, not a keyword |
| Integer | decimal `[0-9][0-9_]*` or hex `0x[0-9a-fA-F_]+`; value must fit in a signed 64-bit integer, else a lexical error. `_` separators may not trail. A leading `-` is the unary operator, so `-9223372036854775808` is written as `-9223372036854775807 - 1`. |
| Float | `[0-9]+ '.' [0-9]+ ([eE] [+-]? [0-9]+)?`; must be finite |
| String | `"..."` with escapes `\n \t \r \0 \\ \" \xHH \u{H..H}` (code point ≤ 0x10FFFF, not a surrogate; encoded as UTF-8). A raw newline inside a string is an error. |
| Keywords | `fn let var const struct enum match if else while for in break continue return true false import` |
| Operators | `+ - * / % == != < <= > >= && \|\| ! = += -= *= /= %= -> => :: . .. , ; : ( ) [ ] { }` |

`_` alone is a token used in patterns.

## 2. Program structure

```
program   := item*
item      := fn_decl | struct_decl | enum_decl | const_decl | var_decl | import
import    := "import" IDENT ";"                    // std modules: math, strings, time
fn_decl   := "fn" IDENT "(" params? ")" ("->" type)? block
params    := IDENT ":" type ("," IDENT ":" type)* ","?
struct_decl := "struct" IDENT "{" (IDENT ":" type ("," IDENT ":" type)* ","?)? "}"
enum_decl := "enum" IDENT "{" variant ("," variant)* ","? "}"
variant   := IDENT ("(" type ("," type)* ")")?
const_decl := "const" IDENT (":" type)? "=" expr ";"   // expr must fold to a constant
var_decl  := ("let" | "var") IDENT (":" type)? "=" expr ";"
type      := IDENT | "[" type "]" | "fn" "(" (type ("," type)*)? ")" ("->" type)?
```

Every program must define `fn main()` with no parameters returning `Unit`.
Top-level functions, structs and enums are visible throughout the file
(declaration order does not matter). Globals (`let`/`var` at top level) are
initialized in source order before `main` runs; reading a global before its
initializer has run is a runtime error (`uninitialized global`).

## 3. Types

| Type | Values | Notes |
| --- | --- | --- |
| `Int` | signed 64-bit integers | overflow is a runtime error, never wraps |
| `Float` | IEEE 754 binary64 | |
| `Bool` | `true`, `false` | |
| `String` | immutable byte strings | `len` counts bytes |
| `Unit` | `()` | the type of statements and functions with no `->` |
| `[T]` | growable arrays of `T` | reference type, mutable |
| `fn(T1, ..) -> R` | functions and closures | reference type |
| struct `S` | records with named fields | nominal, reference type, mutable fields |
| enum `E` | one of several variants, each with zero or more payload values | nominal, reference type, immutable |

There are no implicit conversions. `float(i)` converts Int to Float;
`int(f)` truncates toward zero and fails at runtime for NaN or out-of-range
values. There are no null values; absence is modelled with enums.

Type inference: a `let`/`var` without an annotation takes the type of its
initializer. An empty array literal `[]` needs an expected type (an
annotation, a parameter, or a field type). Function parameters and return
types are always written explicitly.

## 4. Statements

```
stmt := var_decl | fn_decl | if | while | for | block
      | "break" ";" | "continue" ";" | "return" expr? ";"
      | place "=" expr ";" | place OP= expr ";" | expr ";"
if    := "if" expr block ("else" (if | block))?
while := "while" expr block
for   := "for" IDENT "in" expr ".." expr block      // Int range, end exclusive
       | "for" IDENT "in" expr block                 // array elements
place := IDENT | expr "[" expr "]" | expr "." IDENT
```

- Conditions must be `Bool`.
- `for i in a..b` evaluates `a` and `b` once; `i` is a fresh immutable
  variable per iteration. `for x in xs` evaluates `xs` once and iterates over
  indices `0..len(xs)` re-reading the array each iteration; if the array
  shrinks below the current index the loop ends.
- `break`/`continue` are only valid inside loops (not across a function
  boundary); `return` is valid in any function, and its value must match the
  declared return type. A function with a non-Unit return type must return
  on every path (checked conservatively: the last statement must be a
  `return`, or an `if`/`else` whose branches all end in `return`, or a
  `while true` loop with no `break`).
- An expression statement must be a call, a `match`, or an assignment. A
  `match` statement may omit its trailing semicolon.
- `x OP= e` is equivalent to `x = x OP e` except that the place's
  subexpressions (array and index, or object) are evaluated once.

## 5. Expressions and precedence

From lowest to highest precedence; all binary operators are left-associative.

| Level | Operators | Operand types | Result |
| --- | --- | --- | --- |
| 1 | `\|\|` | Bool | Bool (short-circuit) |
| 2 | `&&` | Bool | Bool (short-circuit) |
| 3 | `==` `!=` | Int, Float, Bool, String, Unit (same type on both sides) | Bool |
| 4 | `<` `<=` `>` `>=` | Int, Float, String | Bool |
| 5 | `+` `-` | Int, Float; `+` also String (concatenation) | operand type |
| 6 | `*` `/` `%` | Int, Float (`%` Int only) | operand type |
| 7 | unary `-` `!` | `-`: Int, Float; `!`: Bool | operand type |
| 8 | call `f(a)`, index `a[i]`, field `s.f` | | |

Comparison operators do not chain (`a < b < c` is a type error, since
`Bool < Int` is invalid).

Primary expressions: literals, identifiers, `(expr)`, array literals
`[e1, e2, ...]` and `[value; count]`, struct literals `S { f: e, ... }`
(all fields, any order), enum constructors `E::V` or `E::V(e, ...)`,
anonymous functions `fn(x: T) -> R { ... }`, and `match`.

`[v; n]` creates an array of `n` copies of the value `v`. For reference
types the copies are the same reference (`[[0; 3]; 2]` holds one inner
array twice); build distinct inner arrays with a loop. A negative `n` is an
`invalid argument` error.

A struct literal is not allowed directly in the condition of `if`, `while`,
`for` or the scrutinee of `match` (as in Rust); wrap it in parentheses.

**Evaluation order** is strictly left to right: operands, call arguments,
struct fields in source order, then the operation. For `a[i] = v` the order
is `a`, `i`, `v`, store.

**Integer arithmetic** traps on overflow (`integer overflow`); `/` truncates
toward zero; `%` has the sign of the dividend; division or remainder by zero
traps (`division by zero`); `INT_MIN / -1` traps as overflow, while
`INT_MIN % -1` is `0` (the mathematically exact result, which fits).

**Float arithmetic** follows IEEE 754 (division by zero gives ±inf or NaN).
`==` on floats is IEEE equality (`NaN != NaN`).

**String** comparison is bytewise lexicographic. `s[i]` returns a one-byte
string and traps when out of bounds.

## 6. Pattern matching

```
match := "match" expr "{" arm ("," arm)* ","? "}"
arm   := pattern "=>" (expr | block)
pattern := "_" | INT | "-" INT | STRING | "true" | "false"
         | IDENT "::" IDENT ("(" (IDENT | "_") ("," (IDENT | "_"))* ")")?
```

`match` is an expression. Arms are tried in order. All arms must have the
same type (a block arm has type `Unit`). Matching an enum must be
exhaustive: every variant is covered or a `_` arm exists. Matching `Int`,
`String` or `Bool` requires a `_` arm unless both booleans are listed. A
variant pattern binds each payload value to a fresh immutable variable.
Unreachable arms (after `_`, or repeating a variant) are errors.

## 7. Scoping and bindings

- Lexical (static) scoping. Each block, function body, `for` body and match
  arm opens a scope.
- `let` bindings and parameters are immutable; `var` bindings can be
  reassigned. Struct fields and array elements are always mutable,
  regardless of how the reference is bound.
- Redeclaring a name in the same scope is an error. Shadowing a name from an
  enclosing scope is allowed (including globals, functions and parameters).
- A nested `fn` declaration is bound in its enclosing scope before its body
  is checked, so it may call itself recursively.
- Types and values live in different namespaces: `Point` can be both a
  struct and (in a nested scope) a variable.

## 8. Functions and closures

Functions are first-class values. A nested `fn` or an anonymous `fn`
expression is a **closure**: it captures the variables it refers to from
enclosing functions **by reference**. Captured variables live as long as any
closure that refers to them, even after the declaring function returns, and
all closures that capture the same variable share it. A `var` declared in a
loop body is a fresh variable on each iteration, so closures created in
different iterations capture different variables.

Arguments are passed by value; for reference types (strings, arrays,
structs, enums, functions) the value is a reference. Recursion depth is
limited (default 10,000 frames); exceeding it traps with `stack overflow`.

## 9. Memory

All reference-type values are allocated on a garbage-collected heap and
freed automatically when unreachable (cycles included). There is no manual
memory management and no way to observe addresses. The heap size is
limited (default 256 MiB); exceeding it traps with `out of memory`.

## 10. Errors

**Compile-time errors** (lexical, syntax, name, type, limit) are reported
with file, line, column, the source line and a caret. Compilation reports
all errors it can (up to 50) and produces no bytecode if there is any.

**Runtime errors** stop the program. The message names the error and is
followed by a stack trace with function names and source locations:

```
RuntimeError: division by zero
  at average (stats.em:4:14)
  at main (stats.em:9:5)
```

Runtime error kinds: `division by zero`, `integer overflow`,
`index out of bounds`, `invalid conversion`, `invalid argument`,
`empty array`, `stack overflow`, `out of memory`, `uninitialized global`,
`uninitialized variable`, `panic`, `assertion failed`,
`instruction limit exceeded`, `value too large`.

`uninitialized variable` happens when a nested function runs before a
variable it captures has been declared, for example a nested function that
is called (directly or through another nested function) before the `let`
of a variable it reads. Nested functions are bound at block entry, so this
cannot be ruled out statically without rejecting useful programs.

Programs can raise errors with `panic(msg)` and `assert(cond, msg)`. Errors
cannot be caught; recoverable failures are expressed with enums, e.g.
`enum Parsed { Ok(Int), Err(String) }`.

## 11. Built-in functions

| Function | Type |
| --- | --- |
| `print(x)` | any printable type → Unit; writes the value and a newline |
| `input()` | → String; reads one line from stdin without the newline (empty string at EOF) |
| `len(x)` | `[T]` or String → Int |
| `push(xs, v)` | `([T], T)` → Unit |
| `pop(xs)` | `[T]` → T; traps on an empty array |
| `str(x)` | any printable type → String (same text `print` writes) |
| `int(f)` / `float(i)` | Float → Int / Int → Float |
| `parse_int(s)` | String → Int; accepts `-?[0-9]+` (no spaces, no `+`); otherwise traps (`invalid conversion`) |
| `panic(msg)` | String → Unit (never returns) |
| `assert(c, msg)` | (Bool, String) → Unit |

`print` formats: Int in decimal; Float as the shortest round-trip decimal
with at least one digit after the point (`2.0`, `0.1`, `1e+300`), `nan`,
`inf`, `-inf`; Bool as `true`/`false`; Unit as `()`; String as its bytes;
arrays as `[1, 2]` (strings inside containers are also printed as their
bytes, without quotes); structs as `Point { x: 1, y: 2 }`; enums as `Shape::Rect(2.0, 3.0)`
or `Option::None`; functions as `<fn name>`. Nesting deeper than 32 levels
prints `...` (this also bounds output for cyclic structures).

Standard modules (enabled by `import name;`, used as `name.fn(...)`):

| Module | Functions |
| --- | --- |
| `math` | `abs(Int)->Int`, `fabs(Float)->Float`, `min(Int,Int)->Int`, `max(Int,Int)->Int`, `sqrt(Float)->Float`, `pow(Float,Float)->Float`, `floor(Float)->Float`, `ceil(Float)->Float` |
| `strings` | `substr(String,Int,Int)->String`, `find(String,String)->Int` (-1 if absent), `upper(String)->String`, `lower(String)->String`, `split(String,String)->[String]`, `join([String],String)->String`, `char_code(String,Int)->Int`, `from_char_code(Int)->String`, `repeat(String,Int)->String` |
| `time` | `clock()->Float` (seconds, monotonic; non-deterministic) |

Details: `substr(s, start, length)` traps (`index out of bounds`) unless
`0 <= start <= len(s)` and `0 <= length <= len(s) - start`; `find` returns
the byte index of the first occurrence (`0` for an empty needle); `upper` and
`lower` change ASCII letters only; `split` with an empty separator splits
into single bytes; `char_code(s, i)` returns the byte value (0..255);
`from_char_code(c)` traps (`invalid argument`) unless `0 <= c <= 255`;
`repeat(s, n)` traps (`invalid argument`) for negative `n`; `sqrt` of a
negative number is `nan`; `abs(INT_MIN)` traps (`integer overflow`).

User-defined modules (multiple source files) are not part of version 0.1.

## 12. Limits

All limits produce deterministic errors and are configurable from the CLI.

| Limit | Default |
| --- | --- |
| Source size | 16 MiB |
| Bytecode file size | 64 MiB |
| Call depth | 10,000 frames |
| Operand stack | 1,048,576 values |
| Heap | 256 MiB |
| String length | 64 MiB |
| Array length | 16,777,216 elements |
| Instruction budget | unlimited (opt-in) |
| Nesting depth in the parser | 256 |

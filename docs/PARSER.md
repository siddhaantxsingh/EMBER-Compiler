# Parser

`src/front/parser.{h,cc}`, AST in `src/front/ast.h`.

## Technique

* **Recursive descent** for items, statements and patterns.
* **Pratt / precedence climbing** for binary operators, with the binding
  powers `|| (1) < && (2) < == != (3) < < <= > >= (4) < + - (5) < * / % (6)`,
  then unary `- !`, then postfix call / index / field. All binary operators
  are left-associative.
* Context flag `no_struct_lit_` disables struct literals in the head of
  `if`, `while`, `for` and `match`, so `if x == P { ... }` parses as a
  comparison followed by a block (as in Rust). Inside parentheses, brackets
  and call arguments the flag is reset.

## AST

Nodes own their children through `std::unique_ptr`. Every node has a
`Span`. The parser fills only syntax; name resolution and type checking
fill annotation fields (`sym`, `type`, `field_index`, `variant_index`,
`decl`, `info`) that later stages read. Calls of builtins and module
functions are ordinary call nodes until the resolver rewrites
`module.fn(...)` into a `ModuleCallExpr`.

## Error recovery

A syntax error throws an internal `SyntaxError` to the nearest
synchronization point after recording a diagnostic:

* inside a block: skip to the next `;` or to a token that starts a statement
  (`let var fn if while for return break continue struct enum const`) or `}`;
* at top level: skip to the next item keyword at brace depth 0.

This yields several useful errors per run instead of one, without cascades
from a single typo (golden test `compile_errors/multiple_errors.em`, unit
test `parser_recovers_and_reports_several_errors`). Diagnostics stop after
50 errors.

## Limits

`DepthGuard` bounds recursion to 256 nested expressions/statements and
reports `nesting is too deep (limit 256)`, so adversarial input cannot
overflow the C++ stack (unit test `parser_depth_limit` uses 10,000 nested
brackets and 5,000 nested blocks).

## Statement rules worth knowing

* An expression statement must be a call or a `match`; anything else is
  reported as `expression statement has no effect`.
* A `match` statement may omit its trailing semicolon.
* Assignment targets must be an identifier, an index expression or a field
  access; the check happens in the parser, mutability in the resolver.

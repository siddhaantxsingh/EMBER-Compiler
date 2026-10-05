# Lexer

`src/front/lexer.{h,cc}`, tokens in `src/front/token.h` (an X-macro list
shared by the token enum and `tok_desc`).

## Contract

* Input: the bytes of one `SourceFile` (any bytes; no encoding is assumed
  beyond ASCII for syntax; string literals may contain arbitrary bytes).
* Output: a vector of tokens ending with `Eof`, plus the list of comments
  (used only by the formatter).
* The lexer **never throws and never stops early**. Every problem becomes a
  diagnostic (`error[lexical]`) and an `Error` token, and lexing continues
  so later problems are still reported.
* Every token carries a `Span` (byte range, line, column) so diagnostics and
  debug info can point at it.

## Tokens

* Identifiers `[A-Za-z_][A-Za-z0-9_]*`; keywords are recognized after the
  identifier is scanned.
* Integers: decimal or `0x` hexadecimal, with `_` separators allowed only
  between digits (`1_000`, not `1__0`, `_1` or `1_`). Values are checked for
  overflow while scanning (no undefined behaviour on huge literals); a
  literal above `i64::MAX` is a lexical error. Negative literals do not
  exist: `-5` is unary minus applied to `5` (the parser handles `INT_MIN`
  through constant folding: `-9223372036854775807 - 1`).
* Floats: `digits '.' digits` with an optional exponent. A digit is required
  after the point so that `1.x` stays a field access and `0..n` stays a
  range. Values must be finite.
* Strings: `"..."` with escapes `\n \t \r \0 \\ \" \xHH \u{H..}` (the last
  encodes a code point ≤ 0x10FFFF as UTF-8, surrogates rejected). Raw
  newlines inside strings are an error (`unterminated string literal`),
  which keeps a missing quote from swallowing the rest of the file.
* Comments: `// ...` to end of line and `/* ... */` (not nested; an
  unterminated block comment is an error).
* Operators: longest match (`..`, `::`, `->`, `=>`, `==`, `!=`, `<=`, `>=`,
  `&&`, `||`, compound assignments `+= -= *= /= %=`).

## Robustness

* Size limits are enforced by the driver before lexing (16 MiB default).
* All scanning loops advance by at least one byte, so the lexer is linear in
  the input size.
* Fuzzed with `fuzz/fuzz_lexer.cc` (ASan + UBSan) and indirectly by every
  other fuzz target.

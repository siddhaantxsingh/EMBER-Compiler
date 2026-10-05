# ADR-007: Versioned binary file format with checksum and debug section

Status: accepted (2026-10).

## Decision
Little-endian, header with magic `EMBC`, major/minor version, flags, payload
size, CRC-32; sections for strings, types, signatures, globals, functions;
optional debug info (line tables, names). Readers reject a different major
version and a newer minor version. Opcode numbers are append-only.

## Consequences
+ Files are portable across machines with the same endianness (checked at
  build time) and robust against truncation/corruption (CRC + strict
  decoding).
+ `--no-debug-info` produces smaller files; traces then show function names
  without lines.
- Big-endian hosts are not supported (static_assert).

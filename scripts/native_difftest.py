#!/usr/bin/env python3
"""Differential test: VM (`ember run`) vs native LLVM backend (`ember native`).

Runs every .em program under tests/programs/{run,runtime_errors}, bench/programs
and examples. For each program the native backend either
  * rejects it (exit 6: outside the supported subset), or
  * must agree with the VM exactly: exit status, stdout and stderr
    (including runtime error messages and stack traces).
Programs that read stdin get their "// stdin:" text, as in run_golden.py.

Usage: native_difftest.py path/to/ember [--llvm-opt=N] [--verbose] [--vm-o0]

--vm-o0 instead compares `ember run -O1` (default) with `ember run -O0` on the
same programs (optimization equivalence for the bytecode pipeline).
Exit status 0 iff no supported program disagrees and at least one program
was supported.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ["tests/programs/run", "tests/programs/runtime_errors", "bench/programs", "examples"]


def directives(path):
    args, stdin = [], ""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f.readlines()[:3]:
            if line.startswith("// args:"):
                args = line[len("// args:"):].split()
            if line.startswith("// stdin:"):
                stdin = line[len("// stdin:"):].strip().encode().decode("unicode_escape")
    return args, stdin


def run(cmd, cwd, stdin):
    p = subprocess.run(cmd, cwd=cwd, input=stdin.encode(), capture_output=True, timeout=600)
    return p.returncode, p.stdout, p.stderr


def main():
    ember = os.path.abspath(sys.argv[1])
    extra = [a for a in sys.argv[2:] if a.startswith("--llvm-opt")]
    verbose = "--verbose" in sys.argv
    total = supported = agreed = 0
    rejected = {}
    failures = []
    for d in DIRS:
        full = os.path.join(ROOT, d)
        for name in sorted(os.listdir(full)):
            if not name.endswith(".em"):
                continue
            total += 1
            args, stdin = directives(os.path.join(full, name))
            vm = run([ember, "run", *args, name], full, stdin)
            if "--vm-o0" in sys.argv:
                nat = run([ember, "run", "-O0", *args, name], full, stdin)
            else:
                nat = run([ember, "native", *extra, *args, name], full, stdin)
            rel = f"{d}/{name}"
            if nat[0] == 6:
                reason = nat[2].decode(errors="replace").strip().split(": ", 2)[-1]
                key = reason.split(" (")[0].split(" in function")[0]
                rejected.setdefault(key, []).append(rel)
                if verbose:
                    print(f"REJECTED {rel}: {reason}")
                continue
            supported += 1
            if vm == nat:
                agreed += 1
                if verbose:
                    print(f"ok       {rel} (exit {vm[0]})")
            else:
                failures.append(rel)
                print(f"MISMATCH {rel}")
                print(f"  vm:     exit {vm[0]} stdout {vm[1][:300]!r} stderr {vm[2][:300]!r}")
                print(f"  native: exit {nat[0]} stdout {nat[1][:300]!r} stderr {nat[2][:300]!r}")
    what = "VM -O0" if "--vm-o0" in sys.argv else "native backend"
    print(f"\nprograms: {total}, supported by {what}: {supported}, agreed with VM -O1: {agreed}, "
          f"mismatches: {len(failures)}, rejected: {total - supported}")
    for k, v in sorted(rejected.items(), key=lambda kv: -len(kv[1])):
        print(f"  rejected ({len(v)}): {k}")
    return 0 if not failures and supported > 0 else 1


if __name__ == "__main__":
    sys.exit(main())

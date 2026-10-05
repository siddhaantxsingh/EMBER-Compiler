#!/usr/bin/env python3
"""Golden-file tests for the ember CLI.

Layout:
  tests/programs/run/X.em             expects exit 0, stdout == X.out
  tests/programs/compile_errors/X.em  expects exit 1, stderr == X.err
  tests/programs/runtime_errors/X.em  expects exit 3, stdout == X.out (if present), stderr == X.err

A first line "// args: ..." passes extra CLI options; "// stdin: TEXT" feeds
standard input (\\n escapes allowed). Run with --update to (re)write expected
files from the current output -- review the diff before committing.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
EXPECTED_EXIT = {"run": 0, "compile_errors": 1, "runtime_errors": 3}


def directives(path):
    args, stdin = [], ""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f.readlines()[:3]:
            if line.startswith("// args:"):
                args = line[len("// args:"):].split()
            if line.startswith("// stdin:"):
                stdin = line[len("// stdin:"):].strip().encode().decode("unicode_escape")
    return args, stdin


def main():
    ember = os.path.abspath(sys.argv[1])
    update = "--update" in sys.argv
    failures, count = [], 0
    for kind, code in EXPECTED_EXIT.items():
        d = os.path.join(ROOT, "programs", kind)
        for name in sorted(os.listdir(d)):
            if not name.endswith(".em"):
                continue
            count += 1
            path = os.path.join(d, name)
            base = path[:-3]
            args, stdin = directives(path)
            # Run from the program's directory with a relative path so that
            # file names in messages are stable.
            p = subprocess.run([ember, "run", *args, name], cwd=d, input=stdin.encode(),
                               capture_output=True, timeout=120)
            out = p.stdout.decode("utf-8", "replace")
            err = p.stderr.decode("utf-8", "replace")
            problems = []
            if p.returncode != code:
                problems.append(f"exit status {p.returncode}, expected {code}")
            checks = [("out", out)] if kind == "run" else [("err", err)]
            if kind == "runtime_errors":
                checks.append(("out", out))
            for ext, actual in checks:
                exp_path = f"{base}.{ext}"
                if update:
                    if actual or os.path.exists(exp_path):
                        with open(exp_path, "w", encoding="utf-8") as f:
                            f.write(actual)
                    continue
                expected = open(exp_path, encoding="utf-8").read() if os.path.exists(exp_path) else ""
                if actual != expected:
                    problems.append(f"{ext} differs:\n--- expected\n{expected}--- actual\n{actual}")
            if kind == "run" and err and not update:
                problems.append("unexpected stderr:\n" + err)
            if problems:
                failures.append(f"{kind}/{name}: " + "\n".join(problems))
    for f in failures:
        print("FAIL", f, "\n")
    print(f"golden: {count} programs, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

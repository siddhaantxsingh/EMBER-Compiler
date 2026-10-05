#!/usr/bin/env python3
"""End-to-end tests of the ember command-line interface: exit codes, bytecode
files (build/verify/corruption), formatter behaviour, debugger and REPL
sessions, inspection commands."""
import glob
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
failures = []


def run(args, stdin="", cwd=None):
    p = subprocess.run([EMBER, *args], input=stdin.encode(), capture_output=True, cwd=cwd, timeout=120)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def check(name, cond, detail=""):
    if not cond:
        failures.append(f"{name}: {detail}")


def main():
    tmp = tempfile.mkdtemp(prefix="ember-cli-")
    try:
        prog = os.path.join(tmp, "p.em")
        with open(prog, "w") as f:
            f.write('fn main() {\n    let xs = [1, 2, 3];\n    print(xs[1] * 10);\n    print(xs[5]);\n}\n')

        rc, out, err = run([])
        check("no arguments", rc == 2, rc)
        rc, out, err = run(["--help"])
        check("--help", rc == 0 and "usage:" in out, rc)
        rc, out, err = run(["--version"])
        check("--version", rc == 0 and out.startswith("ember "), out)
        rc, out, err = run(["frobnicate", prog])
        check("unknown command", rc == 2 and "unknown command" in err, err)
        rc, out, err = run(["run", "--bogus", prog])
        check("unknown option", rc == 2, rc)
        rc, out, err = run(["run", os.path.join(tmp, "missing.em")])
        check("missing file", rc == 4 and "cannot read" in err, err)
        rc, out, err = run(["run", prog, prog])
        check("too many files", rc == 2, rc)
        rc, out, err = run(["run", "--max-heap=abc", prog])
        check("bad limit value", rc == 2, err)

        # Runtime error: exit 3, partial output, trace.
        rc, out, err = run(["run", prog])
        check("runtime error exit", rc == 3 and out == "20\n" and "index out of bounds" in err and "p.em:4" in err, (rc, out, err))

        # build + run bytecode; same behaviour as source.
        embc = os.path.join(tmp, "p.embc")
        rc, out, err = run(["build", prog, "-o", embc])
        check("build", rc == 0 and os.path.exists(embc), err)
        rc2, out2, err2 = run(["run", embc])
        check("run bytecode", (rc2, out2) == (3, "20\n") and "p.em:4" in err2, (rc2, out2, err2))
        rc, out, err = run(["verify", embc])
        check("verify ok", rc == 0 and "ok" in out, (rc, out, err))
        rc, out, err = run(["disasm", embc])
        check("disasm bytecode", rc == 0 and "ARRAY_GET" in out, rc)

        # Without debug info, traces have no line numbers but still work.
        nodbg = os.path.join(tmp, "nodbg.embc")
        run(["build", "--no-debug-info", prog, "-o", nodbg])
        rc, out, err = run(["run", nodbg])
        check("no debug info", rc == 3 and "at main" in err and ":4:" not in err, err)
        check("no debug info smaller", os.path.getsize(nodbg) < os.path.getsize(embc))

        data = open(embc, "rb").read()
        # Corruption anywhere in the payload is caught by the checksum.
        for pos in (25, len(data) // 2, len(data) - 1):
            bad = bytearray(data)
            bad[pos] ^= 0x40
            path = os.path.join(tmp, f"bad{pos}.embc")
            open(path, "wb").write(bytes(bad))
            rc, out, err = run(["run", path])
            check(f"corrupt byte {pos}", rc == 5 and "checksum" in err, (rc, err))
        trunc = os.path.join(tmp, "trunc.embc")
        open(trunc, "wb").write(data[:30])
        rc, out, err = run(["verify", trunc])
        check("truncated", rc == 5, (rc, err))
        newer = bytearray(data)
        newer[6] = 9  # minor version 9
        path = os.path.join(tmp, "newer.embc")
        open(path, "wb").write(bytes(newer))
        rc, out, err = run(["run", path])
        check("newer version rejected", rc == 5 and "newer" in err, err)

        # Inspection commands.
        for cmd, needle in (("tokens", "identifier"), ("ast", "Fn main"), ("ir", "fn main"), ("check", "")):
            rc, out, err = run([cmd, prog])
            check(cmd, rc == 0 and needle in out, (rc, out[:200], err))

        # Formatter: idempotent and behaviour-preserving on every run test.
        for src in sorted(glob.glob(os.path.join(ROOT, "programs", "run", "*.em"))):
            name = os.path.basename(src)
            rc, f1, err = run(["fmt", src])
            check(f"fmt {name}", rc == 0, err)
            p1 = os.path.join(tmp, name)
            open(p1, "w").write(f1)
            rc, f2, err = run(["fmt", p1])
            check(f"fmt idempotent {name}", f1 == f2, "second pass differs")
            rc, out, err = run(["fmt", "--check", p1])
            check(f"fmt --check {name}", rc == 0, err)
            # Same program output after formatting (run from the original dir).
            args = []
            first = open(src).readline()
            if first.startswith("// args:"):
                args = first[len("// args:"):].split()
            stdin = ""
            if first.startswith("// stdin:"):
                stdin = first[len("// stdin:"):].strip().encode().decode("unicode_escape")
            r1 = run(["run", *args, src], stdin)
            r2 = run(["run", *args, p1], stdin)
            check(f"fmt preserves behaviour {name}", r1[:2] == r2[:2], (r1[:2], r2[:2]))
        messy = os.path.join(tmp, "messy.em")
        open(messy, "w").write("fn   main( ) {print( 1+2 ) ;   // keep me\n  let x=(1+2)*3;print(x);}\n")
        rc, out, err = run(["fmt", "--check", messy])
        check("fmt --check detects", rc == 1, rc)
        rc, out, err = run(["fmt", "--write", messy])
        formatted = open(messy).read()
        check("fmt --write", rc == 0 and formatted == "fn main() {\n    print(1 + 2); // keep me\n    let x = (1 + 2) * 3;\n    print(x);\n}\n", repr(formatted))

        # Debugger session.
        dbg_prog = os.path.join(ROOT, "programs", "runtime_errors", "division_by_zero.em")
        rc, out, err = run(["debug", dbg_prog], "break 4\ncontinue\nlocals\nbt\nprint s\nquit\n")
        check("debugger", "breakpoint at line 4" in out and "s = 6" in out and "#1 main" in out and rc == 0, out)
        rc, out, err = run(["debug", dbg_prog], "step\nstep\nnext\nfinish\ncontinue\n")
        check("debugger stepping to the error", rc == 3 and "division by zero" in err, (rc, out, err))

        # REPL session.
        rc, out, err = run(["repl"], "let x = 6;\nx * 7\nfn sq(n: Int) -> Int { return n * n; }\nsq(x)\nundefined_thing\n:quit\n")
        check("repl", "42" in out and "36" in out and "undefined name" in out, out)

        # Standard input.
        echo = os.path.join(tmp, "echo.em")
        open(echo, "w").write("fn main() {\n    print(input() + \"!\");\n}\n")
        rc, out, err = run(["run", echo], "hey\n")
        check("stdin", out == "hey!\n", out)
        # --stats goes to stderr and does not change stdout.
        rc, out, err = run(["run", "--stats", echo], "a\n")
        check("stats", out == "a!\n" and "instructions:" in err, err)
    finally:
        shutil.rmtree(tmp)
    for f in failures:
        print("FAIL", f)
    print(f"cli: {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    EMBER = os.path.abspath(sys.argv[1])
    sys.exit(main())

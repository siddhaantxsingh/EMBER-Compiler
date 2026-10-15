#!/usr/bin/env python3
"""Wall-clock time of each bench program: VM (`ember run`) vs native LLVM
backend (`ember native`, JIT compile time included and reported
separately). Median of N runs. Records load averages; results on a shared
machine are noisy. Usage: bench_native.py path/to/ember [runs]"""
import os, re, statistics, subprocess, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ember = os.path.abspath(sys.argv[1]); runs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
d = os.path.join(ROOT, "bench/programs")
print("load before:", os.getloadavg())
print(f"{'program':16} {'vm s':>8} {'native s':>9} {'jit s':>7} {'speedup':>8}  output-equal")
for name in sorted(os.listdir(d)):
    if not name.endswith(".em"): continue
    def t(cmd):
        xs, out, extra = [], None, None
        for _ in range(runs):
            t0 = time.perf_counter(); p = subprocess.run(cmd, cwd=d, capture_output=True); xs.append(time.perf_counter() - t0)
            out = (p.returncode, p.stdout)
            m = re.search(rb"compile time:\s+([0-9.e-]+)", p.stderr)
            if m: extra = float(m.group(1))
        return statistics.median(xs), out, extra
    vm, vo, _ = t([ember, "run", name])
    nat, no, jit = t([ember, "native", "--stats", name])
    print(f"{name:16} {vm:8.3f} {nat:9.3f} {jit or 0:7.3f} {vm/nat:7.1f}x  {vo == no}")
print("load after:", os.getloadavg())

#!/usr/bin/env python3
"""Benchmark runner.

Runs every program in bench/programs with `ember run`, checks its output
against bench/expected/<name>.out, and reports wall-clock time over N runs
(min / median / max). Results are written as JSON and Markdown to
bench/results/ when --save NAME is given.

Usage:
  bench/run.py --ember build-release/ember [--runs 5] [--save NAME] [--filter fib]
  bench/run.py --ember build/ember --check-only        # outputs only (CI)
  bench/run.py --compare A.json B.json                 # side-by-side table
  bench/run.py --ember A/ember --ember-b B/ember --runs 7   # interleaved A/B comparison
"""
import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))


def run_once(ember, prog, extra):
    t0 = time.perf_counter()
    p = subprocess.run([ember, "run", *extra, prog], capture_output=True, timeout=600)
    dt = time.perf_counter() - t0
    return dt, p.returncode, p.stdout.decode()


def compare(a_path, b_path):
    a = json.load(open(a_path))
    b = json.load(open(b_path))
    print(f"| benchmark | {a['label']} median (s) | {b['label']} median (s) | change |")
    print("| --- | ---: | ---: | ---: |")
    for name in sorted(a["results"]):
        if name not in b["results"]:
            continue
        x, y = a["results"][name]["median"], b["results"][name]["median"]
        print(f"| {name} | {x:.3f} | {y:.3f} | {(y - x) / x * 100:+.1f}% |")


def ab(args, a, b):
    """Interleaved A/B runs: A and B alternate within each round so that
    machine drift (thermal, other load) affects both equally."""
    names = sorted(f[:-3] for f in os.listdir(os.path.join(ROOT, "programs")) if f.endswith(".em"))
    names = [n for n in names if args.filter in n]
    rows = []
    print(f"| benchmark | {args.label_a} median (s) | {args.label_b} median (s) | change | {args.label_a} min | {args.label_b} min |")
    print("| --- | ---: | ---: | ---: | ---: | ---: |")
    for name in names:
        prog = os.path.join(ROOT, "programs", name + ".em")
        expected = open(os.path.join(ROOT, "expected", name + ".out")).read()
        ta, tb = [], []
        for i in range(args.runs):
            order = [(a, ta), (b, tb)] if i % 2 == 0 else [(b, tb), (a, ta)]
            for exe, acc in order:
                dt, rc, out = run_once(exe, prog, args.extra.split())
                if rc != 0 or out != expected:
                    print(f"{name}: WRONG OUTPUT from {exe}", file=sys.stderr)
                    return 1
                acc.append(dt)
        ma, mb = statistics.median(ta), statistics.median(tb)
        rows.append((name, ma, mb))
        print(f"| {name} | {ma:.3f} | {mb:.3f} | {(mb - ma) / ma * 100:+.1f}% | {min(ta):.3f} | {min(tb):.3f} |", flush=True)
    if args.save:
        os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
        with open(os.path.join(ROOT, "results", args.save + ".json"), "w") as f:
            json.dump({"label": args.save, "date": time.strftime("%Y-%m-%d %H:%M"), "machine": platform.platform(),
                       "cpu_count": os.cpu_count(), "runs": args.runs, "a": args.label_a, "b": args.label_b,
                       "rows": [{"name": n, "median_a": x, "median_b": y} for n, x, y in rows]}, f, indent=2)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ember")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--save")
    ap.add_argument("--filter", default="")
    ap.add_argument("--check-only", action="store_true")
    ap.add_argument("--compare", nargs=2)
    ap.add_argument("--extra", default="", help="extra ember run options, e.g. '--gc-stress'")
    ap.add_argument("--ember-b", help="second binary: run A and B alternately and compare medians")
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    args = ap.parse_args()
    if args.compare:
        compare(*args.compare)
        return 0
    ember = os.path.abspath(args.ember)
    if args.ember_b:
        return ab(args, ember, os.path.abspath(args.ember_b))
    names = sorted(f[:-3] for f in os.listdir(os.path.join(ROOT, "programs")) if f.endswith(".em"))
    names = [n for n in names if args.filter in n]
    results, failed = {}, False
    runs = 1 if args.check_only else args.runs
    for name in names:
        prog = os.path.join(ROOT, "programs", name + ".em")
        expected = open(os.path.join(ROOT, "expected", name + ".out")).read()
        times = []
        for _ in range(runs):
            dt, rc, out = run_once(ember, prog, args.extra.split())
            if rc != 0 or out != expected:
                print(f"{name}: WRONG OUTPUT (exit {rc})\n{out}", file=sys.stderr)
                failed = True
                break
            times.append(dt)
        if not times:
            continue
        results[name] = {"runs": times, "min": min(times), "median": statistics.median(times), "max": max(times)}
        if args.check_only:
            print(f"{name:16s} ok")
        else:
            print(f"{name:16s} min {min(times):7.3f}s  median {statistics.median(times):7.3f}s  max {max(times):7.3f}s")
    if args.save and not args.check_only:
        meta = {
            "label": args.save,
            "date": time.strftime("%Y-%m-%d %H:%M"),
            "machine": platform.platform(),
            "cpu_count": os.cpu_count(),
            "runs": runs,
            "ember": subprocess.run([ember, "--version"], capture_output=True).stdout.decode().strip(),
            "results": results,
        }
        os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
        with open(os.path.join(ROOT, "results", args.save + ".json"), "w") as f:
            json.dump(meta, f, indent=2)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

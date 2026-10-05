# Performance

All numbers were measured, not estimated. They describe one machine and are
meant for relative comparison, not as absolute claims.

* Machine: 2-vCPU cloud VM, Intel Xeon @ 2.80 GHz, 7 GiB RAM, Linux 6.18,
  Ubuntu 24.04.
* Build: GCC 13.3, CMake `Release` (`-O3`), commit with all optimizations of
  [OPTIMIZATION.md](OPTIMIZATION.md).
* Method: `python3 bench/run.py --ember build-release/ember --runs 5`;
  wall-clock time of the whole `ember run` process (compilation of these
  programs takes a few milliseconds); output checked against
  `bench/expected/` on every run.
* Raw data: `bench/results/final.json`.

## Benchmarks (10 categories)

| benchmark | category | what it does | min (s) | median (s) | max (s) |
| --- | --- | --- | ---: | ---: | ---: |
| fib | calls | naive `fib(32)` (≈7 M calls) | 0.165 | 0.172 | 0.187 |
| loop_sum | integer loop | 20 M iterations of `%`, `==`, `+=` | 0.701 | 0.712 | 0.725 |
| mandelbrot | floating point | 400×400 points, ≤100 iterations | 0.309 | 0.318 | 0.332 |
| sieve | arrays | sieve of Eratosthenes to 5 M | 1.175 | 1.217 | 1.377 |
| strings | strings | build, search, join and split 200 k strings | 0.180 | 0.186 | 0.195 |
| binary_trees | allocation / GC | binary trees up to depth 16 | 0.312 | 0.315 | 0.335 |
| closures | closures | 3 M indirect calls through composed closures | 0.285 | 0.290 | 0.306 |
| enum_dispatch | enums / match | 8 M steps of a stack-machine interpreter written in Ember | 0.612 | 0.622 | 0.671 |
| quicksort | recursion + arrays | sort 1 M integers | 0.822 | 0.834 | 0.873 |
| matrix | nested arrays + floats | 200×200 matrix product | 0.350 | 0.381 | 0.393 |

Rough throughput from `--profile` instruction counts: `loop_sum` executes
≈ 373 M bytecode instructions in ≈ 0.7 s, i.e. ≈ 0.5 G instructions/s
(≈ 1.9 ns per instruction) on this VM.

## Where the time goes

* Call-, loop- and float-heavy code is dominated by instruction dispatch
  and `LOAD_LOCAL`; see the profile in OPTIMIZATION.md.
* `binary_trees` and `strings` are allocation-bound: about half of
  `binary_trees`' instructions are in `malloc`/`free` (callgrind).
* GC (`ember run --stats`, the counting loop, single run):

  | benchmark | collections | GC time | total time | allocated | objects | peak heap |
  | --- | ---: | ---: | ---: | ---: | ---: | ---: |
  | binary_trees | 7 | 0.085 s | 0.298 s | 84.5 MB | 1.17 M | 57.4 MB |
  | strings | 10 | 0.072 s | 0.188 s | 52.9 MB | 0.60 M | 38.7 MB |

  So roughly a quarter to a third of these two programs is collection
  (stop-the-world mark & sweep, threshold 2× live heap), and most of the
  rest is allocation.

## Testing modes are not performance modes

* `--gc-stress` collects before every allocation; on allocation-heavy
  programs its cost grows with live heap × allocations (`binary_trees` did
  not finish within about 9 minutes). It exists to find missing GC roots.
* `--profile`, `--stats`, `--max-steps` and the debugger use the counting
  interpreter loop (up to ~15% slower, measured).
* Debug and sanitizer builds are much slower (unoptimized code,
  instrumentation).

## Reproducing

```
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
python3 bench/run.py --ember build-release/ember --runs 5 --save mine
python3 bench/run.py --compare bench/results/final.json bench/results/mine.json
# A/B of two builds or flags, interleaved:
python3 bench/run.py --ember old/ember --ember-b build-release/ember --runs 7
```

# C17 optimization report

## Method

Measurements were taken on 2026-09-01 on an Apple M4 running Darwin 25.6.0,
using Apple clang 21.0.0. The workloads are single-threaded. All builds use
`-O2`; both optimized releases also define `NDEBUG`. No native-architecture,
fast-math, PGO, or LTO flags are included.

Three versions were compared:

- commit `467b24b`, the original C17 implementation;
- commit `a7aa901`, the saved C++20 implementation of all optimizations; and
- the final C17 port, retaining the algorithmic, CRC, and POSIX changes.

Every process discarded one warm-up sample. The direct C17-versus-C++20
comparison uses two paired runs in opposite orders: 18 measured training and
evaluation samples per build and 22 checkpoint samples. Original-baseline
figures are the prior medians of nine training/evaluation samples and eleven
checkpoint samples. Higher throughput is better.

## Results

### Training

| Workload | Original C17 | C++20 optimized | Final C17 | C vs C++ | C gain vs original |
|---|---:|---:|---:|---:|---:|
| Kuhn CFR iter/s | 173,621 | 653,867 | 619,452 | -5.26% | +256.8% |
| Kuhn CFR+ iter/s | 170,034 | 637,005 | 608,882 | -4.41% | +258.1% |
| Leduc CFR iter/s | 1,327.64 | 8,267.50 | 8,026.16 | -2.92% | +504.5% |
| Leduc CFR+ iter/s | 1,322.12 | 8,124.43 | 8,014.67 | -1.35% | +506.2% |
| Leduc MCCFR iter/s | 115,908 | 502,156 | 498,085 | -0.81% | +329.7% |

All compared runs visited the same number of game-tree nodes for their
workload. Seeded MCCFR retained deterministic results.

### Evaluation and checkpoint I/O

| Workload | Original C17 | C++20 optimized | Final C17 | C vs C++ | C gain vs original |
|---|---:|---:|---:|---:|---:|
| Kuhn evaluations/s | 202,540 | 355,745 | 358,160 | +0.68% | +76.8% |
| Leduc evaluations/s | 2,128.12 | 7,724.28 | 7,631.90 | -1.20% | +258.6% |
| 8 MB checkpoint write | 129.94 MB/s | 291.29 MB/s | 292.84 MB/s | +0.53% | +125.4% |

Evaluation produced identical exploitability values before and after the
changes. Checkpoint tests verify byte format, checksums, restore behavior, and
continuous-versus-resumed training.

## What changed

- The entire implementation, applications, tests, benchmarks, and final links
  now build as strict C17. No C++ compiler or runtime library is required.
- Kuhn and Leduc now use the existing trusted-operation mechanism already used
  by blackjack. A traversal deeply validates its root, then uses callbacks that
  omit redundant full-history replay. Direct public game operations remain
  checked.
- Full-tree trainers reuse one growable traversal workspace for the whole
  `cfr_trainer_run` call instead of allocating four buffers per player on every
  iteration.
- The CRC-32 table is a constant C lookup table. This replaces eight bitwise
  CRC rounds per byte with one table lookup. Double encoding uses `memcpy`,
  which the optimizing compiler lowers to a bit copy without aliasing UB.
- On POSIX systems, checkpoint and strategy operations use `flockfile` once per
  complete operation and explicitly unlock on every exit path, avoiding a
  mutex acquisition for every small stdio call. Non-POSIX builds use no-op
  helpers.

## Ablations and decisions

Keep:

- Trusted adapter operations: the isolated C17/O2 change raised Kuhn CFR by
  about 3.0x, Leduc CFR by about 5.9x, and Leduc MCCFR by about 4.3x.
- Reusable full-tree workspace: compared with the otherwise optimized build,
  this added about 19% on Kuhn CFR and 2.6% on Leduc CFR.
- Constant CRC table: checkpoint throughput rose from about 130.6 MB/s to
  288.5 MB/s before the POSIX stream-lock improvement.
- One POSIX stream lock per operation: after CRC ceased to dominate, this added
  about 6.7% to checkpoint throughput. It retains normal stdio calls and has a
  portable no-op fallback.
- Pure C17 implementation: retain it. Removing C++ costs at most 5.3% in the
  smallest training loop, less than 3% on every Leduc workload, and nothing
  measurable in checkpoint or Kuhn evaluation. In exchange, the project drops
  its C++ compiler/linker dependency while retaining 3.57x--6.06x training
  speedups over the original C implementation.

Discard:

- `putc_unlocked`/`getc_unlocked`: the measured checkpoint result remained at
  roughly 129--130 MB/s before the CRC change, so per-byte calls added
  portability cost without a gain.
- Default `-O3`: it was flat to slightly slower than `-O2` on the training
  workloads.
- Default LTO: it improved Kuhn by roughly 1--2% but was flat or slightly worse
  on Leduc and adds build/link complexity. The more predictable `-O2` build is
  retained.
- Architecture-specific and fast-math flags: they were not adopted because
  they would reduce binary portability or weaken the library's finite-number
  and reproducibility contracts.

The C++20 state remains available at commit `a7aa901` for exact rollback or
future compiler comparisons.

## Reproduction

```sh
make benchmark
build/release/training-benchmark kuhn cfr 500000 9
build/release/training-benchmark kuhn plus 500000 9
build/release/training-benchmark leduc cfr 3000 9
build/release/training-benchmark leduc plus 3000 9
build/release/training-benchmark leduc mccfr 100000 9
build/release/evaluation-benchmark kuhn 100 10000 9
build/release/evaluation-benchmark leduc 10 500 9
build/release/checkpoint-benchmark 100000 11
```

Exact throughput varies with CPU temperature and system load; use the median
speedup from interleaved or adjacent baseline/optimized runs rather than one
sample.

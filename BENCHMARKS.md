# Optimization report

## Method

Measurements were taken on 2026-09-01 on an Apple M4 running Darwin 25.6.0,
using Apple clang 21.0.0. The workloads are single-threaded. Both builds use
`-O2`; the optimized release also defines `NDEBUG`. No native-architecture,
fast-math, PGO, or LTO flags are included.

The baseline is commit `467b24b` built as the original C17 library before any
source changes. Its binaries and static library were preserved and linked to
the same benchmark sources used by the optimized C++20 build. Every process
discarded one warm-up sample. The tables report medians of nine training and
evaluation samples and eleven checkpoint samples. Higher throughput is better.

## Results

### Training

| Workload | Baseline iter/s | Optimized iter/s | Speedup | Gain |
|---|---:|---:|---:|---:|
| Kuhn CFR | 173,621 | 645,234 | 3.72x | +271.6% |
| Kuhn CFR+ | 170,034 | 635,534 | 3.74x | +273.8% |
| Leduc CFR | 1,327.64 | 8,251.28 | 6.22x | +521.5% |
| Leduc CFR+ | 1,322.12 | 8,089.22 | 6.12x | +511.8% |
| Leduc MCCFR | 115,908 | 505,692 | 4.36x | +336.3% |

All compared runs visited the same number of game-tree nodes for their
workload. Seeded MCCFR retained deterministic results.

### Evaluation and checkpoint I/O

| Workload | Baseline | Optimized | Speedup | Gain |
|---|---:|---:|---:|---:|
| Kuhn evaluations/s | 202,540 | 369,194 | 1.82x | +82.3% |
| Leduc evaluations/s | 2,128.12 | 7,913.77 | 3.72x | +271.9% |
| 8 MB checkpoint write | 129.94 MB/s | 302.81 MB/s | 2.33x | +133.0% |

Evaluation produced identical exploitability values before and after the
changes. Checkpoint tests verify byte format, checksums, restore behavior, and
continuous-versus-resumed training.

## What changed

- The implementation now builds as C++20 while preserving the C ABI. Public
  C++ enums have a fixed `int` representation so malformed values supplied by
  C remain safe for validators to reject.
- Kuhn and Leduc now use the existing trusted-operation mechanism already used
  by blackjack. A traversal deeply validates its root, then uses callbacks that
  omit redundant full-history replay. Direct public game operations remain
  checked.
- Full-tree trainers reuse one growable traversal workspace for the whole
  `cfr_trainer_run` call instead of allocating four buffers per player on every
  iteration.
- Internal constants are `constexpr`; hash indexing uses `std::bit_width`, and
  checkpoint double encoding uses `std::bit_cast`.
- The CRC-32 lookup table is generated at compile time. This replaced eight
  bitwise CRC rounds per byte with one table lookup.
- On POSIX systems, checkpoint and strategy operations use `flockfile` once per
  complete operation and release it through a C++ RAII guard, avoiding a mutex
  acquisition for every small stdio call. Non-POSIX builds use a no-op guard.

## Ablations and decisions

Keep:

- Trusted adapter operations: the isolated C17/O2 change raised Kuhn CFR by
  about 3.0x, Leduc CFR by about 5.9x, and Leduc MCCFR by about 4.3x.
- Reusable full-tree workspace: compared with the otherwise optimized build,
  this added about 19% on Kuhn CFR and 2.6% on Leduc CFR.
- Compile-time CRC table: checkpoint throughput rose from about 130.6 MB/s to
  288.5 MB/s before the POSIX stream-lock improvement.
- One POSIX stream lock per operation: after CRC ceased to dominate, this added
  about 6.7% to checkpoint throughput. It retains normal stdio calls and has a
  portable no-op fallback.
- C++20 migration and C ABI tests: runtime-only gains are small, but it enables
  the compile-time CRC table and safer bit conversion without changing the
  consumer language.

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

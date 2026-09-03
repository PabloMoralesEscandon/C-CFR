# CFR, CFR+, and MCCFR in C17

This project implements CFR, CFR+, and external-sampling Monte Carlo CFR for
finite extensive-form games. The first version supports two-player zero-sum
games.

The public API, library implementation, applications, tests, and benchmarks
are all C17. POSIX platforms additionally use guarded fast paths for stream
operations; portable C fallbacks remain available on other systems. Public
headers retain C linkage guards so C++ applications can consume the C library
without changing its ABI.

The repository includes complete Kuhn Poker, Leduc Poker, and blackjack
adapters. Three independent applications train the games, and all adapters are
available through the same public library API.

The Leduc adapter uses the standard six-card deck with two copies each of jack,
queen, and king. Each player antes one chip and receives one private card. A
two-chip fixed-limit betting round is followed by one public card and a
four-chip betting round. Each round permits an opening bet and one raise. A
private card paired with the public card beats an unpaired hand; otherwise the
higher private rank wins. Equal hands split the pot. Suits are strategically
irrelevant, so chance exposes rank outcomes with the exact card-removal
probabilities. The adapter is declared in `include/cfr/leduc_poker.h`.

The blackjack adapter is declared in `include/cfr/blackjack.h` and uses the
fixed rank distribution assumed by basic strategy: ace through nine each have
probability 1/13 and ten-valued cards have probability 4/13 on every draw. The
dealer stands on soft 17, natural blackjack pays 3:2, and the player can hit,
stand, double down, or split. A player can double any two-card hand, including
after a split. Equal rank classes can be split to at most four hands; split
aces receive one card, and a split 21 pays even money. The adapter does not
include insurance or surrender. `CFR_PLAYER_0` is the player and `CFR_PLAYER_1`
receives the dealer's opposite utility; rule-driven dealer draws are represented
as chance nodes.

## Building

Run the commands from the repository root.

```sh
make all
```

The build requires a C17 compiler in `CC` and the Zstandard development
library. It has no C++ build or runtime dependency.

This target creates the following release artifacts:

- `build/release/libcfr.a`
- `build/release/cfr-kuhn`
- `build/release/cfr-leduc`
- `build/release/cfr-blackjack`

The applications are not part of the library. `app/cfr_cli.c`,
`app/leduc_cli.c`, and `app/blackjack_cli.c` consume the public API as external
applications.

To build only the library and the Leduc executable:

```sh
make leduc
```

To build only the library and the blackjack executable:

```sh
make blackjack
```

Use the following target to create the debug configuration:

```sh
make debug
```

This target creates the library, the C test suite, and both debug applications.
The files are placed in `build/debug`.

Use `make clean` to remove the `build` directory.

## Benchmarking

Build the reproducible training, evaluation, and checkpoint microbenchmarks:

```sh
make benchmark
```

Example invocations are:

```sh
build/release/training-benchmark kuhn cfr 500000 9
build/release/training-benchmark leduc mccfr 100000 9
build/release/evaluation-benchmark leduc 10 500 9
build/release/checkpoint-benchmark 100000 11
```

Each benchmark performs one unreported warm-up sample, then emits CSV rows for
the requested number of measured samples. See [BENCHMARKS.md](BENCHMARKS.md)
for the baseline methodology, results, ablations, and keep/discard decisions.

## Testing

Run the C test suite and the application integration test:

```sh
make test
```

The Kuhn and Leduc integration tests check arguments, reports, average
strategies, resumable checkpoints, exact evaluation, and convergence. MCCFR
tests additionally check seeded reproducibility, sampled traversal size,
checkpoint continuation, convergence, and hidden-information isolation. The
blackjack tests check its rules, integration with `Trainer` on a bounded
subtree, and its interface without accidentally starting a full traversal. Run
the Leduc CLI checks separately with:

```sh
make test-leduc-cli
```

Run the blackjack checks separately with:

```sh
make test-blackjack
```

Use `make test-blackjack-cli` to check only the executable's arguments and help
output.

The development tools in `tools/` are not part of `make test`. Check the
decision-tree exporter separately with:

```sh
make test-poker-tree-export
```

Run allocation fault injection:

```sh
make test-alloc
```

This target injects failures into the library's C test suite. It does not
inject failures into `cfr-kuhn`.

The following targets run the tests with sanitizers:

```sh
make test-asan
make test-ubsan
make test-tsan
make test-sanitize
```

`test-asan` uses AddressSanitizer. `test-ubsan` uses
UndefinedBehaviorSanitizer. `test-tsan` uses ThreadSanitizer.
`test-sanitize` runs the address/undefined combination and ThreadSanitizer.

The targets require a compiler that supports the requested options. A compiler
without that support produces a visible failure.

## Using Kuhn Poker

Display help with either of these commands:

```sh
build/release/cfr-kuhn --help
build/release/cfr-kuhn -h
```

The general form is:

```text
cfr-kuhn --iterations N [--report-every N] [--print-strategy]
         [--cfr-plus | --mccfr [--seed N]]
         [--save FILE] [--export-strategy FILE]
cfr-kuhn --load FILE --iterations N [--report-every N] [--print-strategy]
         [--save FILE] [--export-strategy FILE]
cfr-kuhn --load FILE --evaluate [--print-strategy]
         [--export-strategy FILE]
```

| Option | Description |
|---|---|
| `--iterations N` | Runs `N` iterations. `N` must be a positive decimal integer. |
| `--report-every N` | Prints a report after each block of at most `N` iterations. |
| `--print-strategy` | Prints the average strategy after the final report. |
| `--cfr-plus` | Uses CFR+ instead of the default classic CFR. |
| `--mccfr` | Uses external-sampling MCCFR instead of a full-tree traversal. |
| `--seed N` | Selects the MCCFR random stream; the default is zero. |
| `--load FILE` | Loads a binary checkpoint for evaluation or continued training. |
| `--save FILE` | Saves a binary checkpoint after successful training. |
| `--evaluate` | Exactly evaluates a loaded checkpoint without training. |
| `--export-strategy FILE` | Writes the normalized average strategy as readable text. |
| `--help`, `-h` | Prints help without initializing CFR. |

If you omit `--report-every`, the application prints only the final report. Use
an explicit frequency during a long run.

For example, use `--report-every 10000` with 100,000 iterations. The application
will print ten reports. A lower interval increases evaluation overhead.

When `--load` and `--iterations` are used together, `N` is the number of
additional iterations. The checkpoint selects classic CFR, CFR+, or MCCFR; do
not pass a variant or seed option while loading. Reports from resumed training
use the cumulative training iteration count.

## Using Leduc Poker

The Leduc executable has the same training, reporting, checkpoint, evaluation,
and export options as `cfr-kuhn`:

```text
cfr-leduc --iterations N [--report-every N] [--print-strategy]
          [--cfr-plus | --mccfr [--seed N]]
          [--save FILE] [--export-strategy FILE]
cfr-leduc --load FILE --iterations N [--report-every N] [--print-strategy]
          [--save FILE] [--export-strategy FILE]
cfr-leduc --load FILE --evaluate [--print-strategy]
          [--export-strategy FILE]
```

For example, train with CFR+, save everything needed to resume, and export a
readable average policy:

```sh
build/release/cfr-leduc --iterations 100000 --cfr-plus \
    --report-every 10000 --save leduc-100000.cfr \
    --export-strategy leduc-100000.txt
```

The binary `.cfr` file contains regrets, strategy sums, trainer statistics, and
the completed iteration count. The text file contains only normalized action
probabilities and cannot resume training. `--print-strategy` writes a
deterministic, labeled view to standard output after the report. Each label
identifies the acting player, private rank, public rank, round, and visible
betting history; its probabilities are named `check`, `bet`, `fold`, `call`, or
`raise`.

The generic text export retains indexed actions for compatibility with the
library format. At an unopened Leduc information set, `action_0` is check and
`action_1` is bet. When facing a wager, the order is fold, call, raise; after
the cap is reached only fold and call remain.

Evaluate or continue a checkpoint with:

```sh
build/release/cfr-leduc --load leduc-100000.cfr --evaluate
build/release/cfr-leduc --load leduc-100000.cfr --iterations 50000 \
    --save leduc-150000.cfr
```

Leduc has 288 information sets under these rules. A well-converged strategy's
Player 0 value approaches approximately `-0.0856064`; exploitability approaches
zero.

## Saving and loading strategies

A binary checkpoint is the authoritative saved strategy. It contains both the
cumulative strategy sums used for play and evaluation and the cumulative
regrets needed to continue training. It also retains the trainer variant,
completed training iteration count, and trainer statistics. CFR+ therefore
continues with the correct linear averaging weight. An MCCFR checkpoint also
stores its random-stream state, so resumed training is byte-for-byte identical
to uninterrupted training.

Train and save a checkpoint with:

```sh
build/release/cfr-kuhn --iterations 100000 --cfr-plus \
    --save kuhn-100000.cfr
```

Continue that training for another 50,000 iterations with:

```sh
build/release/cfr-kuhn --load kuhn-100000.cfr --iterations 50000 \
    --save kuhn-150000.cfr
```

Saving to the same path that was loaded is supported. The application writes a
temporary sibling file and replaces the destination only after the checkpoint
has been written and closed successfully.

Checkpoints use a versioned, checksummed binary format. Information sets are
sorted by key, so equal training states produce equal files independently of
the hash-table layout. A checkpoint also contains the adapter's stable strategy
schema identifier. Loading rejects a checkpoint if that identifier differs
from the current game descriptor.

The serialization API is game-independent and is declared in
`cfr/checkpoint.h`:

```c
Status cfr_checkpoint_write(FILE *stream, const Trainer *trainer);
Status cfr_checkpoint_read(FILE *stream, const Game *game, GameState *state,
                           InfoStore *store_out, Trainer *trainer_out);
Status cfr_strategy_write_text(FILE *stream, const Trainer *trainer);
```

Every serializable adapter supplies `Game.strategy_schema_id`. The identifier
must change when game rules, information-set keys, or the meaning or order of
action indices becomes incompatible. Serialization stores generic keys and
indexed arrays; it does not contain Kuhn-specific actions or labels.

All bundled adapters are serializable: Kuhn Poker declares
`cfr.kuhn-poker/v1`, Leduc Poker declares `cfr.leduc-poker/v1`, and blackjack
declares `cfr.blackjack/v5`.

### Exact evaluation of a saved strategy

Run the complete-tree evaluator directly on a checkpoint with:

```sh
build/release/cfr-kuhn --load kuhn-150000.cfr --evaluate
```

The command reports both profile values, both best-response values, both
unilateral improvements, NashConv, and exploitability. It enumerates chance and
player branches exactly and does not use random simulation.

For inspection or external tooling, export only the normalized average policy:

```sh
build/release/cfr-kuhn --load kuhn-150000.cfr --evaluate \
    --export-strategy kuhn-150000.txt
```

The text file is deterministic and ordered by information-set key and action
index. It intentionally omits cumulative regrets and cannot be loaded or used
to resume training.

Because the text export cannot be loaded, writing it over a checkpoint destroys
that checkpoint. The application therefore creates strategy exports
exclusively and refuses to replace any existing filesystem object, including a
previous text export. Exclusive creation also covers a symbolic link, a
relative-path alias, a write-only file, and a target created while training is
running. Remove an old text export explicitly or choose a new path before
exporting again.

`--save` is unaffected: replacing the loaded checkpoint with a newer one is how
training resumes.

## Training variants

Without additional options, the application and `cfr_trainer_init` use classic
CFR. To use CFR+ from the application, add `--cfr-plus`:

```sh
build/release/cfr-kuhn --iterations 1000 --cfr-plus
```

When using the library, initialize the trainer with `cfr_trainer_init_plus`.
CFR+ reuses the existing per-player update order and adds its other two rules:

- Regret Matching+ truncates negative cumulative regrets to zero after each
  successful traversal.
- The strategy from iteration `t` contributes to the average strategy with
  weight `t`, so recent strategies have more weight.

The trainer retains the number of training iterations across calls to
`cfr_trainer_run`. `cfr_trainer_reset_stats` resets only the statistics, not
these weights.

To build a custom loop, pass the iteration number explicitly to
`cfr_traverse_plus` and `cfr_traverse_plus_with_stats`. The value starts at one
and must be the same for both players' traversals.

### External-sampling MCCFR

Use `--mccfr` when a full traversal is too expensive. `--seed` accepts any
unsigned 64-bit decimal value and defaults to zero:

```sh
build/release/cfr-kuhn --iterations 100000 --mccfr --seed 42
build/release/cfr-blackjack --deal 5,10,6 --iterations 10000 \
    --mccfr --seed 42
```

The library entry point is `cfr_trainer_init_mccfr`. A lower-level caller can
manage an `MccfrRng` and call `cfr_mccfr_external_traverse` directly; both are
declared in `cfr/mccfr.h`.

MCCFR can train one shared `InfoStore` from multiple threads. Give every worker
its own `Trainer`, mutable root `GameState`, and seed, then call
`cfr_trainer_run_concurrent` in each worker. The normal `cfr_trainer_run`
retains the faster sequential traversal and requires exclusive access to its
store. The game descriptor can be shared when its operations and context are
safe for concurrent calls. Do not share a `Trainer`, `GameState`, or
`MccfrRng`, and do not initialize or destroy the store while a worker is using
it. Pause the workers before writing a resumable checkpoint so that trainer
counters, random streams, and the shared learning state describe one
coordinated boundary.

The shared table uses concurrent reader access and takes an exclusive lock only
when publishing a new information set or growing the table. Learning arrays use
one lock per information set. A traversal retains its first strategy snapshot
for each information set, collects deltas without holding store locks, then
locks only the nodes in its commit in a stable order. This keeps unrelated
traversals independent while preserving coherent decisions, all-or-nothing
commits, and lock-order safety. A per-worker node cache removes shared table
lookups after warm-up.

External sampling draws one chance outcome and one action at every opponent
information set, while expanding every action of the player whose regrets are
being updated. The sampled opponent choice is cached by information-set key
for the complete traversal. If two hidden histories have the same key, they
therefore receive the same sampled action. The traversal also rejects different
ordered action mappings for the same sampled information set. Terminal utility
can inspect the complete state, but policy lookup, sampling, regret storage,
and average-policy storage use only the information-set node.

A traversal updates regrets for the player it targets and the average strategy
for the sampled player. Sampling already supplies the reach that weights a
counterfactual regret, so neither update carries an importance weight. A
complete iteration therefore traverses once per player, which `cfr_trainer_run`
does. A game with a single strategic player has no sampled player to carry the
average, so the traversal weights the target's own strategy by its reach
instead; only chance separates it from the information set, and chance does not
depend on the strategy.

An adapter remains responsible for the game model's information boundary:

- Indistinguishable states for the acting player must return the same stable
  `information_set_key`.
- Every occurrence of an information set must expose the same ordered legal
  actions.
- Information sets must satisfy perfect recall, as required by CFR and MCCFR.

Average-strategy deltas use inverse external-reach weighting, so they are
unbiased estimates of the full CFR strategy sums. Failed traversals preserve
the random stream and do not commit regret or strategy deltas.

Early sampled runs can have information sets that have not been visited yet.
The command-line reports evaluate those missing policies as uniform without
adding them to the training store. Library users can request the same behavior
with `cfr_evaluation_metrics_with_unvisited_uniform`; the original exact
evaluation functions retain their strict missing-key errors.

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Help or execution completed successfully. |
| `1` | An operation, clock query, or write failed. |
| `2` | The arguments are invalid. |

These codes are the values returned by `main`. The operating system can also
terminate the process with a signal. For example, a closed pipe can produce
`SIGPIPE`.

## Reports

Each report contains the following fields:

| Field | Meaning |
|---|---|
| `iterations` | Number of completed iterations. |
| `average_value_player_0` | Player zero's value in the average-strategy profile. |
| `exploitability` | `NashConv` divided by two, following this project's convention. |
| `information_sets` | Number of information sets in the store. |
| `seconds` | Elapsed time since the run started. |

Lower exploitability indicates a profile that is harder to exploit. An
exploitability of zero allows no unilateral improvement.

Time is reported only as execution information. It does not affect learning.

## Short run

This command runs five iterations and prints three reports:

```sh
build/release/cfr-kuhn --iterations 5 --report-every 2
```

One run produced this output:

```text
report iterations=2 average_value_player_0=5.5511151231257827e-17 exploitability=0.27083333333333343 information_sets=12 seconds=0.000056
report iterations=4 average_value_player_0=-0.05598958333333337 exploitability=0.14062500000000003 information_sets=12 seconds=0.000107
report iterations=5 average_value_player_0=-0.048166666666666691 exploitability=0.12138888888888885 information_sets=12 seconds=0.000131
```

The `seconds` field changes between runs.

Add `--print-strategy` to print all twelve decisions. The application keeps a
stable order by context and card.

## Long validation run

Use this command to repeat the long validation run:

```sh
build/release/cfr-kuhn --iterations 100000 --report-every 100000
```

Player zero's value must be within `0.0001` of `-1/18`. Exploitability must be
less than or equal to `0.01`.

The measurement recorded for this documentation produced these values:

```text
report iterations=100000 average_value_player_0=-0.055556357899689546 exploitability=1.7310539843606865e-05 information_sets=12 seconds=0.556525
```

The distance from `-1/18` is approximately `8.02e-07`. Both results satisfy the
documented limits.

CFR approximates an equilibrium. The output does not represent an exact
solution.

## Reproducibility

Classic CFR and CFR+ enumerate chance outcomes and do not use a random seed.
MCCFR uses the explicit seed supplied with `--seed`, or zero when it is omitted.

Two runs with the same arguments produce the same metrics. They also produce
the same strategies in the same order.

The comparison must exclude only the `seconds` field. Timing depends on the
system and can differ between runs.

## Performance reference

This reference is from a specific measurement taken on August 29, 2026.

| Item | Value |
|---|---|
| Compiler | GCC 16.1.1 |
| System | Linux 7.1.3 x86_64 |
| Processor | AMD Ryzen AI 7 350 with Radeon 860M |
| Configuration | Release with `-O2` through `make all` |
| Command | `build/release/cfr-kuhn --iterations 100000 --report-every 100000` |
| Wall-clock time | `0.557319` seconds |
| Maximum resident set size | `1,620` KiB |

A temporary probe measured the child process with `CLOCK_MONOTONIC` and
`wait4`. On Linux, `ru_maxrss` reports the maximum resident set size in KiB.

This measurement is a reference, not a performance guarantee. The result can
change with the compiler, hardware, and system load.

## Inspecting a poker strategy as a decision tree

`tools/` holds development aids that live outside the library and the
applications. One of them draws a trained Kuhn or Leduc Poker strategy as an
interactive decision tree in a browser tab:

```sh
tools/poker-tree-view kuhn
tools/poker-tree-view leduc --load leduc.bin
```

The launcher trains or loads a strategy, walks the complete tree through the
public `cfr_game_*` operations, and opens the graph. Every edge is labelled
with the probability the acting entity gives that action, and every node shows
the expected utility of player zero below it, so the root node repeats the
`average_value_player_0` that training reports.

See [`tools/README.md`](tools/README.md) for the controls, the underlying
`poker-tree-export` program, and the JSON it writes.

## Using blackjack

Display help with:

```sh
build/release/cfr-blackjack --help
```

The general form is:

```text
cfr-blackjack --deal R,R,R --iterations N [--report-every N] [--evaluate]
              [--cfr-plus | --mccfr [--seed N] | --load FILE] [--save FILE]
              [--export-strategy FILE]
cfr-blackjack --full-tree --iterations N [--report-every N] [--evaluate]
              [--cfr-plus | --mccfr [--seed N] | --load FILE] [--save FILE]
              [--export-strategy FILE]
```

| Option | Description |
|---|---|
| `--deal R,R,R` | Solves the hand defined by the three visible initial cards. |
| `--full-tree` | Trains from the state before the initial draw. |
| `--iterations N` | Runs `N` complete training iterations. |
| `--report-every N` | Reports statistics after each block of up to `N` iterations. |
| `--evaluate` | Evaluates the average profile after training and reports its value and exploitability. |
| `--cfr-plus` | Uses CFR+ with Regret Matching+ and linear averaging. |
| `--mccfr` | Uses external-sampling MCCFR. |
| `--seed N` | Selects the MCCFR random stream; the default is zero. |
| `--load FILE` | Loads a binary checkpoint before additional training. |
| `--save FILE` | Saves a resumable binary checkpoint after training. |
| `--export-strategy FILE` | Exports the average strategy as deterministic, human-readable text. |
| `--help`, `-h` | Displays help without initializing CFR. |

Exactly one of `--deal` and `--full-tree` is required. The executable prints and
flushes a `start` line, naming the chosen scope, before the first traversal.

### Solving one initial deal

`--deal` takes the three visible cards in dealing order: player, dealer up card,
player. Each rank is an integer from 1 to 10, where 1 is an ace and 10 covers
every ten, jack, queen, and king. The hidden dealer hole card is deliberately
not supplied: it remains the first chance event below the traversal root, so
training includes every state in the player's information set.

The strategy can choose hit, stand, and double down on an eligible two-card
hand. When the two player ranks are equal, it can also split. Ten-valued cards
share one rank class in this adapter. Non-ace pairs can be resplit to the
four-hand limit; split aces receive exactly one additional card.

```sh
build/release/cfr-blackjack --deal 5,10,6 --iterations 10 \
    --report-every 5 --evaluate
```

That command completes in a few seconds on a typical development machine and
reports the player's average value and its exploitability for a hard eleven
against a dealer ten. This is the supported blackjack workflow.

### Saving and resuming a strategy

To retain both the resumable data and a readable snapshot, use distinct paths:

```sh
build/release/cfr-blackjack --deal 5,10,6 --iterations 10 \
    --save blackjack.cfr --export-strategy blackjack-strategy.txt
```

The checkpoint stores the trainer and strategy data, but not the traversal root.
When loading it, select the same `--deal` or `--full-tree` scope that created it:

```sh
build/release/cfr-blackjack --deal 5,10,6 --load blackjack.cfr \
    --iterations 10 --save blackjack.cfr
```

Checkpoint and text exports are written through temporary sibling files and
atomically replace their destinations only after successful training and
serialization. A loaded checkpoint selects classic CFR, CFR+, or MCCFR
automatically, so `--load` cannot be combined with a variant or seed option.

### Complete-tree cost

Each iteration runs one traversal for the only strategic player and enumerates
the tree below the root. The dealer follows a deterministic policy and its draws
are chance nodes, so it does not receive a second traversal.

Every chance node has the same ten rank-class outcomes. No deck-composition
state is retained. Strategy keys merge hands with the same dealer up card, hard
or soft total, and available action class.

Split hands are retained and played individually. A resplit increases the hand
count by one, so the adapter represents the exact `1 -> 2 -> 3 -> 4` sequence
and shares the four-hand cap across siblings. A resplit pair still reuses the
same information key as the original pair while another split is legal.

The generic `--evaluate` path materializes the reachable tree in memory and is
therefore more demanding than training. The executable still requires an
explicit choice between `--deal` and `--full-tree` so the intended scope is
unambiguous.

For compact analysis of a complete tree or bounded deal, build the
blackjack-specific helper:

```sh
make blackjack-compact-eval
build/release/blackjack-compact-eval CHECKPOINT
```

The helper merges future-equivalent hand states into a DAG and supports hit,
stand, double, and split decisions. It reports the learned policy value, the
value of the matching infinite-deck S17, double-after-split, no-surrender basic
strategy, and a deterministic policy-improvement candidate. Because a resplit
can encounter the same abstract information set more than once along a
trajectory, the candidate improvement is a convergence proxy rather than a
certified perfect-recall exploitability value.

The helper can create a fresh CFR+ checkpoint as well as resume an existing CFR
or CFR+ checkpoint:

```sh
build/release/blackjack-compact-eval --new-cfr-plus 150 OUTPUT.cfr 1 6 1
build/release/blackjack-compact-eval INPUT.cfr 150 OUTPUT.cfr 1 6 1
```

Omit the three ranks to use the root before the initial draw. `OUTPUT.cfr` must
not already exist. Raw visited-node statistics saturate at `SIZE_MAX`; the exact
split tree exceeds that count in one full traversal. Compact updates can differ
from a raw checkpoint at the last few floating-point bits because equivalent
contributions are summed in a different order.

In an August 2026 release-build measurement, the full compact graph contained
16,220,814 states and 114,470,680 edges. One CFR+ iteration completed in 28.2
seconds with an 8.4 GB peak resident set; ten iterations completed in 45.5
seconds with a 9.3 GB peak. These are example measurements, not performance
guarantees.

Each training report contains:

| Field | Meaning |
|---|---|
| `iterations` | Complete iterations. |
| `traversals` | Accumulated complete CFR traversals. |
| `visited_nodes` | Accumulated states visited. |
| `information_sets` | Distinct stored decisions. |
| `seconds` | Time since process start. |

With `--evaluate`, the final line contains the player's average value, the
dealer's opposite value, exploitability, and total time.

## License

Copyright 2026 Pablo Morales Escandón.

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the
complete terms. This license applies to all copyrightable code and documentation
in this repository, including historical revisions that predate the addition of
the license, unless otherwise stated.

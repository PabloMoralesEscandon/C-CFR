# CFR and CFR+ in C17

This project implements a CFR and CFR+ library for finite extensive-form games.
The first version supports two-player zero-sum games.

The repository includes complete Kuhn Poker and blackjack adapters. Two
independent applications train the games, and both adapters are available
through the same public library API.

The blackjack adapter is declared in `include/cfr/blackjack.h` and uses a
52-card deck without replacement, dealer stands on soft 17, a 3:2 natural
blackjack payout, and hit or stand actions. It does not include doubling,
splitting, insurance, or surrender. `CFR_PLAYER_0` is the player and
`CFR_PLAYER_1` receives the dealer's opposite utility; rule-driven dealer draws
are represented as chance nodes.

## Building

Run the commands from the repository root.

```sh
make all
```

This target creates the following release artifacts:

- `build/release/libcfr.a`
- `build/release/cfr-kuhn`
- `build/release/cfr-blackjack`

The applications are not part of the library. `app/cfr_cli.c` and
`app/blackjack_cli.c` consume the public API as external applications.

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

## Testing

Run the C test suite and the application integration test:

```sh
make test
```

The Kuhn integration test checks arguments, reports, the average strategy,
reproducibility, and convergence. The blackjack tests check its rules,
integration with `Trainer` on a bounded subtree, and its interface without
accidentally starting a full traversal. Run them separately with:

```sh
make test-blackjack
```

Use `make test-blackjack-cli` to check only the executable's arguments and help
output.

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
make test-sanitize
```

`test-asan` uses AddressSanitizer. `test-ubsan` uses
UndefinedBehaviorSanitizer. `test-sanitize` combines both sanitizers.

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
cfr-kuhn --iterations N [--report-every N] [--print-strategy] [--cfr-plus]
```

| Option | Description |
|---|---|
| `--iterations N` | Runs `N` iterations. `N` must be a positive decimal integer. |
| `--report-every N` | Prints a report after each block of at most `N` iterations. |
| `--print-strategy` | Prints the average strategy after the final report. |
| `--cfr-plus` | Uses CFR+ instead of the default classic CFR. |
| `--help`, `-h` | Prints help without initializing CFR. |

If you omit `--report-every`, the application prints only the final report. Use
an explicit frequency during a long run.

For example, use `--report-every 10000` with 100,000 iterations. The application
will print ten reports. A lower interval increases evaluation overhead.

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

The trainer enumerates chance outcomes, so the program does not use a random
seed.

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

## Using blackjack

Display help with:

```sh
build/release/cfr-blackjack --help
```

The general form is:

```text
cfr-blackjack --iterations N [--report-every N] [--evaluate] [--cfr-plus]
```

| Option | Description |
|---|---|
| `--iterations N` | Runs `N` complete training iterations. |
| `--report-every N` | Reports statistics after each block of up to `N` iterations. |
| `--evaluate` | Evaluates the average profile after training and reports its value and exploitability. |
| `--cfr-plus` | Uses CFR+ with Regret Matching+ and linear averaging. |
| `--help`, `-h` | Displays help without initializing CFR. |

Always start with one iteration and no evaluation:

```sh
build/release/cfr-blackjack --iterations 1 --report-every 1
```

Each iteration runs one traversal for the only strategic player and enumerates
the complete tree. The dealer follows a deterministic policy and its draws are
chance nodes, so it does not receive a second traversal. A full-deck blackjack
tree is much larger than Kuhn's tree. The `--evaluate` option performs another
complete enumeration and can require considerably more time and memory. The
executable prints and flushes a `start` line before the first traversal, giving
immediate confirmation of the configuration and selected variant during a long
run.

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

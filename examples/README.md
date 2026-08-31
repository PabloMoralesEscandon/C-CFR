# Examples

The repository includes complete Kuhn Poker, Leduc Poker, and blackjack
adapters. The applications in `app/cfr_cli.c` and `app/leduc_cli.c` connect the
two poker games to the trainer and evaluator. `app/blackjack_cli.c` does the
same for blackjack and skips exhaustive evaluation unless explicitly requested
with `--evaluate`.

Each application consumes the public API through the `cfr_game_*` functions.
It also retains ownership of its game state.

The applications are not part of the public library. See the main
[`README.md`](../README.md) to build and use the executables.

The example uses classic CFR by default. Pass `--cfr-plus` to use Regret
Matching+ and linear strategy averaging.

The application can save and resume the complete generic trainer state with
`--save FILE` and `--load FILE`. Use `--load FILE --evaluate` to run the exact
full-tree evaluator on a checkpoint, or `--export-strategy FILE` to write a
readable, export-only average policy. See the main README for compatibility
rules and complete commands.

# Examples

The repository includes complete Kuhn Poker and blackjack adapters. The
application in `app/cfr_cli.c` connects Kuhn Poker to the trainer and evaluator.
`app/blackjack_cli.c` does the same for blackjack and skips exhaustive
evaluation unless explicitly requested with `--evaluate`.

The application consumes the public API through the `cfr_game_*` functions. It
also retains ownership of the game state.

The application is not part of the public library. See the main
[`README.md`](../README.md) to build and use the executable.

The example uses classic CFR by default. Pass `--cfr-plus` to use Regret
Matching+ and linear strategy averaging.

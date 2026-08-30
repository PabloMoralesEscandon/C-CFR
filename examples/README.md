# Examples

The repository includes a complete Kuhn Poker adapter. The application in
`app/cfr_cli.c` connects the adapter to the trainer and evaluator.

The application consumes the public API through the `cfr_game_*` functions. It
also retains ownership of the game state.

The application is not part of the public library. See the main
[`README.md`](../README.md) to build and use the executable.

The example uses classic CFR by default. Pass `--cfr-plus` to use Regret
Matching+ and linear strategy averaging.

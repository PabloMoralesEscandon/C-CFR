# Examples

The repository includes a complete Kuhn Poker adapter. The application in
`app/cfr_cli.c` connects the adapter to the trainer and evaluator.

The application consumes the public API through the `cfr_game_*` functions. It
also retains ownership of the game state.

The application is not part of the public library. See the main
[`README.md`](../README.md) to build and use the executable.

The example uses classic CFR by default. Pass `--cfr-plus` to use Regret
Matching+ and linear strategy averaging.

The application can save and resume the complete generic trainer state with
`--save FILE` and `--load FILE`. Use `--load FILE --evaluate` to run the exact
full-tree evaluator on a checkpoint, or `--export-strategy FILE` to write a
readable, export-only average policy. See the main README for compatibility
rules and complete commands.

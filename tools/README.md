# Tools

Programs in this directory sit outside the library and the applications. They
are development aids: they consume the public API but nothing in `src/`,
`include/`, or `app/` depends on them, and nothing here is covered by the
library's compatibility promises.

- `poker_tree_export.c` and `poker_tree_view.html` — inspect a trained Kuhn or
  Leduc Poker strategy as an interactive decision tree in a browser.
- `blackjack_compact_eval.c` — evaluate a blackjack checkpoint over a compact
  state graph. See the main [`README.md`](../README.md) for its options.

## Poker decision-tree viewer

The viewer answers one question: *given a strategy the trainer computed, what
does the game tree actually look like, and how likely is every branch?* It
walks the whole tree through the public `cfr_game_*` operations and draws it as
a graph, so it also serves as an independent check on the trained policy.

### One command

```sh
tools/poker-tree-view kuhn
tools/poker-tree-view leduc
```

The launcher builds what it needs, trains a default run (200,000 iterations for
Kuhn Poker, 20,000 for Leduc Poker), walks the tree, embeds the result in a
copy of the viewer page, and opens a browser tab. Nothing is written inside the
repository.

To inspect a strategy you already have, pass it instead of training:

```sh
tools/poker-tree-view leduc --load leduc.bin
tools/poker-tree-view leduc --strategy leduc-strategy.txt
tools/poker-tree-view kuhn --iterations 500000 --cfr-plus
tools/poker-tree-view kuhn --output kuhn-tree.html --no-open
```

`--load` reads a binary checkpoint written by `--save`. `--strategy` reads the
text file written by `--export-strategy`. Both describe the same average
strategy, and the viewer reports which one it used.

### Reading the graph

Each box is one game state. Grey boxes are chance nodes, blue are player zero,
amber are player one, and terminal boxes shade from red to green with the
payoff to player zero. The number inside every box is the expected utility of
player zero below that state under the average strategy, so the root box holds
the value of the whole profile. Under each player box, a thin stacked bar shows
the strategy at a glance.

Each edge is one action. Its label gives the probability the acting entity
assigns to it — a chance probability at a deal, an average-strategy probability
at a player node — and its thickness repeats that probability. A dashed edge
carries probability zero.

Selecting a player node outlines every other node in the same information set,
which shows exactly what that player cannot distinguish.

| Control | Effect |
|---|---|
| Click a node | Select it and fold or unfold its subtree. |
| Double-click a node | Redraw the tree from that node. The breadcrumb walks back up. |
| Drag, scroll | Pan and zoom. |
| `depth` | Unfold that many levels below the focused node. |
| `min reach` | Hide branches the profile reaches less often than the threshold. |
| `hide 0%` | Hide actions the strategy never takes. |
| `find infoset` | Search node and information-set labels; `‹` and `›` step through matches. |
| `Fit` | Frame the visible tree. |

The full Leduc Poker tree has 1,936 nodes over 288 information sets, so it
opens folded to two levels. Double-clicking a deal is usually the fastest way
in.

### The exporter alone

The launcher is a convenience. The exporter is a normal program that writes
JSON to standard output or to `--output`:

```sh
make poker-tree-export
build/release/poker-tree-export --game leduc --load leduc.bin --output tree.json
```

Without `--load` or `--strategy` it walks the tree with a uniform strategy,
which shows the shape of a game before any training. The page also accepts a
JSON file dropped onto it, so an exported tree can be opened later without
re-running the launcher.

Each node carries its kind, label, reach probability, expected utility, and the
game-specific detail behind it: phase, private cards, board card, pot,
contributions, and visible history. Player nodes add the information-set key
and label, and whether the loaded strategy covered that set. The document ends
with counts and a `complete` flag that is false when any information set fell
back to a uniform strategy.

### Test

```sh
make test-poker-tree-export
```

The test checks the exact node counts implied by the rules of both games and
confirms that the root expected utility written by the exporter matches the
profile value that the library's own evaluator reports for the same checkpoint.

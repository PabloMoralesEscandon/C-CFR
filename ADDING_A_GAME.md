# Adding a new game

The library does not know any game rules. The trainer, MCCFR, the evaluator,
and checkpoints work with any game that implements the adapter interface in
`include/cfr/game.h`. This guide explains that interface, walks through a
small complete adapter, and lists the steps to integrate a game into the
repository.

The bundled adapters are the reference implementations:

| Adapter | Useful as a model for |
|---|---|
| `src/kuhn_poker.c` | The smallest real adapter: phases, undo snapshots, trusted operations. |
| `src/leduc_poker.c` | Several betting rounds, a public chance card, and card-removal probabilities. |
| `src/blackjack.c` | One strategic player, a rule-driven opponent modelled as chance, and a configurable root. |

## Requirements for the game

Before writing code, check that the game fits the model:

- **Two players, zero sum.** Utilities are read for `CFR_PLAYER_0` and
  `CFR_PLAYER_1`, and the utility of player 1 must be the negative of the
  utility of player 0.
- **Finite.** Every path from the root reaches a terminal state. Classic CFR
  and CFR+ traverse the complete tree in each iteration, and the exact
  evaluator materializes it. For a large tree, use MCCFR and restrict exact
  evaluation to a subtree.
- **Randomness as chance nodes.** Every random event is a state whose actor is
  `CFR_ACTOR_CHANCE`, with explicit outcomes and probabilities. The adapter
  never draws random numbers itself.
- **Perfect recall.** A player never forgets information that they observed
  or actions that they took.
- **One or two strategic players.** `strategic_player_count` is 2 for a normal
  game. It is 1 when the second participant has no choices and only receives
  the opposite utility, as the blackjack dealer does. In that case, all the
  decisions of the second participant must be chance nodes.

## The three parts of an adapter

An adapter supplies three things:

1. **A concrete state type.** The library sees only the opaque type
   `GameState`. The caller allocates the concrete state (usually on the stack),
   and the adapter casts the pointer back to its own type. The state must
   support `apply_action` and `undo_action`, because traversals walk the tree
   in place instead of copying states.
2. **A `GameOperations` table** with one callback per query or transition.
3. **A `Game` descriptor** that connects the table with the constants of the
   game:

   | Field | Meaning |
   |---|---|
   | `operations` | Required callback table. |
   | `context` | Optional pointer to constant configuration, passed to each callback. It can be `NULL`. |
   | `strategic_player_count` | `1` or `2` (see above). |
   | `max_legal_actions` | Upper bound on the legal actions in **any** state, chance nodes included. |
   | `strategy_schema_id` | Stable, printable ASCII identifier for checkpoints, for example `"cfr.my-game/v1"`. |
   | `trusted_operations` | Optional faster table for states that are already validated. It can be `NULL`. |

## The callback contract

| Callback | Called for | Must do |
|---|---|---|
| `is_terminal` | Any valid state | Write `true` or `false` to `result`. |
| `terminal_utility` | Terminal states | Write the payoff of `player`. A nonterminal state gives `CFR_STATUS_INVALID_ARGUMENT`. |
| `current_actor` | Nonterminal states | Write `CFR_ACTOR_CHANCE`, or `CFR_ACTOR_PLAYER` with `player`. |
| `legal_actions` | Nonterminal states | Write the count to `required_count`. If `capacity` is too small, return `CFR_STATUS_BUFFER_TOO_SMALL` and do not write `actions`. |
| `apply_action` | Nonterminal states | Apply a legal action. An illegal action gives `CFR_STATUS_ILLEGAL_ACTION`. |
| `undo_action` | States with history | Restore the state from before the last `apply_action`. |
| `chance_probability` | Chance states | Write the probability of one legal outcome. The probabilities of all outcomes add up to 1. |
| `information_set_key` | Player states | Write the key of the information set of the acting player. |
| `validate_state` | Any state (optional) | Do a complete semantic check. Must not modify the state. |
| `chance_outcomes` | Chance states (optional) | Write all outcomes and their probabilities in one call. |

Rules that apply to every callback:

- **No allocation.** The caller provides all storage. Keep the state a
  fixed-size structure; for example, bound its history arrays with the maximum
  game length.
- **Errors change nothing.** A callback that returns an error must not modify
  the state or its outputs. The only exception is `required_count` together
  with `CFR_STATUS_BUFFER_TOO_SMALL`.
- **Do not retain pointers.** A callback borrows the state and its outputs only
  for the duration of the call.
- **Undo is exact.** `apply_action` followed by `undo_action` must restore the
  state completely. Traversals depend on this.
- **Reject invalid states.** Callbacks in `operations` must return
  `CFR_STATUS_INVALID_ARGUMENT` for a state that is inconsistent, rather than
  reading out of bounds.

### Actions

`Action` is an `int` whose values the adapter chooses. The trainer does not
store action values. It stores one regret and one strategy entry for each
**position** in the list that `legal_actions` returns. Therefore:

- Every state of one information set must return the same actions in the
  same order.
- The order is part of the checkpoint format. If you change it, change
  `strategy_schema_id`.

### Information-set keys

`InfoSetKey` is an `int64_t`. All players share one store, so keys must satisfy
these conditions:

- States that the acting player cannot distinguish return **the same** key.
  Build the key only from information that the player can see: their private
  data and the public history. Never use the private data of the opponent.
- States that the player **can** distinguish return different keys.
- The information sets of player 0 and player 1 never share a key. Encode the
  player in the key.
- The key of an information set is the same in every run. Checkpoints and text
  exports are sorted by key.

A mixed-radix encoding is the usual solution. Kuhn Poker, for example, uses
`((player * 4) + context) * 3 + card`.

## Complete example: High Card

The rules of this game are simple:

1. Chance deals one card to player 0: low or high, each with probability 1/2.
2. Player 0 sees the card and checks or bets.
3. After a check, the higher card wins 1 chip: player 0 gets +1 with high and
   −1 with low.
4. After a bet, player 1, who does not see the card, folds or calls. A fold
   gives player 0 +1. A call gives player 0 +2 with high and −2 with low.

The equilibrium is known: player 0 always bets with high and bets 1/3 of the
time with low; player 1 calls 2/3 of the time. The value of the game for
player 0 is 1/3.

### State

The action history is the complete state, so undo only removes the last
action. Games with more state (chips, board cards, phases) usually store a
snapshot per transition, as `KuhnPokerUndoEntry` does.

```c
#include <stdbool.h>
#include <stddef.h>

#include "cfr/game.h"

enum {
    HIGH_CARD_DEAL_LOW,
    HIGH_CARD_DEAL_HIGH,
    HIGH_CARD_CHECK,
    HIGH_CARD_BET,
    HIGH_CARD_FOLD,
    HIGH_CARD_CALL
};

#define HIGH_CARD_MAX_HISTORY 3
#define HIGH_CARD_MAX_ACTIONS 2

/* The complete state is the action history: deal, player 0, player 1. */
typedef struct {
    Action history[HIGH_CARD_MAX_HISTORY];
    size_t count;
} HighCardState;

static const HighCardState *as_high_card(const GameState *state) {
    return (const HighCardState *)state;
}

static Status validate(const HighCardState *state) {
    if (state == NULL || state->count > HIGH_CARD_MAX_HISTORY)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (state->count >= 1 && state->history[0] != HIGH_CARD_DEAL_LOW &&
        state->history[0] != HIGH_CARD_DEAL_HIGH)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (state->count >= 2 && state->history[1] != HIGH_CARD_CHECK &&
        state->history[1] != HIGH_CARD_BET)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (state->count == 3 &&
        (state->history[1] != HIGH_CARD_BET ||
         (state->history[2] != HIGH_CARD_FOLD &&
          state->history[2] != HIGH_CARD_CALL)))
        return CFR_STATUS_INVALID_ARGUMENT;
    return CFR_STATUS_SUCCESS;
}

static bool terminal(const HighCardState *state) {
    return state->count == 3 ||
           (state->count == 2 && state->history[1] == HIGH_CARD_CHECK);
}

/* Writes the legal actions of a valid nonterminal state in a stable order. */
static size_t list_actions(const HighCardState *state, Action *out) {
    switch (state->count) {
    case 0:
        out[0] = HIGH_CARD_DEAL_LOW;
        out[1] = HIGH_CARD_DEAL_HIGH;
        return 2;
    case 1:
        out[0] = HIGH_CARD_CHECK;
        out[1] = HIGH_CARD_BET;
        return 2;
    default:
        out[0] = HIGH_CARD_FOLD;
        out[1] = HIGH_CARD_CALL;
        return 2;
    }
}
```

### Callbacks

Each callback validates the state first, then answers the query.

```c
static Status high_card_validate_state(const void *context,
                                       const GameState *state) {
    (void)context;
    return validate(as_high_card(state));
}

static Status high_card_is_terminal(const void *context,
                                    const GameState *state, bool *result) {
    (void)context;
    const HighCardState *s = as_high_card(state);
    Status status = validate(s);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *result = terminal(s);
    return CFR_STATUS_SUCCESS;
}

static Status high_card_terminal_utility(const void *context,
                                         const GameState *state, Player player,
                                         Utility *result) {
    (void)context;
    const HighCardState *s = as_high_card(state);
    Status status = validate(s);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (!terminal(s) || (player != CFR_PLAYER_0 && player != CFR_PLAYER_1))
        return CFR_STATUS_INVALID_ARGUMENT;

    const Utility sign = s->history[0] == HIGH_CARD_DEAL_HIGH ? 1.0 : -1.0;
    Utility player_0;
    if (s->history[1] == HIGH_CARD_CHECK)
        player_0 = sign;
    else if (s->history[2] == HIGH_CARD_FOLD)
        player_0 = 1.0;
    else
        player_0 = 2.0 * sign;

    *result = player == CFR_PLAYER_0 ? player_0 : -player_0;
    return CFR_STATUS_SUCCESS;
}

static Status high_card_current_actor(const void *context,
                                      const GameState *state, Actor *result) {
    (void)context;
    const HighCardState *s = as_high_card(state);
    Status status = validate(s);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (terminal(s))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (s->count == 0) {
        result->kind = CFR_ACTOR_CHANCE;
    } else {
        result->kind = CFR_ACTOR_PLAYER;
        result->player = s->count == 1 ? CFR_PLAYER_0 : CFR_PLAYER_1;
    }
    return CFR_STATUS_SUCCESS;
}

static Status high_card_legal_actions(const void *context,
                                      const GameState *state, Action *actions,
                                      size_t capacity,
                                      size_t *required_count) {
    (void)context;
    const HighCardState *s = as_high_card(state);
    Action legal[HIGH_CARD_MAX_ACTIONS];
    Status status = validate(s);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (terminal(s))
        return CFR_STATUS_INVALID_ARGUMENT;

    const size_t count = list_actions(s, legal);
    *required_count = count;
    if (capacity < count)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    for (size_t i = 0; i < count; i++)
        actions[i] = legal[i];
    return CFR_STATUS_SUCCESS;
}

static Status high_card_apply_action(const void *context, GameState *state,
                                     Action action) {
    (void)context;
    HighCardState *s = (HighCardState *)state;
    Action legal[HIGH_CARD_MAX_ACTIONS];
    Status status = validate(s);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (terminal(s))
        return CFR_STATUS_ILLEGAL_ACTION;

    const size_t count = list_actions(s, legal);
    for (size_t i = 0; i < count; i++) {
        if (legal[i] == action) {
            s->history[s->count] = action;
            s->count += 1;
            return CFR_STATUS_SUCCESS;
        }
    }
    return CFR_STATUS_ILLEGAL_ACTION;
}

static Status high_card_undo_action(const void *context, GameState *state) {
    (void)context;
    HighCardState *s = (HighCardState *)state;
    Status status = validate(s);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (s->count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    s->count -= 1;
    return CFR_STATUS_SUCCESS;
}

static Status high_card_chance_probability(const void *context,
                                           const GameState *state,
                                           Action action,
                                           Probability *result) {
    (void)context;
    const HighCardState *s = as_high_card(state);
    Status status = validate(s);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (s->count != 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action != HIGH_CARD_DEAL_LOW && action != HIGH_CARD_DEAL_HIGH)
        return CFR_STATUS_ILLEGAL_ACTION;
    *result = 0.5;
    return CFR_STATUS_SUCCESS;
}

static Status high_card_information_set_key(const void *context,
                                            const GameState *state,
                                            InfoSetKey *result) {
    (void)context;
    const HighCardState *s = as_high_card(state);
    Status status = validate(s);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (s->count == 1) {
        /* Player 0 sees the card: keys 0 (low) and 1 (high). */
        *result = s->history[0] == HIGH_CARD_DEAL_HIGH ? 1 : 0;
    } else if (s->count == 2 && s->history[1] == HIGH_CARD_BET) {
        /* Player 1 sees only the bet: one key shared by both deals. */
        *result = 2;
    } else {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    return CFR_STATUS_SUCCESS;
}
```

### Descriptor

```c
static const GameOperations HIGH_CARD_OPERATIONS = {
    .is_terminal = high_card_is_terminal,
    .terminal_utility = high_card_terminal_utility,
    .current_actor = high_card_current_actor,
    .legal_actions = high_card_legal_actions,
    .apply_action = high_card_apply_action,
    .undo_action = high_card_undo_action,
    .chance_probability = high_card_chance_probability,
    .information_set_key = high_card_information_set_key,
    .validate_state = high_card_validate_state};

static const Game HIGH_CARD_GAME = {
    .operations = &HIGH_CARD_OPERATIONS,
    .context = NULL,
    .strategic_player_count = 2,
    .max_legal_actions = HIGH_CARD_MAX_ACTIONS,
    .strategy_schema_id = "example.high-card/v1",
    .trusted_operations = NULL};
```

Use designated initializers. The optional fields at the end of both structures
are then `NULL` unless you set them.

### Training, evaluation, and export

```c
#include <stdio.h>

#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/info_store.h"
#include "cfr/trainer.h"

int main(void) {
    HighCardState state = {.count = 0};
    GameState *root = (GameState *)&state;
    InfoStore store;
    Trainer trainer;
    EvaluationMetrics metrics;

    Status status = cfr_info_store_init(&store);
    if (status != CFR_STATUS_SUCCESS)
        return 1;

    status = cfr_trainer_init_plus(&trainer, &HIGH_CARD_GAME, root, &store);
    if (status == CFR_STATUS_SUCCESS)
        status = cfr_trainer_run(&trainer, 10000);
    if (status == CFR_STATUS_SUCCESS)
        status = cfr_evaluation_metrics(&HIGH_CARD_GAME, root, &store,
                                        &metrics);
    if (status == CFR_STATUS_SUCCESS) {
        printf("value_player_0=%.6f exploitability=%.3g\n",
               metrics.profile_value_player_0, metrics.exploitability);
        status = cfr_strategy_write_text(stdout, &trainer);
    }

    if (cfr_info_store_destroy(&store) != CFR_STATUS_SUCCESS)
        status = CFR_STATUS_INVALID_ARGUMENT;
    if (status != CFR_STATUS_SUCCESS) {
        fprintf(stderr, "error: status %d\n", (int)status);
        return 1;
    }
    return 0;
}
```

Put all three code blocks in one file and build it against the library:

```sh
make build/release/libcfr.a
cc -std=c17 -O2 -Iinclude high_card.c build/release/libcfr.a -lzstd -pthread \
    -o high_card
./high_card
```

The output shows the expected equilibrium. Action 0 is check at keys 0 and 1
and fold at key 2:

```text
value_player_0=0.333333 exploitability=4.21e-05
cfr-strategy version=1 schema=example.high-card/v1 variant=cfr-plus training_iterations=10000 information_sets=3
infoset key=0 actions=2 action_0=0.66659471780987589 action_1=0.33340528219012405
infoset key=1 actions=2 action_0=9.9990000999900001e-09 action_1=0.99999999000099982
infoset key=2 actions=2 action_0=0.33323670026110241 action_1=0.66676329973889759
```

To use a different algorithm, replace `cfr_trainer_init_plus` with
`cfr_trainer_init` (classic CFR) or with `cfr_trainer_init_mccfr(&trainer,
&HIGH_CARD_GAME, root, &store, seed)`. With MCCFR, some information sets can
be missing early in training, so evaluate with
`cfr_evaluation_metrics_with_unvisited_uniform`. To save and resume training,
use `cfr_checkpoint_write` and `cfr_checkpoint_read` from `cfr/checkpoint.h`.
`app/cfr_cli.c` shows the complete flow, including atomic checkpoint
replacement.

The root state does not have to be the start of the game. Any valid state can
be the root: blackjack's `--deal` option trains only the subtree after three
known cards.

## Optional extensions

Add these after the basic adapter works and is tested.

- **`validate_state`.** The engine calls it at operation boundaries, for
  example on the root before a traversal. Implement it whenever the state can
  become inconsistent.
- **`chance_outcomes`.** This callback returns all outcomes and probabilities
  in one call, instead of `legal_actions` followed by one
  `chance_probability` call per outcome. It is useful for chance nodes with
  many outcomes.
- **`trusted_operations`.** A second table whose callbacks omit whole-state
  validation. Traversals and evaluation use it only after the root passes
  validation. It must still keep every other guarantee of the contract. Kuhn
  Poker implements each public callback as "validate, then call the trusted
  version", so both tables share one implementation. This is usually the
  largest speed improvement for an adapter whose `validate` replays the
  history.
- **`context`.** Use it for rule variants (stack sizes, deck composition, bet
  limits) instead of global variables. The context is constant and borrowed,
  and it must stay alive while the descriptor exists. Validate it in each
  callback. If a variant changes the keys or the action order, it needs its
  own `strategy_schema_id`.
- **Concurrent MCCFR.** `cfr_trainer_run_concurrent` can share one `Game`
  between threads. That is safe only if the callbacks and the context do not
  modify shared data. Each thread needs its own state.
- **Warm start.** `cfr_info_store_set_initializer` lets you seed the regrets
  or strategy sums of each new information set, for example from a heuristic.

## Integrating the game into the repository

Follow the structure of the existing adapters. For a game named `my_game`:

1. **Public header:** `include/cfr/my_game.h`. Declare the state type, the
   action enumeration with a comment that documents the order, the capacity
   constants, `cfr_my_game_state_init`, `cfr_my_game_descriptor`, and the
   `cfr_my_game_state_as_game_state[_const]` casts. Wrap the declarations in
   `CFR_EXTERN_C_BEGIN` and `CFR_EXTERN_C_END`, and use `CFR_ENUM_INT` for
   enumerations so the header also works in C++.
2. **Implementation:** `src/my_game.c`. Make the callbacks and the operation
   tables `static`, and return the descriptor through
   `cfr_my_game_descriptor()`.
3. **Makefile:** add `src/my_game.c` to `LIB_SOURCES`.
4. **Tests:** create `tests/test_my_game.c` with an `int test_my_game(void)`
   function that returns the number of failures. Declare it in
   `tests/test_suite.h`, call it from `tests/test_main.c`, and add the file
   to `TEST_SOURCES`. Use `tests/test_kuhn_poker.c` as a template. Test at
   least:
   - the legal actions and their order in each kind of state;
   - illegal actions and invalid states, and that errors do not change the
     state;
   - apply and undo round trips through the complete tree;
   - that the chance probabilities add up to 1;
   - terminal utilities, and that they are zero sum;
   - equal keys for indistinguishable states and different keys otherwise;
   - convergence with a known value or low exploitability, if the tree is
     small enough.
5. **Header test:** include the new header in `tests/test_public_headers.c`.
6. **Application (optional):** add `app/my_game_cli.c` and its Makefile
   targets, based on `app/leduc_cli.c`, if the game needs an executable.
7. **Documentation:** describe the rules and the schema identifier in
   `README.md`, next to the other adapters.

Then run the complete checks:

```sh
make test
make test-sanitize
```

## Checklist

- [ ] Player 1's utility is the negative of player 0's utility in every
      terminal state.
- [ ] `max_legal_actions` includes chance nodes.
- [ ] The chance probabilities of each node add up to 1.
- [ ] Keys use only information visible to the acting player and include the
      player.
- [ ] Every state of an information set returns the same actions in the same
      order.
- [ ] `apply_action` followed by `undo_action` restores the state exactly.
- [ ] Callbacks that fail do not modify the state or the outputs.
- [ ] Callbacks do not allocate memory and do not keep pointers.
- [ ] `strategy_schema_id` is unique and changes with any incompatible change
      to the rules, keys, or action order.

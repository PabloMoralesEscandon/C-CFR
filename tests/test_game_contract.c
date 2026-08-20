#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cfr/game.h"
#include "support/fake_game.h"
#include "test_suite.h"

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: no se cumple: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            failures += 1;                                                      \
        }                                                                      \
    } while (0)

static bool same_double(double left, double right) {
    const double difference = left - right;
    return difference < 0.000000001 && difference > -0.000000001;
}

static bool same_state(const FakeGameState *left,
                       const FakeGameState *right) {
    size_t index;

    if (left->phase != right->phase || left->coin != right->coin ||
        left->history_count != right->history_count) {
        return false;
    }

    for (index = 0; index < left->history_count; index += 1) {
        if (left->history[index].previous_phase !=
                right->history[index].previous_phase ||
            left->history[index].previous_coin !=
                right->history[index].previous_coin ||
            left->history[index].applied_action !=
                right->history[index].applied_action) {
            return false;
        }
    }

    return true;
}

static void initialize(FakeGameState *state) {
    memset(state, 0, sizeof(*state));
    CHECK(fake_game_state_init(state) == CFR_STATUS_SUCCESS);
}

static GameState *as_state(FakeGameState *state) {
    return fake_game_state_as_public(state);
}

static const GameState *as_const_state(const FakeGameState *state) {
    return fake_game_state_as_public_const(state);
}

static void check_terminal(const Game *game, const FakeGameState *state,
                           bool expected) {
    bool result = !expected;

    CHECK(cfr_game_is_terminal(game, as_const_state(state), &result) ==
          CFR_STATUS_SUCCESS);
    CHECK(result == expected);
}

static void check_actor(const Game *game, const FakeGameState *state,
                        ActorKind kind, Player player) {
    Actor result = {.kind = CFR_ACTOR_CHANCE, .player = CFR_PLAYER_1};

    CHECK(cfr_game_current_actor(game, as_const_state(state), &result) ==
          CFR_STATUS_SUCCESS);
    CHECK(result.kind == kind);
    if (kind == CFR_ACTOR_PLAYER) {
        CHECK(result.player == player);
    }
}

static void check_actions(const Game *game, const FakeGameState *state,
                          Action first, Action second) {
    Action actions[2] = {-1, -1};
    const size_t actions_capacity = sizeof(actions) / sizeof(actions[0]);
    size_t required_count = 99;

    CHECK(game->max_legal_actions == actions_capacity);
    if (game->max_legal_actions != actions_capacity) {
        return;
    }

    CHECK(cfr_game_legal_actions(game, as_const_state(state), actions,
                                 game->max_legal_actions, &required_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(required_count <= game->max_legal_actions);
    CHECK(required_count == 2);
    CHECK(actions[0] == first);
    CHECK(actions[1] == second);
}

static void check_initial_state(const Game *game, const FakeGameState *state) {
    Probability heads = -1.0;
    Probability tails = -1.0;

    CHECK(state->phase == FAKE_PHASE_CHANCE);
    CHECK(state->coin == FAKE_COIN_NOT_SET);
    CHECK(state->history_count == 0);
    check_terminal(game, state, false);
    check_actor(game, state, CFR_ACTOR_CHANCE, CFR_PLAYER_0);
    check_actions(game, state, FAKE_ACTION_HEADS, FAKE_ACTION_TAILS);

    CHECK(cfr_game_chance_probability(game, as_const_state(state),
                                      FAKE_ACTION_HEADS,
                                      &heads) == CFR_STATUS_SUCCESS);
    CHECK(cfr_game_chance_probability(game, as_const_state(state),
                                      FAKE_ACTION_TAILS,
                                      &tails) == CFR_STATUS_SUCCESS);
    CHECK(same_double(heads, 0.5));
    CHECK(same_double(tails, 0.5));
    CHECK(same_double(heads + tails, 1.0));
}

static void test_initial_and_limits(void) {
    const Game *game = fake_game_descriptor();
    FakeGameState state;
    Action actions[2] = {71, 72};
    size_t required_count = 88;
    Utility utility = 17.0;
    InfoSetKey key = 23;

    CHECK(game != NULL);
    CHECK(game->operations != NULL);
    CHECK(game->context != NULL);
    CHECK(game->max_legal_actions == 2);
    CHECK(fake_game_state_init(NULL) == CFR_STATUS_INVALID_ARGUMENT);

    initialize(&state);
    check_initial_state(game, &state);

    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_double(utility, 17.0));
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(key == 23);

    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions, 0,
                                 &required_count) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(required_count == 2);
    CHECK(actions[0] == 71 && actions[1] == 72);

    required_count = 88;
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions, 1,
                                 &required_count) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(required_count == 2);
    CHECK(actions[0] == 71 && actions[1] == 72);
}

typedef struct {
    FakeAction coin_action;
    FakeCoin coin;
    FakeAction first_action;
    FakeAction response_action;
    size_t expected_history_count;
    Utility expected_player0_utility;
} TerminalPath;

static void run_terminal_path(const TerminalPath *path) {
    const Game *game = fake_game_descriptor();
    FakeGameState state;
    FakeGameState snapshots[4];
    size_t snapshot_count = 1;
    Utility player0 = 41.0;
    Utility player1 = 42.0;
    Actor actor = {.kind = CFR_ACTOR_PLAYER, .player = CFR_PLAYER_0};
    Action actions[2] = {81, 82};
    size_t required_count = 83;
    InfoSetKey key = 84;

    initialize(&state);
    snapshots[0] = state;

    CHECK(cfr_game_apply_action(game, as_state(&state), path->coin_action) ==
          CFR_STATUS_SUCCESS);
    CHECK(state.phase == FAKE_PHASE_PLAYER_0);
    CHECK(state.coin == path->coin);
    CHECK(state.history_count == 1);
    check_terminal(game, &state, false);
    check_actor(game, &state, CFR_ACTOR_PLAYER, CFR_PLAYER_0);
    check_actions(game, &state, FAKE_ACTION_PASS, FAKE_ACTION_BET);
    snapshots[snapshot_count] = state;
    snapshot_count += 1;

    CHECK(cfr_game_apply_action(game, as_state(&state), path->first_action) ==
          CFR_STATUS_SUCCESS);
    snapshots[snapshot_count] = state;
    snapshot_count += 1;

    if (path->first_action == FAKE_ACTION_BET) {
        CHECK(state.phase == FAKE_PHASE_PLAYER_1);
        CHECK(state.history_count == 2);
        check_terminal(game, &state, false);
        check_actor(game, &state, CFR_ACTOR_PLAYER, CFR_PLAYER_1);
        check_actions(game, &state, FAKE_ACTION_FOLD, FAKE_ACTION_CALL);

        CHECK(cfr_game_apply_action(game, as_state(&state),
                                    path->response_action) ==
              CFR_STATUS_SUCCESS);
        snapshots[snapshot_count] = state;
        snapshot_count += 1;
    }

    CHECK(state.phase == FAKE_PHASE_TERMINAL);
    CHECK(state.history_count == path->expected_history_count);
    check_terminal(game, &state, true);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &player0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_1,
                                    &player1) == CFR_STATUS_SUCCESS);
    CHECK(same_double(player0, path->expected_player0_utility));
    CHECK(same_double(player1, -path->expected_player0_utility));
    CHECK(same_double(player0 + player1, 0.0));

    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(actor.kind == CFR_ACTOR_PLAYER && actor.player == CFR_PLAYER_0);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions, 2,
                                 &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(actions[0] == 81 && actions[1] == 82 && required_count == 83);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(key == 84);

    {
        const FakeGameState terminal_snapshot = state;
        CHECK(cfr_game_apply_action(game, as_state(&state), FAKE_ACTION_HEADS) ==
              CFR_STATUS_ILLEGAL_ACTION);
        CHECK(same_state(&state, &terminal_snapshot));
    }

    while (snapshot_count > 1) {
        snapshot_count -= 1;
        CHECK(cfr_game_undo_action(game, as_state(&state)) ==
              CFR_STATUS_SUCCESS);
        CHECK(same_state(&state, &snapshots[snapshot_count - 1]));
    }

    check_initial_state(game, &state);
    {
        const FakeGameState initial_snapshot = state;
        CHECK(cfr_game_undo_action(game, as_state(&state)) ==
              CFR_STATUS_INVALID_ARGUMENT);
        CHECK(same_state(&state, &initial_snapshot));
    }
}

static void test_all_terminal_paths(void) {
    static const TerminalPath paths[] = {
        {FAKE_ACTION_HEADS, FAKE_COIN_HEADS, FAKE_ACTION_PASS,
         FAKE_ACTION_HEADS, 2, 0.0},
        {FAKE_ACTION_TAILS, FAKE_COIN_TAILS, FAKE_ACTION_PASS,
         FAKE_ACTION_HEADS, 2, 0.0},
        {FAKE_ACTION_HEADS, FAKE_COIN_HEADS, FAKE_ACTION_BET,
         FAKE_ACTION_FOLD, 3, 1.0},
        {FAKE_ACTION_TAILS, FAKE_COIN_TAILS, FAKE_ACTION_BET,
         FAKE_ACTION_FOLD, 3, 1.0},
        {FAKE_ACTION_HEADS, FAKE_COIN_HEADS, FAKE_ACTION_BET,
         FAKE_ACTION_CALL, 3, 2.0},
        {FAKE_ACTION_TAILS, FAKE_COIN_TAILS, FAKE_ACTION_BET,
         FAKE_ACTION_CALL, 3, -2.0},
    };
    size_t index;

    for (index = 0; index < sizeof(paths) / sizeof(paths[0]); index += 1) {
        run_terminal_path(&paths[index]);
    }
}

static void collect_information_keys(FakeAction coin_action,
                                     InfoSetKey *player0_key,
                                     InfoSetKey *player1_key) {
    const Game *game = fake_game_descriptor();
    FakeGameState state;

    initialize(&state);
    CHECK(cfr_game_apply_action(game, as_state(&state), coin_action) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state),
                                       player0_key) == CFR_STATUS_SUCCESS);
    CHECK(cfr_game_apply_action(game, as_state(&state), FAKE_ACTION_BET) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_game_information_set_key(game, as_const_state(&state),
                                       player1_key) == CFR_STATUS_SUCCESS);
}

static void test_information_sets(void) {
    InfoSetKey heads_player0 = -1;
    InfoSetKey heads_player1 = -1;
    InfoSetKey tails_player0 = -1;
    InfoSetKey tails_player1 = -1;

    collect_information_keys(FAKE_ACTION_HEADS, &heads_player0,
                             &heads_player1);
    collect_information_keys(FAKE_ACTION_TAILS, &tails_player0,
                             &tails_player1);

    CHECK(heads_player0 == tails_player0);
    CHECK(heads_player1 == tails_player1);
    CHECK(heads_player0 != heads_player1);
}

static void test_illegal_actions_and_capacity(void) {
    const Game *game = fake_game_descriptor();
    FakeGameState state;
    FakeGameState snapshot;

    initialize(&state);
    snapshot = state;
    CHECK(cfr_game_apply_action(game, as_state(&state), FAKE_ACTION_BET) ==
          CFR_STATUS_ILLEGAL_ACTION);
    CHECK(same_state(&state, &snapshot));

    CHECK(cfr_game_apply_action(game, as_state(&state), FAKE_ACTION_HEADS) ==
          CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_game_apply_action(game, as_state(&state), FAKE_ACTION_CALL) ==
          CFR_STATUS_ILLEGAL_ACTION);
    CHECK(same_state(&state, &snapshot));

    CHECK(cfr_game_apply_action(game, as_state(&state), FAKE_ACTION_BET) ==
          CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_game_apply_action(game, as_state(&state), FAKE_ACTION_PASS) ==
          CFR_STATUS_ILLEGAL_ACTION);
    CHECK(same_state(&state, &snapshot));

    state.phase = FAKE_PHASE_PLAYER_0;
    state.history_count = 3;
    snapshot = state;
    CHECK(cfr_game_apply_action(game, as_state(&state), FAKE_ACTION_PASS) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(same_state(&state, &snapshot));
}

static void test_corrupt_states(void) {
    const Game *game = fake_game_descriptor();
    FakeGameState state;
    bool terminal = true;
    Actor actor = {.kind = CFR_ACTOR_PLAYER, .player = CFR_PLAYER_1};
    Action actions[2] = {31, 32};
    size_t required_count = 33;
    Utility utility = 34.0;
    Probability probability = 35.0;
    InfoSetKey key = 36;

    initialize(&state);
    state.phase = (FakePhase)99;

    CHECK(cfr_game_is_terminal(game, as_const_state(&state), &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(terminal);
    CHECK(cfr_game_current_actor(game, as_const_state(&state), &actor) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(actor.kind == CFR_ACTOR_PLAYER && actor.player == CFR_PLAYER_1);
    CHECK(cfr_game_legal_actions(game, as_const_state(&state), actions, 2,
                                 &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(actions[0] == 31 && actions[1] == 32 && required_count == 33);
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_double(utility, 34.0));
    CHECK(cfr_game_chance_probability(game, as_const_state(&state),
                                      FAKE_ACTION_HEADS,
                                      &probability) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_double(probability, 35.0));
    CHECK(cfr_game_information_set_key(game, as_const_state(&state), &key) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(key == 36);

    {
        const FakeGameState snapshot = state;
        CHECK(cfr_game_apply_action(game, as_state(&state), FAKE_ACTION_HEADS) ==
              CFR_STATUS_INVALID_ARGUMENT);
        CHECK(same_state(&state, &snapshot));
    }

    initialize(&state);
    state.phase = FAKE_PHASE_TERMINAL;
    state.history_count = 1;
    state.history[0].applied_action = FAKE_ACTION_BET;
    utility = 34.0;
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), CFR_PLAYER_0,
                                    &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_double(utility, 34.0));

    state.history[0].applied_action = FAKE_ACTION_PASS;
    CHECK(cfr_game_terminal_utility(game, as_const_state(&state), (Player)99,
                                    &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_double(utility, 34.0));
}

static void test_public_wrapper_validation(void) {
    const Game *game = fake_game_descriptor();
    const Game no_operations = {.operations = NULL,
                                .context = NULL,
                                .max_legal_actions = 0};
    const GameOperations empty_operations = {0};
    const Game no_callbacks = {.operations = &empty_operations,
                               .context = NULL,
                               .max_legal_actions = 0};
    FakeGameState state;
    const GameState *const_state;
    GameState *mutable_state;
    bool terminal = true;
    Utility utility = 51.0;
    Actor actor = {.kind = CFR_ACTOR_PLAYER, .player = CFR_PLAYER_1};
    Action actions[2] = {52, 53};
    size_t required_count = 54;
    Probability probability = 55.0;
    InfoSetKey key = 56;

    initialize(&state);
    const_state = as_const_state(&state);
    mutable_state = as_state(&state);

    CHECK(cfr_game_is_terminal(NULL, const_state, &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_terminal_utility(NULL, const_state, CFR_PLAYER_0, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_current_actor(NULL, const_state, &actor) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_legal_actions(NULL, const_state, actions, 2,
                                 &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_apply_action(NULL, mutable_state, FAKE_ACTION_HEADS) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_undo_action(NULL, mutable_state) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_chance_probability(NULL, const_state, FAKE_ACTION_HEADS,
                                      &probability) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_information_set_key(NULL, const_state, &key) ==
          CFR_STATUS_INVALID_ARGUMENT);

    CHECK(cfr_game_is_terminal(&no_operations, const_state, &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_is_terminal(&no_callbacks, const_state, &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_terminal_utility(&no_callbacks, const_state, CFR_PLAYER_0,
                                    &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_current_actor(&no_callbacks, const_state, &actor) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_legal_actions(&no_callbacks, const_state, actions, 2,
                                 &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_apply_action(&no_callbacks, mutable_state,
                                FAKE_ACTION_HEADS) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_undo_action(&no_callbacks, mutable_state) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_chance_probability(&no_callbacks, const_state,
                                      FAKE_ACTION_HEADS,
                                      &probability) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_information_set_key(&no_callbacks, const_state, &key) ==
          CFR_STATUS_INVALID_ARGUMENT);

    CHECK(cfr_game_is_terminal(game, NULL, &terminal) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_is_terminal(game, const_state, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_terminal_utility(game, NULL, CFR_PLAYER_0, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_terminal_utility(game, const_state, CFR_PLAYER_0, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_current_actor(game, NULL, &actor) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_current_actor(game, const_state, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_legal_actions(game, NULL, actions, 2, &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_legal_actions(game, const_state, NULL, 2,
                                 &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_legal_actions(game, const_state, actions, 2, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_apply_action(game, NULL, FAKE_ACTION_HEADS) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_undo_action(game, NULL) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_chance_probability(game, NULL, FAKE_ACTION_HEADS,
                                      &probability) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_chance_probability(game, const_state, FAKE_ACTION_HEADS,
                                      NULL) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_information_set_key(game, NULL, &key) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_game_information_set_key(game, const_state, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);

    CHECK(terminal);
    CHECK(same_double(utility, 51.0));
    CHECK(actor.kind == CFR_ACTOR_PLAYER && actor.player == CFR_PLAYER_1);
    CHECK(actions[0] == 52 && actions[1] == 53 && required_count == 54);
    CHECK(same_double(probability, 55.0));
    CHECK(key == 56);
    CHECK(state.phase == FAKE_PHASE_CHANCE && state.history_count == 0);
}

int test_game_contract(void) {
    failures = 0;

    test_initial_and_limits();
    test_all_terminal_paths();
    test_information_sets();
    test_illegal_actions_and_capacity();
    test_corrupt_states();
    test_public_wrapper_validation();

    return failures;
}

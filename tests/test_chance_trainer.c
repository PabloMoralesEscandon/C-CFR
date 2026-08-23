#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cfr/trainer.h"
#include "cfr/traversal.h"
#include "support/chance_game.h"
#include "support/test_allocator.h"
#include "support/traversal_game.h"
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

static bool near(double left, double right) {
    return fabs(left - right) <= 1e-12;
}

static void initialize_store(InfoStore *store) {
    *store = (InfoStore){0};
    CHECK(cfr_info_store_init(store) == CFR_STATUS_SUCCESS);
}

static void destroy_store(InfoStore *store) {
    CHECK(cfr_info_store_destroy(store) == CFR_STATUS_SUCCESS);
}

static InfoNode *find_node(InfoStore *store, InfoSetKey key) {
    InfoNode *node = NULL;

    CHECK(cfr_info_store_find(store, key, &node) == CFR_STATUS_SUCCESS);
    CHECK(node != NULL);
    return node;
}

static void check_stats(const TrainerStats *stats, size_t iterations,
                        size_t traversals, size_t visited_nodes,
                        size_t errors) {
    CHECK(stats->iterations == iterations);
    CHECK(stats->traversals == traversals);
    CHECK(stats->visited_nodes == visited_nodes);
    CHECK(stats->errors == errors);
}

static void check_coin_node_after_two_iterations(const InfoNode *node) {
    CHECK(near(node->regret_sums[0], -1.5));
    CHECK(near(node->regret_sums[1], 0.5));
    CHECK(near(node->strategy_sums[0], 0.5));
    CHECK(near(node->strategy_sums[1], 1.5));
}

static bool same_trainer(const Trainer *left, const Trainer *right) {
    return left->game == right->game && left->state == right->state &&
           left->store == right->store &&
           left->stats.iterations == right->stats.iterations &&
           left->stats.traversals == right->stats.traversals &&
           left->stats.visited_nodes == right->stats.visited_nodes &&
           left->stats.errors == right->stats.errors;
}

static void test_coin_expectation_reach_and_stats(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    InfoNode *node;
    Utility utility = 70.0;
    TraversalStats stats = {.visited_nodes = 71};

    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_SUCCESS);
    CHECK(near(utility, 0.0));
    CHECK(stats.visited_nodes == 5);
    CHECK(chance_game_state_equal(&state, &snapshot));
    CHECK(store.size == 1);

    node = find_node(&store, 800);
    CHECK(near(node->regret_sums[0], -0.5));
    CHECK(near(node->regret_sums[1], 0.5));
    CHECK(near(node->strategy_sums[0], 0.5));
    CHECK(near(node->strategy_sums[1], 0.5));
    destroy_store(&store);
}

static void test_plain_traversal_supports_chance(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    Utility utility = 71.5;

    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse(game, chance_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, 0.0));
    CHECK(chance_game_state_equal(&state, &snapshot));
    CHECK(store.size == 1);
    destroy_store(&store);
}

static void test_zero_probability_is_still_traversed(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    InfoNode *node;
    Utility utility = 72.0;
    TraversalStats stats = {.visited_nodes = 73};

    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_set_probabilities(&state, 0.0, 1.0);
    snapshot = state;
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_SUCCESS);
    CHECK(near(utility, 1.0));
    CHECK(stats.visited_nodes == 5);
    CHECK(chance_game_state_equal(&state, &snapshot));

    node = find_node(&store, 800);
    CHECK(near(node->regret_sums[0], -1.0));
    CHECK(near(node->regret_sums[1], 1.0));
    CHECK(near(node->strategy_sums[0], 0.5));
    CHECK(near(node->strategy_sums[1], 0.5));
    destroy_store(&store);
}

static void test_distribution_inside_tolerance_is_accepted(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    Utility utility = 74.0;
    TraversalStats stats = {.visited_nodes = 75};

    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_set_probabilities(&state, 0.5, 0.5 + 5e-13);
    snapshot = state;
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_SUCCESS);
    CHECK(near(utility, 5e-13));
    CHECK(stats.visited_nodes == 5);
    CHECK(chance_game_state_equal(&state, &snapshot));
    destroy_store(&store);
}

static void test_invalid_distributions_are_atomic(void) {
    const Game *game = chance_game_descriptor();
    const Probability invalid[][2] = {
        {-0.1, 1.1},
        {NAN, 1.0},
        {INFINITY, 0.0},
        {0.4, 0.4},
        {0.5, 0.50000002},
    };
    size_t index;

    for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index += 1) {
        ChanceGameState state;
        ChanceGameState snapshot;
        InfoStore store;
        Utility utility = 76.0;
        TraversalStats stats = {.visited_nodes = 77};

        initialize_store(&store);
        CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
        chance_game_set_probabilities(&state, invalid[index][0],
                                      invalid[index][1]);
        snapshot = state;
        CHECK(cfr_traverse_with_stats(
                  game, chance_game_state_as_public(&state), &store,
                  CFR_PLAYER_0, &utility, &stats) ==
              CFR_STATUS_INVALID_ARGUMENT);
        CHECK(utility == 76.0);
        CHECK(stats.visited_nodes == 77);
        CHECK(store.size == 0);
        CHECK(chance_game_state_equal(&state, &snapshot));
        destroy_store(&store);
    }
}

static void test_probability_callback_error_is_propagated(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    Utility utility = 78.0;
    TraversalStats stats = {.visited_nodes = 79};

    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_fail_probability(&state, CHANCE_GAME_ACTION_HEADS,
                                 CFR_STATUS_ILLEGAL_ACTION);
    snapshot = state;
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_ILLEGAL_ACTION);
    CHECK(utility == 78.0);
    CHECK(stats.visited_nodes == 79);
    CHECK(store.size == 0);
    CHECK(chance_game_state_equal(&state, &snapshot));
    destroy_store(&store);
}

static void test_error_after_chance_action_restores_state(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    Utility utility = 80.0;
    TraversalStats stats = {.visited_nodes = 81};

    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_fail_after_action(&state, CHANCE_GAME_ACTION_HEADS,
                                  CFR_STATUS_NUMERIC_ERROR);
    snapshot = state;
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(utility == 80.0);
    CHECK(stats.visited_nodes == 81);
    CHECK(store.size == 0);
    CHECK(chance_game_state_equal(&state, &snapshot));
    destroy_store(&store);
}

static void test_chance_undo_error_has_priority(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    InfoStore store;
    Utility utility = 82.0;
    TraversalStats stats = {.visited_nodes = 83};

    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_fail_undo(&state, CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(utility == 82.0);
    CHECK(stats.visited_nodes == 83);
    CHECK(state.phase == CHANCE_GAME_PHASE_TERMINAL);
    CHECK(state.history_count == 1);
    CHECK(state.last_action == CHANCE_GAME_ACTION_TAILS);
    destroy_store(&store);
}

static void test_deep_chance_tree_grows_workspace(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    InfoNode *node;
    Utility utility = 84.0;
    TraversalStats stats = {.visited_nodes = 85};

    initialize_store(&store);
    CHECK(chance_game_state_init_deep(&state, 40) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_SUCCESS);
    CHECK(near(utility, 1.0));
    CHECK(stats.visited_nodes == 43);
    CHECK(chance_game_state_equal(&state, &snapshot));
    CHECK(store.size == 1);
    node = find_node(&store, 801);
    CHECK(near(node->regret_sums[0], -1.0));
    CHECK(near(node->regret_sums[1], 1.0));
    CHECK(near(node->strategy_sums[0], 0.5));
    CHECK(near(node->strategy_sums[1], 0.5));
    destroy_store(&store);
}

static void test_traversal_stats_invalid_arguments(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    Utility utility = 86.0;
    TraversalStats stats = {.visited_nodes = 87};

    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse_with_stats(NULL, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse_with_stats(game, NULL, &store, CFR_PLAYER_0, &utility,
                                  &stats) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  NULL, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, (Player)99, &utility, &stats) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, NULL, &stats) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 86.0);
    CHECK(stats.visited_nodes == 87);
    CHECK(store.size == 0);
    CHECK(chance_game_state_equal(&state, &snapshot));
    destroy_store(&store);
}

static void test_trainer_invalid_arguments_and_zero_iterations(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    Trainer trainer;
    Trainer before;
    Trainer invalid;
    TrainerStats stats = {.iterations = 9,
                          .traversals = 8,
                          .visited_nodes = 7,
                          .errors = 6};

    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    trainer.game = game;
    trainer.state = chance_game_state_as_public(&state);
    trainer.store = &store;
    trainer.stats = stats;
    before = trainer;

    CHECK(cfr_trainer_init(NULL, game, chance_game_state_as_public(&state),
                           &store) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_trainer_init(&trainer, NULL,
                           chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_trainer_init(&trainer, game, NULL, &store) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_trainer_init(&trainer, game,
                           chance_game_state_as_public(&state), NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_trainer(&trainer, &before));

    CHECK(cfr_trainer_init(&trainer, game,
                           chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(NULL, 0) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_trainer_run(&trainer, 0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, 0, 0, 0, 0);
    CHECK(chance_game_state_equal(&state, &snapshot));
    CHECK(store.size == 0);

    stats = (TrainerStats){.iterations = 9,
                           .traversals = 8,
                           .visited_nodes = 7,
                           .errors = 6};
    CHECK(cfr_trainer_get_stats(NULL, &stats) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_trainer_get_stats(&trainer, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    check_stats(&stats, 9, 8, 7, 6);
    CHECK(cfr_trainer_reset_stats(NULL) == CFR_STATUS_INVALID_ARGUMENT);

    invalid = trainer;
    invalid.game = NULL;
    CHECK(cfr_trainer_run(&invalid, 1) == CFR_STATUS_INVALID_ARGUMENT);
    invalid = trainer;
    invalid.state = NULL;
    CHECK(cfr_trainer_run(&invalid, 1) == CFR_STATUS_INVALID_ARGUMENT);
    invalid = trainer;
    invalid.store = NULL;
    CHECK(cfr_trainer_run(&invalid, 1) == CFR_STATUS_INVALID_ARGUMENT);
    destroy_store(&store);
}

static void run_two_iteration_trainer(ChanceGameState *state, InfoStore *store,
                                      TrainerStats *stats_out,
                                      InfoNode **node_out) {
    Trainer trainer;

    CHECK(chance_game_state_init_coin(state) == CFR_STATUS_SUCCESS);
    initialize_store(store);
    CHECK(cfr_trainer_init(&trainer, chance_game_descriptor(),
                           chance_game_state_as_public(state), store) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, 2) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, stats_out) == CFR_STATUS_SUCCESS);
    *node_out = find_node(store, 800);
}

static void test_trainer_exact_iterations_and_determinism(void) {
    ChanceGameState state_a;
    ChanceGameState state_b;
    ChanceGameState snapshot_a;
    ChanceGameState snapshot_b;
    InfoStore store_a;
    InfoStore store_b;
    InfoNode *node_a;
    InfoNode *node_b;
    TrainerStats stats_a;
    TrainerStats stats_b;

    CHECK(chance_game_state_init_coin(&snapshot_a) == CFR_STATUS_SUCCESS);
    snapshot_b = snapshot_a;
    run_two_iteration_trainer(&state_a, &store_a, &stats_a, &node_a);
    run_two_iteration_trainer(&state_b, &store_b, &stats_b, &node_b);

    check_stats(&stats_a, 2, 4, 20, 0);
    check_stats(&stats_b, 2, 4, 20, 0);
    CHECK(chance_game_state_equal(&state_a, &snapshot_a));
    CHECK(chance_game_state_equal(&state_b, &snapshot_b));
    check_coin_node_after_two_iterations(node_a);
    check_coin_node_after_two_iterations(node_b);
    CHECK(node_a->action_count == node_b->action_count);
    CHECK(node_a->regret_sums[0] == node_b->regret_sums[0]);
    CHECK(node_a->regret_sums[1] == node_b->regret_sums[1]);
    CHECK(node_a->strategy_sums[0] == node_b->strategy_sums[0]);
    CHECK(node_a->strategy_sums[1] == node_b->strategy_sums[1]);
    destroy_store(&store_a);
    destroy_store(&store_b);
}

static void test_trainer_alternating_update_changes_player_1_learning(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    InfoStore store;
    InfoNode *player_0_node;
    InfoNode *player_1_node;
    Trainer trainer;
    TrainerStats stats;

    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    initialize_store(&store);
    CHECK(cfr_trainer_init(&trainer, game,
                           traversal_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, 1, 2, 10, 0);
    CHECK(state.phase == TRAVERSAL_PHASE_ROOT_PLAYER_0);
    CHECK(state.history_count == 0);

    player_0_node = find_node(&store, 100);
    player_1_node = find_node(&store, 200);
    CHECK(near(player_0_node->regret_sums[0], 1.0));
    CHECK(near(player_0_node->regret_sums[1], -1.0));
    CHECK(near(player_0_node->strategy_sums[0], 0.5));
    CHECK(near(player_0_node->strategy_sums[1], 0.5));
    CHECK(near(player_1_node->regret_sums[0], 0.0));
    CHECK(near(player_1_node->regret_sums[1], 0.0));
    CHECK(near(player_1_node->strategy_sums[0], 0.5));
    CHECK(near(player_1_node->strategy_sums[1], 0.5));
    destroy_store(&store);
}

static void test_trainer_reset_preserves_learning_and_loans(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    InfoStore store;
    InfoNode *node;
    Trainer trainer;
    TrainerStats stats;
    Utility regrets[2];
    double strategies[2];

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    initialize_store(&store);
    CHECK(cfr_trainer_init(&trainer, game,
                           chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    node = find_node(&store, 800);
    regrets[0] = node->regret_sums[0];
    regrets[1] = node->regret_sums[1];
    strategies[0] = node->strategy_sums[0];
    strategies[1] = node->strategy_sums[1];

    CHECK(cfr_trainer_reset_stats(&trainer) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, 0, 0, 0, 0);
    CHECK(trainer.game == game);
    CHECK(trainer.state == chance_game_state_as_public(&state));
    CHECK(trainer.store == &store);
    CHECK(node->regret_sums[0] == regrets[0]);
    CHECK(node->regret_sums[1] == regrets[1]);
    CHECK(node->strategy_sums[0] == strategies[0]);
    CHECK(node->strategy_sums[1] == strategies[1]);
    destroy_store(&store);
}

static void test_trainer_second_traversal_error_and_recovery(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    InfoNode *node;
    Trainer trainer;
    TrainerStats stats;

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_fail_terminal_for_player(&state, CFR_PLAYER_1,
                                         CFR_STATUS_NUMERIC_ERROR);
    snapshot = state;
    initialize_store(&store);
    CHECK(cfr_trainer_init(&trainer, game,
                           chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, 0, 1, 5, 1);
    CHECK(chance_game_state_equal(&state, &snapshot));
    node = find_node(&store, 800);
    CHECK(near(node->regret_sums[0], -0.5));
    CHECK(near(node->regret_sums[1], 0.5));
    CHECK(near(node->strategy_sums[0], 0.5));
    CHECK(near(node->strategy_sums[1], 0.5));

    chance_game_fail_terminal_for_player(&state, CFR_PLAYER_1,
                                         CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, 1, 3, 15, 1);
    CHECK(chance_game_state_equal(&state, &snapshot));
    check_coin_node_after_two_iterations(node);
    destroy_store(&store);
}

static void test_trainer_undo_error_requires_explicit_restoration(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState root;
    InfoStore store;
    Trainer trainer;
    TrainerStats stats;

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    root = state;
    chance_game_fail_undo(&state, CFR_STATUS_BUFFER_TOO_SMALL);
    initialize_store(&store);
    CHECK(cfr_trainer_init(&trainer, game,
                           chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);

    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, 0, 0, 0, 1);
    CHECK(state.phase == CHANCE_GAME_PHASE_TERMINAL);
    CHECK(state.history_count == 1);
    CHECK(state.last_action == CHANCE_GAME_ACTION_TAILS);
    CHECK(!chance_game_state_equal(&state, &root));
    CHECK(store.size == 0);

    chance_game_fail_undo(&state, CFR_STATUS_SUCCESS);
    CHECK(cfr_game_undo_action(game, chance_game_state_as_public(&state)) ==
          CFR_STATUS_SUCCESS);
    CHECK(chance_game_state_equal(&state, &root));
    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, 1, 2, 10, 1);
    CHECK(chance_game_state_equal(&state, &root));
    destroy_store(&store);
}

static void test_saturated_counters_stay_at_maximum_on_success(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState root;
    InfoStore store;
    Trainer trainer;
    TrainerStats stats;

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    root = state;
    initialize_store(&store);
    CHECK(cfr_trainer_init(&trainer, game,
                           chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    trainer.stats = (TrainerStats){.iterations = SIZE_MAX,
                                   .traversals = SIZE_MAX,
                                   .visited_nodes = SIZE_MAX,
                                   .errors = SIZE_MAX};

    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX);
    CHECK(chance_game_state_equal(&state, &root));
    destroy_store(&store);
}

static void test_saturated_counters_stay_at_maximum_on_error(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState root;
    InfoStore store;
    Trainer trainer;
    TrainerStats stats;

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_fail_terminal_for_player(&state, CFR_PLAYER_0,
                                         CFR_STATUS_NUMERIC_ERROR);
    root = state;
    initialize_store(&store);
    CHECK(cfr_trainer_init(&trainer, game,
                           chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    trainer.stats = (TrainerStats){.iterations = SIZE_MAX,
                                   .traversals = SIZE_MAX,
                                   .visited_nodes = SIZE_MAX,
                                   .errors = SIZE_MAX};

    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX);
    CHECK(chance_game_state_equal(&state, &root));
    destroy_store(&store);
}

static void test_visited_nodes_saturates_when_five_do_not_fit(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState root;
    InfoStore store;
    Trainer trainer;
    TrainerStats stats;

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    root = state;
    initialize_store(&store);
    CHECK(cfr_trainer_init(&trainer, game,
                           chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    trainer.stats.visited_nodes = SIZE_MAX - 3;

    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, 1, 2, SIZE_MAX, 0);
    CHECK(chance_game_state_equal(&state, &root));
    destroy_store(&store);
}

static void test_counters_increment_from_one_below_maximum(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState root;
    InfoStore store;
    Trainer trainer;
    TrainerStats stats;

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_fail_terminal_for_player(&state, CFR_PLAYER_1,
                                         CFR_STATUS_NUMERIC_ERROR);
    root = state;
    initialize_store(&store);
    CHECK(cfr_trainer_init(&trainer, game,
                           chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    trainer.stats = (TrainerStats){.iterations = SIZE_MAX - 1,
                                   .traversals = SIZE_MAX - 1,
                                   .visited_nodes = SIZE_MAX - 1,
                                   .errors = SIZE_MAX - 1};

    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, SIZE_MAX - 1, SIZE_MAX, SIZE_MAX, SIZE_MAX);
    CHECK(chance_game_state_equal(&state, &root));

    chance_game_fail_terminal_for_player(&state, CFR_PLAYER_1,
                                         CFR_STATUS_SUCCESS);
    root = state;
    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    check_stats(&stats, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX);
    CHECK(chance_game_state_equal(&state, &root));
    destroy_store(&store);
}

static void test_trainer_counter_saturation(void) {
    test_saturated_counters_stay_at_maximum_on_success();
    test_saturated_counters_stay_at_maximum_on_error();
    test_visited_nodes_saturates_when_five_do_not_fit();
    test_counters_increment_from_one_below_maximum();
}

#ifdef CFR_TEST_WRAP_ALLOCATOR
static void test_workspace_initialization_failures_are_clean(void) {
    const Game *game = chance_game_descriptor();
    size_t failure_index;

    for (failure_index = 0; failure_index < 4; failure_index += 1) {
        ChanceGameState state;
        ChanceGameState snapshot;
        InfoStore store;
        Utility utility = 88.0;
        TraversalStats stats = {.visited_nodes = 89};
        size_t live_before;

        initialize_store(&store);
        CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
        snapshot = state;
        live_before = test_allocator_live_allocations();
        test_allocator_fail_after(failure_index);
        CHECK(cfr_traverse_with_stats(
                  game, chance_game_state_as_public(&state), &store,
                  CFR_PLAYER_0, &utility, &stats) ==
              CFR_STATUS_OUT_OF_MEMORY);
        test_allocator_disable_failures();
        CHECK(test_allocator_live_allocations() == live_before);
        CHECK(utility == 88.0);
        CHECK(stats.visited_nodes == 89);
        CHECK(store.size == 0);
        CHECK(chance_game_state_equal(&state, &snapshot));
        destroy_store(&store);
        CHECK(test_allocator_live_allocations() == 0);
    }
}

static void test_frame_growth_realloc_failure_is_clean(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    InfoNode *node = NULL;
    Utility utility = 90.0;
    TraversalStats stats = {.visited_nodes = 91};
    size_t live_before;

    initialize_store(&store);
    CHECK(cfr_info_store_get_or_create(&store, 801, 2, &node) ==
          CFR_STATUS_SUCCESS);
    node->regret_sums[0] = 2.0;
    node->regret_sums[1] = -3.0;
    node->strategy_sums[0] = 0.25;
    node->strategy_sums[1] = 0.75;
    CHECK(chance_game_state_init_deep(&state, 40) == CFR_STATUS_SUCCESS);
    snapshot = state;
    live_before = test_allocator_live_allocations();

    test_allocator_fail_after(4);
    CHECK(cfr_traverse_with_stats(game, chance_game_state_as_public(&state),
                                  &store, CFR_PLAYER_0, &utility, &stats) ==
          CFR_STATUS_OUT_OF_MEMORY);
    test_allocator_disable_failures();
    CHECK(test_allocator_live_allocations() == live_before);
    CHECK(utility == 90.0);
    CHECK(stats.visited_nodes == 91);
    CHECK(chance_game_state_equal(&state, &snapshot));
    CHECK(store.size == 1);
    CHECK(node->regret_sums[0] == 2.0);
    CHECK(node->regret_sums[1] == -3.0);
    CHECK(node->strategy_sums[0] == 0.25);
    CHECK(node->strategy_sums[1] == 0.75);
    destroy_store(&store);
    CHECK(test_allocator_live_allocations() == 0);
}
#endif

int test_chance_trainer(void) {
    failures = 0;

    test_coin_expectation_reach_and_stats();
    test_plain_traversal_supports_chance();
    test_zero_probability_is_still_traversed();
    test_distribution_inside_tolerance_is_accepted();
    test_invalid_distributions_are_atomic();
    test_probability_callback_error_is_propagated();
    test_error_after_chance_action_restores_state();
    test_chance_undo_error_has_priority();
    test_deep_chance_tree_grows_workspace();
    test_traversal_stats_invalid_arguments();
    test_trainer_invalid_arguments_and_zero_iterations();
    test_trainer_exact_iterations_and_determinism();
    test_trainer_alternating_update_changes_player_1_learning();
    test_trainer_reset_preserves_learning_and_loans();
    test_trainer_second_traversal_error_and_recovery();
    test_trainer_undo_error_requires_explicit_restoration();
    test_trainer_counter_saturation();
#ifdef CFR_TEST_WRAP_ALLOCATOR
    test_workspace_initialization_failures_are_clean();
    test_frame_growth_realloc_failure_is_clean();
#endif

    return failures;
}

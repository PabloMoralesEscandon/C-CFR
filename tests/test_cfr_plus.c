#include <math.h>
#include <stdio.h>

#include "cfr/info_node.h"
#include "cfr/info_store.h"
#include "cfr/trainer.h"
#include "cfr/traversal.h"
#include "support/chance_game.h"
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

static void test_plus_traversal_truncates_and_weights(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    InfoStore store;
    InfoNode *node;
    Utility utility = 91.0;
    TraversalStats stats = {.visited_nodes = 92};

    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    initialize_store(&store);

    CHECK(cfr_traverse_plus_with_stats(
              game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, 3, &utility, &stats) == CFR_STATUS_SUCCESS);
    CHECK(stats.visited_nodes == 5);
    CHECK(state.phase == TRAVERSAL_PHASE_ROOT_PLAYER_0);
    CHECK(state.history_count == 0);

    node = find_node(&store, 100);
    CHECK(near(node->regret_sums[0], 1.0));
    CHECK(near(node->regret_sums[1], 0.0));
    CHECK(near(node->strategy_sums[0], 1.5));
    CHECK(near(node->strategy_sums[1], 1.5));
    destroy_store(&store);
}

static void test_plus_traversal_rejects_iteration_zero_atomically(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    Utility utility = 93.0;
    TraversalStats stats = {.visited_nodes = 94};

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    initialize_store(&store);

    CHECK(cfr_traverse_plus_with_stats(
              game, chance_game_state_as_public(&state), &store, CFR_PLAYER_0,
              0, &utility, &stats) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse_plus(game, chance_game_state_as_public(&state), &store,
                            CFR_PLAYER_0, 0, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 93.0);
    CHECK(stats.visited_nodes == 94);
    CHECK(store.size == 0);
    CHECK(chance_game_state_equal(&state, &snapshot));
    destroy_store(&store);
}

static void test_plus_trainer_uses_linear_average_across_calls(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState root;
    InfoStore store;
    InfoNode *node;
    Trainer trainer;
    TrainerStats stats;
    Probability average[2] = {0.0, 0.0};

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    root = state;
    initialize_store(&store);
    CHECK(cfr_trainer_init_plus(&trainer, game,
                                chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    CHECK(trainer.variant == CFR_TRAINER_VARIANT_CFR_PLUS);
    CHECK(trainer.training_iterations == 0);

    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    node = find_node(&store, 800);
    CHECK(near(node->regret_sums[0], 0.0));
    CHECK(near(node->regret_sums[1], 0.5));
    CHECK(near(node->strategy_sums[0], 0.5));
    CHECK(near(node->strategy_sums[1], 0.5));
    CHECK(trainer.training_iterations == 1);

    CHECK(cfr_trainer_reset_stats(&trainer) == CFR_STATUS_SUCCESS);
    CHECK(trainer.training_iterations == 1);
    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats) == CFR_STATUS_SUCCESS);
    CHECK(stats.iterations == 1);
    CHECK(stats.traversals == 2);
    CHECK(stats.visited_nodes == 10);
    CHECK(stats.errors == 0);
    CHECK(trainer.training_iterations == 2);
    CHECK(chance_game_state_equal(&state, &root));

    CHECK(near(node->regret_sums[0], 0.0));
    CHECK(near(node->regret_sums[1], 0.5));
    CHECK(near(node->strategy_sums[0], 0.5));
    CHECK(near(node->strategy_sums[1], 2.5));
    CHECK(cfr_info_node_average_strategy(node, average, 2) ==
          CFR_STATUS_SUCCESS);
    CHECK(near(average[0], 1.0 / 6.0));
    CHECK(near(average[1], 5.0 / 6.0));
    destroy_store(&store);
}

static void test_plus_trainer_validation(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    InfoStore store;
    Trainer trainer = {0};
    Trainer before;

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    initialize_store(&store);
    trainer.variant = CFR_TRAINER_VARIANT_CFR;
    trainer.training_iterations = 17;
    before = trainer;

    CHECK(cfr_trainer_init_plus(NULL, game,
                                chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_trainer_init_plus(&trainer, NULL,
                                chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_trainer_init_plus(&trainer, game, NULL, &store) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_trainer_init_plus(&trainer, game,
                                chance_game_state_as_public(&state), NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(trainer.game == before.game);
    CHECK(trainer.state == before.state);
    CHECK(trainer.store == before.store);
    CHECK(trainer.variant == before.variant);
    CHECK(trainer.training_iterations == before.training_iterations);

    CHECK(cfr_trainer_init_plus(&trainer, game,
                                chance_game_state_as_public(&state), &store) ==
          CFR_STATUS_SUCCESS);
    trainer.variant = (TrainerVariant)99;
    CHECK(cfr_trainer_run(&trainer, 1) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(store.size == 0);
    destroy_store(&store);
}

int test_cfr_plus(void) {
    failures = 0;

    test_plus_traversal_truncates_and_weights();
    test_plus_traversal_rejects_iteration_zero_atomically();
    test_plus_trainer_uses_linear_average_across_calls();
    test_plus_trainer_validation();

    return failures;
}

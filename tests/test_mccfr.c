#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfr/blackjack.h"
#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/kuhn_poker.h"
#include "cfr/mccfr.h"
#include "cfr/traversal.h"
#include "cfr/trainer.h"
#include "support/chance_game.h"
#include "support/test_allocator.h"
#include "support/traversal_game.h"
#include "test_suite.h"

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            failures += 1;                                                     \
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

static void test_rng_contract(void) {
    MccfrRng rng = {.state = 91};

    CHECK(cfr_mccfr_rng_seed(NULL, 7) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_mccfr_rng_seed(&rng, 0) == CFR_STATUS_SUCCESS);
    CHECK(rng.state == 0);
    CHECK(cfr_mccfr_rng_seed(&rng, UINT64_MAX) == CFR_STATUS_SUCCESS);
    CHECK(rng.state == UINT64_MAX);
}

static void test_chance_is_sampled_and_target_actions_are_expanded(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState root;
    InfoStore store;
    InfoNode *node;
    MccfrRng rng;
    Utility utility = 91.0;
    TraversalStats stats = {.visited_nodes = 92};

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_set_probabilities(&state, 0.0, 1.0);
    root = state;
    initialize_store(&store);
    CHECK(cfr_mccfr_rng_seed(&rng, 11) == CFR_STATUS_SUCCESS);

    CHECK(cfr_mccfr_external_traverse_with_stats(
              game, chance_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &rng, &utility, &stats) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, 1.0));
    CHECK(stats.visited_nodes == 4);
    CHECK(chance_game_state_equal(&state, &root));
    CHECK(rng.state == UINT64_C(11) + UINT64_C(0x9e3779b97f4a7c15));

    node = find_node(&store, 800);
    CHECK(near(node->regret_sums[0], -1.0));
    CHECK(near(node->regret_sums[1], 1.0));
    /*
     * The game declares two strategic players, so an average strategy for
     * player zero belongs to a traversal that samples player zero. This
     * traversal targets player zero and therefore records regrets only.
     */
    CHECK(near(node->strategy_sums[0], 0.0));
    CHECK(near(node->strategy_sums[1], 0.0));
    destroy_store(&store);
}

/*
 * The two children of key 500 contain different hidden histories but lead to
 * the same opponent information set, key 501. External sampling must select
 * one action for key 501 and reuse it in both children. Otherwise the sampled
 * opponent policy can condition on information that the opponent cannot see.
 */
static void test_opponent_sample_is_shared_by_information_set(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState root;
    InfoStore store;
    InfoNode *root_node;
    InfoNode *opponent_node;
    MccfrRng rng;
    Utility utility = 93.0;
    TraversalStats stats = {.visited_nodes = 94};
    const uint64_t seed = UINT64_C(1234567);

    CHECK(traversal_game_state_init_shared(&state, false) ==
          CFR_STATUS_SUCCESS);
    root = state;
    initialize_store(&store);
    CHECK(cfr_mccfr_rng_seed(&rng, seed) == CFR_STATUS_SUCCESS);

    CHECK(cfr_mccfr_external_traverse_with_stats(
              game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &rng, &utility, &stats) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, -2.0));
    CHECK(stats.visited_nodes == 5);
    CHECK(state.phase == root.phase);
    CHECK(state.history_count == root.history_count);
    /* Exactly one draw proves that the second hidden history reused key 501. */
    CHECK(rng.state == seed + UINT64_C(0x9e3779b97f4a7c15));

    root_node = find_node(&store, 500);
    opponent_node = find_node(&store, 501);
    CHECK(near(root_node->regret_sums[0],
               -root_node->regret_sums[1]));
    CHECK(near(fabs(root_node->regret_sums[0]), 2.0));
    CHECK(near(root_node->strategy_sums[0], 0.0));
    CHECK(near(root_node->strategy_sums[1], 0.0));
    CHECK(near(opponent_node->regret_sums[0], 0.0));
    CHECK(near(opponent_node->regret_sums[1], 0.0));
    /*
     * Key 501 carries the average strategy because player one is sampled here.
     * Both hidden histories reach it, so each action collects its uniform
     * probability twice.
     */
    CHECK(near(opponent_node->strategy_sums[0], 1.0));
    CHECK(near(opponent_node->strategy_sums[1], 1.0));
    destroy_store(&store);
}

static void test_error_preserves_rng_outputs_and_learning(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState root;
    InfoStore store;
    InfoNode *node;
    MccfrRng rng;
    Utility utility = 95.0;
    TraversalStats stats = {.visited_nodes = 96};

    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    traversal_game_fail_after_action(&state, TRAVERSAL_ACTION_YIELD,
                                     CFR_STATUS_NUMERIC_ERROR);
    root = state;
    initialize_store(&store);
    CHECK(cfr_mccfr_rng_seed(&rng, 77) == CFR_STATUS_SUCCESS);

    CHECK(cfr_mccfr_external_traverse_with_stats(
              game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &rng, &utility, &stats) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(rng.state == 77);
    CHECK(utility == 95.0);
    CHECK(stats.visited_nodes == 96);
    CHECK(state.phase == root.phase);
    CHECK(state.history_count == root.history_count);
    node = find_node(&store, 100);
    CHECK(node->regret_sums[0] == 0.0);
    CHECK(node->regret_sums[1] == 0.0);
    CHECK(node->strategy_sums[0] == 0.0);
    CHECK(node->strategy_sums[1] == 0.0);
    destroy_store(&store);
}

static void test_hidden_histories_require_identical_action_mapping(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    InfoStore store;
    MccfrRng rng;
    Utility utility = 97.0;
    TraversalStats stats = {.visited_nodes = 98};

    CHECK(traversal_game_state_init_inconsistent_shared(&state) ==
          CFR_STATUS_SUCCESS);
    initialize_store(&store);
    CHECK(cfr_mccfr_rng_seed(&rng, 79) == CFR_STATUS_SUCCESS);
    CHECK(cfr_mccfr_external_traverse_with_stats(
              game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &rng, &utility, &stats) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(rng.state == 79);
    CHECK(utility == 97.0);
    CHECK(stats.visited_nodes == 98);
    CHECK(state.phase == TRAVERSAL_PHASE_SHARED_ROOT_PLAYER_0);
    CHECK(state.history_count == 0);
    destroy_store(&store);
}

static void test_seeded_trainers_are_reproducible(void) {
    const Game *game = chance_game_descriptor();
    ChanceGameState state_a;
    ChanceGameState state_b;
    InfoStore store_a;
    InfoStore store_b;
    Trainer trainer_a;
    Trainer trainer_b;

    CHECK(chance_game_state_init_coin(&state_a) == CFR_STATUS_SUCCESS);
    state_b = state_a;
    initialize_store(&store_a);
    initialize_store(&store_b);
    CHECK(cfr_trainer_init_mccfr(
              &trainer_a, game, chance_game_state_as_public(&state_a),
              &store_a, 991) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init_mccfr(
              &trainer_b, game, chance_game_state_as_public(&state_b),
              &store_b, 991) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer_a, 100) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer_b, 100) == CFR_STATUS_SUCCESS);
    CHECK(trainer_a.variant == CFR_TRAINER_VARIANT_MCCFR_EXTERNAL);
    CHECK(trainer_a.mccfr_rng.state == trainer_b.mccfr_rng.state);
    CHECK(trainer_a.stats.iterations == 100);
    CHECK(trainer_a.stats.traversals == 200);
    CHECK(trainer_a.stats.visited_nodes == trainer_b.stats.visited_nodes);
    CHECK(store_a.size == store_b.size);
    if (store_a.size != 0) {
        InfoNode *node_a = find_node(&store_a, 800);
        InfoNode *node_b = find_node(&store_b, 800);

        CHECK(node_a->regret_sums[0] == node_b->regret_sums[0]);
        CHECK(node_a->regret_sums[1] == node_b->regret_sums[1]);
        CHECK(node_a->strategy_sums[0] == node_b->strategy_sums[0]);
        CHECK(node_a->strategy_sums[1] == node_b->strategy_sums[1]);
    }
    destroy_store(&store_a);
    destroy_store(&store_b);
}

#define MCCFR_TEST_MAX_SLOTS 64
#define MCCFR_TEST_MAX_KEYS 32

/* Collects every key the store holds, in ascending key order. */
static size_t collect_keys(const InfoStore *store, InfoSetKey *keys,
                           size_t *action_counts) {
    size_t count = 0;

    for (InfoSetKey key = 0; key < 64 && count < MCCFR_TEST_MAX_KEYS; key += 1) {
        const InfoNode *node;

        if (cfr_info_store_find_const(store, key, &node) != CFR_STATUS_SUCCESS)
            continue;
        keys[count] = key;
        action_counts[count] = node->action_count;
        count += 1;
    }
    return count;
}

static void copy_strategy_sums(InfoStore *store, const InfoSetKey *keys,
                               size_t key_count, double *out) {
    size_t slot = 0;

    for (size_t index = 0; index < key_count; index += 1) {
        InfoNode *node = find_node(store, keys[index]);

        for (size_t action = 0; action < node->action_count; action += 1) {
            out[slot] = node->strategy_sums[action];
            slot += 1;
        }
    }
}

static void restore_accumulators(InfoStore *store, const InfoSetKey *keys,
                                 size_t key_count, const double *regrets,
                                 const double *strategies) {
    size_t slot = 0;

    for (size_t index = 0; index < key_count; index += 1) {
        InfoNode *node = find_node(store, keys[index]);

        for (size_t action = 0; action < node->action_count; action += 1) {
            node->regret_sums[action] = regrets[slot];
            node->strategy_sums[action] = strategies[slot];
            slot += 1;
        }
    }
}

/*
 * A traversal that targets one player must leave the other player's average
 * strategy proportional to the full CFR strategy sums, so the normalized
 * distributions must agree exactly.
 *
 * The profile is trained first so that regret matching produces exact zero
 * probabilities. A history behind a zero-probability action can never be
 * sampled, yet it still contributes to the full CFR strategy sums, so an
 * estimator that divides by the external reach silently loses it.
 */
static void test_sampled_player_average_matches_exact_cfr(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState state;
    InfoSetKey keys[MCCFR_TEST_MAX_KEYS];
    size_t action_counts[MCCFR_TEST_MAX_KEYS];
    double base_regrets[MCCFR_TEST_MAX_SLOTS];
    double base_strategies[MCCFR_TEST_MAX_SLOTS];
    double exact[MCCFR_TEST_MAX_SLOTS];
    double sampled[MCCFR_TEST_MAX_SLOTS] = {0};
    double scratch[MCCFR_TEST_MAX_SLOTS];
    InfoStore store;
    Utility utility;
    size_t zero_probabilities = 0;
    size_t compared_sets = 0;

    CHECK(cfr_kuhn_poker_state_init(&state) == CFR_STATUS_SUCCESS);
    initialize_store(&store);

    GameState *public_state = cfr_kuhn_poker_state_as_game_state(&state);

    /* Train a skewed profile, then freeze it as the common starting point. */
    for (size_t iteration = 0; iteration < 200; iteration += 1) {
        CHECK(cfr_traverse(game, public_state, &store, CFR_PLAYER_0,
                           &utility) == CFR_STATUS_SUCCESS);
        CHECK(cfr_traverse(game, public_state, &store, CFR_PLAYER_1,
                           &utility) == CFR_STATUS_SUCCESS);
    }

    const size_t key_count = collect_keys(&store, keys, action_counts);
    CHECK(key_count == 12);

    size_t slot_count = 0;
    for (size_t index = 0; index < key_count; index += 1)
        slot_count += action_counts[index];
    CHECK(slot_count <= MCCFR_TEST_MAX_SLOTS);

    for (size_t index = 0, slot = 0; index < key_count; index += 1) {
        InfoNode *node = find_node(&store, keys[index]);
        Probability strategy[CFR_TRAVERSAL_MAX_ACTIONS];

        CHECK(cfr_info_node_current_strategy(node, strategy,
                                             CFR_TRAVERSAL_MAX_ACTIONS) ==
              CFR_STATUS_SUCCESS);
        for (size_t action = 0; action < node->action_count; action += 1,
                                                             slot += 1) {
            base_regrets[slot] = node->regret_sums[action];
            base_strategies[slot] = node->strategy_sums[action];
            if (strategy[action] == 0.0)
                zero_probabilities += 1;
        }
    }
    /* Without exact zeros the comparison below cannot detect the defect. */
    CHECK(zero_probabilities > 0);

    /* Exact CFR strategy-sum deltas for player one. */
    CHECK(cfr_traverse(game, public_state, &store, CFR_PLAYER_1, &utility) ==
          CFR_STATUS_SUCCESS);
    copy_strategy_sums(&store, keys, key_count, exact);
    for (size_t slot = 0; slot < slot_count; slot += 1)
        exact[slot] -= base_strategies[slot];

    /*
     * Sampled deltas for player one, gathered while targeting player zero.
     * Each traversal restarts from the frozen profile, so several seeds only
     * widen the set of information sets reached.
     */
    for (uint64_t seed = 1; seed <= 8; seed += 1) {
        MccfrRng rng;

        restore_accumulators(&store, keys, key_count, base_regrets,
                             base_strategies);
        CHECK(cfr_mccfr_rng_seed(&rng, seed) == CFR_STATUS_SUCCESS);
        CHECK(cfr_mccfr_external_traverse(game, public_state, &store,
                                          CFR_PLAYER_0, &rng, &utility) ==
              CFR_STATUS_SUCCESS);
        copy_strategy_sums(&store, keys, key_count, scratch);
        for (size_t slot = 0; slot < slot_count; slot += 1)
            sampled[slot] += scratch[slot] - base_strategies[slot];
    }
    restore_accumulators(&store, keys, key_count, base_regrets,
                         base_strategies);

    for (size_t index = 0, slot = 0; index < key_count;
         slot += action_counts[index], index += 1) {
        double exact_total = 0.0;
        double sampled_total = 0.0;

        for (size_t action = 0; action < action_counts[index]; action += 1) {
            exact_total += exact[slot + action];
            sampled_total += sampled[slot + action];
        }
        if (sampled_total == 0.0)
            continue;
        /* Only a sampled player's information set may collect an average. */
        CHECK(exact_total > 0.0);
        for (size_t action = 0; action < action_counts[index]; action += 1) {
            CHECK(near(sampled[slot + action] / sampled_total,
                       exact[slot + action] / exact_total));
        }
        compared_sets += 1;
    }
    /* An estimator that records nothing here would compare nothing. */
    CHECK(compared_sets >= 4);
    destroy_store(&store);
}

/*
 * Blackjack declares a single strategic player, so no sampled player can carry
 * the average strategy and the traversal must weight the target's own strategy
 * by its reach. An average that never accumulates degrades to uniform, which
 * costs about two orders of magnitude in the optimality gap.
 */
static void test_single_strategic_player_accumulates_average(void) {
    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state;
    InfoStore store;
    Trainer trainer;
    Utility profile_value = 0.0;
    Utility best_response_value = 0.0;

    CHECK(cfr_blackjack_state_init(&state) == CFR_STATUS_SUCCESS);

    GameState *public_state = cfr_blackjack_state_as_game_state(&state);
    const Action deal[] = {CFR_BLACKJACK_ACTION_DEAL_FIVE,
                           CFR_BLACKJACK_ACTION_DEAL_TEN,
                           CFR_BLACKJACK_ACTION_DEAL_SIX};

    for (size_t card = 0; card < sizeof(deal) / sizeof(deal[0]); card += 1) {
        CHECK(cfr_game_apply_action(game, public_state, deal[card]) ==
              CFR_STATUS_SUCCESS);
    }
    initialize_store(&store);
    CHECK(cfr_trainer_init_mccfr(&trainer, game, public_state, &store, 7) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, 20000) == CFR_STATUS_SUCCESS);
    CHECK(store.size == 10);

    for (InfoSetKey key = 0; key < 4096; key += 1) {
        const InfoNode *node;
        double total = 0.0;

        if (cfr_info_store_find_const(&store, key, &node) !=
            CFR_STATUS_SUCCESS)
            continue;
        for (size_t action = 0; action < node->action_count; action += 1)
            total += node->strategy_sums[action];
        CHECK(total > 0.0);
    }

    CHECK(cfr_evaluation_profile_value(game, public_state, &store,
                                       CFR_PLAYER_0, &profile_value) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_best_response_value(
              game, public_state, &store, CFR_PLAYER_0,
              &best_response_value) == CFR_STATUS_SUCCESS);
    CHECK(best_response_value - profile_value < 0.01);
    destroy_store(&store);
}

static void test_kuhn_converges(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState state;
    InfoStore store;
    Trainer trainer;
    EvaluationMetrics metrics;

    CHECK(cfr_kuhn_poker_state_init(&state) == CFR_STATUS_SUCCESS);
    initialize_store(&store);
    CHECK(cfr_trainer_init_mccfr(
              &trainer, game,
              cfr_kuhn_poker_state_as_game_state(&state), &store,
              UINT64_C(20260831)) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, 100000) == CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_metrics(
              game, cfr_kuhn_poker_state_as_game_state(&state), &store,
              &metrics) == CFR_STATUS_SUCCESS);
    CHECK(store.size == 12);
    CHECK(metrics.exploitability < 0.005);
    destroy_store(&store);
}

static bool files_equal(FILE *left, FILE *right) {
    int left_byte;
    int right_byte;

    rewind(left);
    rewind(right);
    do {
        left_byte = fgetc(left);
        right_byte = fgetc(right);
        if (left_byte != right_byte)
            return false;
    } while (left_byte != EOF);
    return !ferror(left) && !ferror(right);
}

static void test_checkpoint_restores_random_stream(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState continuous_state;
    KuhnPokerState split_state;
    KuhnPokerState restored_state;
    InfoStore continuous_store;
    InfoStore split_store;
    InfoStore restored_store = {0};
    Trainer continuous;
    Trainer split;
    Trainer restored = {0};
    FILE *intermediate = tmpfile();
    FILE *continuous_file = tmpfile();
    FILE *restored_file = tmpfile();

    CHECK(intermediate != NULL);
    CHECK(continuous_file != NULL);
    CHECK(restored_file != NULL);
    if (intermediate == NULL || continuous_file == NULL ||
        restored_file == NULL) {
        if (intermediate != NULL)
            (void)fclose(intermediate);
        if (continuous_file != NULL)
            (void)fclose(continuous_file);
        if (restored_file != NULL)
            (void)fclose(restored_file);
        return;
    }

    CHECK(cfr_kuhn_poker_state_init(&continuous_state) == CFR_STATUS_SUCCESS);
    CHECK(cfr_kuhn_poker_state_init(&split_state) == CFR_STATUS_SUCCESS);
    CHECK(cfr_kuhn_poker_state_init(&restored_state) == CFR_STATUS_SUCCESS);
    initialize_store(&continuous_store);
    initialize_store(&split_store);
    CHECK(cfr_trainer_init_mccfr(
              &continuous, game,
              cfr_kuhn_poker_state_as_game_state(&continuous_state),
              &continuous_store, 456) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init_mccfr(
              &split, game, cfr_kuhn_poker_state_as_game_state(&split_state),
              &split_store, 456) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&continuous, 100) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&split, 40) == CFR_STATUS_SUCCESS);
    CHECK(cfr_checkpoint_write(intermediate, &split) == CFR_STATUS_SUCCESS);
    CHECK(fflush(intermediate) == 0);
    rewind(intermediate);
    CHECK(cfr_checkpoint_read(
              intermediate, game,
              cfr_kuhn_poker_state_as_game_state(&restored_state),
              &restored_store, &restored) == CFR_STATUS_SUCCESS);
    CHECK(restored.variant == CFR_TRAINER_VARIANT_MCCFR_EXTERNAL);
    CHECK(restored.mccfr_rng.state == split.mccfr_rng.state);
    CHECK(cfr_trainer_run(&restored, 60) == CFR_STATUS_SUCCESS);
    CHECK(cfr_checkpoint_write(continuous_file, &continuous) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_checkpoint_write(restored_file, &restored) == CFR_STATUS_SUCCESS);
    CHECK(fflush(continuous_file) == 0);
    CHECK(fflush(restored_file) == 0);
    CHECK(files_equal(continuous_file, restored_file));

    destroy_store(&continuous_store);
    destroy_store(&split_store);
    destroy_store(&restored_store);
    CHECK(fclose(intermediate) == 0);
    CHECK(fclose(continuous_file) == 0);
    CHECK(fclose(restored_file) == 0);
}

#ifdef CFR_TEST_WRAP_ALLOCATOR
static void test_allocation_failures_are_transactional(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    bool reached_success = false;

    for (size_t failure_index = 0; failure_index < 128; failure_index += 1) {
        KuhnPokerState state;
        KuhnPokerState root;
        InfoStore store;
        MccfrRng rng;
        Utility utility = 101.0;
        TraversalStats stats = {.visited_nodes = 102};

        CHECK(cfr_kuhn_poker_state_init(&state) == CFR_STATUS_SUCCESS);
        root = state;
        initialize_store(&store);
        CHECK(cfr_mccfr_rng_seed(&rng, 103) == CFR_STATUS_SUCCESS);
        test_allocator_fail_after(failure_index);
        const Status status = cfr_mccfr_external_traverse_with_stats(
            game, cfr_kuhn_poker_state_as_game_state(&state), &store,
            CFR_PLAYER_0, &rng, &utility, &stats);
        test_allocator_disable_failures();

        CHECK(memcmp(&state, &root, sizeof(state)) == 0);
        if (status == CFR_STATUS_SUCCESS) {
            reached_success = true;
        } else {
            CHECK(status == CFR_STATUS_OUT_OF_MEMORY);
            CHECK(rng.state == 103);
            CHECK(utility == 101.0);
            CHECK(stats.visited_nodes == 102);
        }
        destroy_store(&store);
        CHECK(test_allocator_live_allocations() == 0);
        if (reached_success)
            break;
    }
    CHECK(reached_success);
}
#endif

int test_mccfr(void) {
    failures = 0;

    test_rng_contract();
    test_chance_is_sampled_and_target_actions_are_expanded();
    test_opponent_sample_is_shared_by_information_set();
    test_error_preserves_rng_outputs_and_learning();
    test_hidden_histories_require_identical_action_mapping();
    test_seeded_trainers_are_reproducible();
    test_sampled_player_average_matches_exact_cfr();
    test_single_strategic_player_accumulates_average();
    test_kuhn_converges();
    test_checkpoint_restores_random_stream();
#ifdef CFR_TEST_WRAP_ALLOCATOR
    test_allocation_failures_are_transactional();
#endif

    return failures;
}

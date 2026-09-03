#include <float.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "cfr/blackjack.h"
#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/kuhn_poker.h"
#include "cfr/leduc_poker.h"
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

enum {
    MCCFR_REACH_CHAIN_ACTION_COUNT = 3,
    MCCFR_REACH_CHAIN_DEPTH = 430,
    MCCFR_REACH_CHAIN_KEY_BASE = 10000
};

static const Probability MCCFR_REACH_CHAIN_RARE_PROBABILITY = 0x1p-40;

typedef struct {
    size_t depth;
} MccfrReachChainState;

static const MccfrReachChainState *reach_chain_const(const GameState *state) {
    return (const MccfrReachChainState *)state;
}

static MccfrReachChainState *reach_chain(GameState *state) {
    return (MccfrReachChainState *)state;
}

static Status reach_chain_is_terminal(const void *context,
                                      const GameState *state, bool *result) {
    const MccfrReachChainState *chain = reach_chain_const(state);

    (void)context;
    if (chain == NULL || result == NULL ||
        chain->depth > MCCFR_REACH_CHAIN_DEPTH) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    *result = chain->depth == MCCFR_REACH_CHAIN_DEPTH;
    return CFR_STATUS_SUCCESS;
}

static Status reach_chain_terminal_utility(const void *context,
                                           const GameState *state,
                                           Player player, Utility *result) {
    const MccfrReachChainState *chain = reach_chain_const(state);

    (void)context;
    if (chain == NULL || result == NULL ||
        chain->depth != MCCFR_REACH_CHAIN_DEPTH ||
        (player != CFR_PLAYER_0 && player != CFR_PLAYER_1)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    *result = 0.0;
    return CFR_STATUS_SUCCESS;
}

static Status reach_chain_current_actor(const void *context,
                                        const GameState *state,
                                        Actor *result) {
    const MccfrReachChainState *chain = reach_chain_const(state);

    (void)context;
    if (chain == NULL || result == NULL ||
        chain->depth >= MCCFR_REACH_CHAIN_DEPTH) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    result->kind = CFR_ACTOR_PLAYER;
    result->player = CFR_PLAYER_1;
    return CFR_STATUS_SUCCESS;
}

static Status reach_chain_legal_actions(const void *context,
                                        const GameState *state,
                                        Action *actions, size_t capacity,
                                        size_t *required_count) {
    const MccfrReachChainState *chain = reach_chain_const(state);

    (void)context;
    if (chain == NULL || actions == NULL || required_count == NULL ||
        chain->depth >= MCCFR_REACH_CHAIN_DEPTH) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    *required_count = MCCFR_REACH_CHAIN_ACTION_COUNT;
    if (capacity < MCCFR_REACH_CHAIN_ACTION_COUNT)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    for (size_t action = 0; action < MCCFR_REACH_CHAIN_ACTION_COUNT;
         action += 1) {
        actions[action] = (Action)action;
    }
    return CFR_STATUS_SUCCESS;
}

static Status reach_chain_apply_action(const void *context, GameState *state,
                                       Action action) {
    MccfrReachChainState *chain = reach_chain(state);

    (void)context;
    if (chain == NULL || chain->depth >= MCCFR_REACH_CHAIN_DEPTH)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action < 0 || action >= MCCFR_REACH_CHAIN_ACTION_COUNT)
        return CFR_STATUS_ILLEGAL_ACTION;
    chain->depth += 1;
    return CFR_STATUS_SUCCESS;
}

static Status reach_chain_undo_action(const void *context, GameState *state) {
    MccfrReachChainState *chain = reach_chain(state);

    (void)context;
    if (chain == NULL || chain->depth == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    chain->depth -= 1;
    return CFR_STATUS_SUCCESS;
}

static Status reach_chain_chance_probability(const void *context,
                                             const GameState *state,
                                             Action action,
                                             Probability *result) {
    (void)context;
    (void)state;
    (void)action;
    (void)result;
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status reach_chain_information_set_key(const void *context,
                                              const GameState *state,
                                              InfoSetKey *result) {
    const MccfrReachChainState *chain = reach_chain_const(state);

    (void)context;
    if (chain == NULL || result == NULL ||
        chain->depth >= MCCFR_REACH_CHAIN_DEPTH) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    *result = MCCFR_REACH_CHAIN_KEY_BASE + (InfoSetKey)chain->depth;
    return CFR_STATUS_SUCCESS;
}

static const GameOperations MCCFR_REACH_CHAIN_OPERATIONS = {
    .is_terminal = reach_chain_is_terminal,
    .terminal_utility = reach_chain_terminal_utility,
    .current_actor = reach_chain_current_actor,
    .legal_actions = reach_chain_legal_actions,
    .apply_action = reach_chain_apply_action,
    .undo_action = reach_chain_undo_action,
    .chance_probability = reach_chain_chance_probability,
    .information_set_key = reach_chain_information_set_key,
};

static const Game MCCFR_REACH_CHAIN_GAME = {
    .operations = &MCCFR_REACH_CHAIN_OPERATIONS,
    .context = NULL,
    .strategic_player_count = 2,
    .max_legal_actions = MCCFR_REACH_CHAIN_ACTION_COUNT,
    .strategy_schema_id = "cfr.test.mccfr-reach-chain/v1",
};

static uint64_t reach_chain_rng_next(uint64_t *state) {
    uint64_t value;

    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void test_rng_contract(void) {
    MccfrRng rng = {.state = 91};

    CHECK(cfr_mccfr_rng_seed(NULL, 7) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_mccfr_rng_seed(&rng, 0) == CFR_STATUS_SUCCESS);
    CHECK(rng.state == 0);
    CHECK(cfr_mccfr_rng_seed(&rng, UINT64_MAX) == CFR_STATUS_SUCCESS);
    CHECK(rng.state == UINT64_MAX);
}

/*
 * SplitMix64 output one has zero in its upper 53 bits. The old binary64 draw
 * therefore rounded it down to zero and selected DBL_TRUE_MIN as though that
 * outcome had probability 2^-53. A 64-bit interval assigns only output zero to
 * the tiny outcome, so output one selects the other chance action.
 */
static void test_sampler_uses_all_rng_bits(void) {
    const uint64_t seed = UINT64_C(0xf8364607e9c949bd);
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState root;
    InfoStore store;
    MccfrRng rng;
    Utility utility = 103.0;

    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    chance_game_set_probabilities(&state, DBL_TRUE_MIN, 1.0);
    root = state;
    initialize_store(&store);
    CHECK(cfr_mccfr_rng_seed(&rng, seed) == CFR_STATUS_SUCCESS);

    CHECK(cfr_mccfr_external_traverse(
              game, chance_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &rng, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, 1.0));
    CHECK(chance_game_state_equal(&state, &root));
    CHECK(rng.state == seed + UINT64_C(0x9e3779b97f4a7c15));
    destroy_store(&store);
}

/*
 * The former inverse-reach average multiplied every sampled probability into
 * one cumulative long double. This chain selects an approximately 2^-40 action
 * at every level, which forced that cumulative reach to zero and made retries
 * repeat the same numeric error. External sampling does not need that product.
 */
static void test_tiny_sample_reach_does_not_abort(void) {
    const uint64_t seed = UINT64_C(0x4d43434652);
    MccfrReachChainState state = {0};
    InfoStore store;
    MccfrRng rng;
    Utility utility = 101.0;
    TraversalStats stats = {.visited_nodes = 102};
    uint64_t expected_rng_state = seed;

    initialize_store(&store);
    for (size_t depth = 0; depth < MCCFR_REACH_CHAIN_DEPTH; depth += 1) {
        const double draw =
            (double)(reach_chain_rng_next(&expected_rng_state) >> 11) *
            0x1.0p-53;
        const Probability half_rare =
            MCCFR_REACH_CHAIN_RARE_PROBABILITY / 2.0;
        Probability lower;

        if (draw < half_rare) {
            lower = 0.0;
        } else if (draw > 1.0 - half_rare) {
            lower = 1.0 - MCCFR_REACH_CHAIN_RARE_PROBABILITY;
        } else {
            lower = draw - half_rare;
        }

        InfoNode *node = NULL;
        CHECK(cfr_info_store_get_or_create(
                  &store, MCCFR_REACH_CHAIN_KEY_BASE + (InfoSetKey)depth,
                  MCCFR_REACH_CHAIN_ACTION_COUNT, &node) ==
              CFR_STATUS_SUCCESS);
        CHECK(node != NULL);
        node->regret_sums[0] = lower;
        node->regret_sums[1] = MCCFR_REACH_CHAIN_RARE_PROBABILITY;
        node->regret_sums[2] =
            1.0 - (lower + MCCFR_REACH_CHAIN_RARE_PROBABILITY);

        Probability strategy[MCCFR_REACH_CHAIN_ACTION_COUNT];
        CHECK(cfr_info_node_current_strategy(
                  node, strategy, MCCFR_REACH_CHAIN_ACTION_COUNT) ==
              CFR_STATUS_SUCCESS);
        const double probability_sum =
            strategy[0] + strategy[1] + strategy[2];
        const double scaled_draw = draw * probability_sum;
        CHECK(scaled_draw >= strategy[0]);
        CHECK(scaled_draw < strategy[0] + strategy[1]);
    }

    CHECK(cfr_mccfr_rng_seed(&rng, seed) == CFR_STATUS_SUCCESS);
    CHECK(cfr_mccfr_external_traverse_with_stats(
              &MCCFR_REACH_CHAIN_GAME, (GameState *)&state, &store,
              CFR_PLAYER_0, &rng, &utility, &stats) == CFR_STATUS_SUCCESS);
    CHECK(state.depth == 0);
    CHECK(rng.state == expected_rng_state);
    CHECK(near(utility, 0.0));
    CHECK(stats.visited_nodes == MCCFR_REACH_CHAIN_DEPTH + 1);
    destroy_store(&store);
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

typedef struct {
    const GameOperations *base_operations;
    InfoStore *store;
    bool *mutated;
} StrategySnapshotMutation;

static Status mutate_strategy_before_second_hidden_history(
    const void *context, const GameState *state, Actor *result) {
    const StrategySnapshotMutation *mutation = context;
    const TraversalGameState *traversal_state =
        (const TraversalGameState *)state;

    if (traversal_state->phase ==
            TRAVERSAL_PHASE_SHARED_RIGHT_PLAYER_1 &&
        !*mutation->mutated) {
        InfoNode *node = NULL;
        const Utility regret_delta[2] = {-2.0, 2.0};
        const double strategy_delta[2] = {0.0, 0.0};
        Status status = cfr_info_store_find(mutation->store, 501, &node);

        if (status != CFR_STATUS_SUCCESS)
            return status;
        status = cfr_info_node_apply_deltas(
            node, regret_delta, strategy_delta, 2);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        *mutation->mutated = true;
    }
    return mutation->base_operations->current_actor(NULL, state, result);
}

/*
 * Simulate another worker replacing an opponent strategy between the two
 * hidden histories that share key 501. The traversal sampled action zero from
 * [1, 0]. It must keep both that action and its strategy snapshot after the
 * shared node changes to [0, 1]. Mixing the cached action with the later
 * distribution used to return CFR_STATUS_NUMERIC_ERROR.
 */
static void test_strategy_snapshot_survives_shared_node_update(void) {
    const Game *base_game = traversal_game_descriptor();
    GameOperations operations = *base_game->operations;
    Game game = *base_game;
    TraversalGameState state;
    TraversalGameState root;
    InfoStore store;
    InfoNode *opponent_node = NULL;
    MccfrRng rng;
    Utility utility = 93.0;
    TraversalStats stats = {.visited_nodes = 94};
    const uint64_t seed = UINT64_C(1234567);
    const Utility initial_regret[2] = {1.0, -1.0};
    const double initial_strategy[2] = {0.0, 0.0};
    bool mutated = false;
    StrategySnapshotMutation mutation = {
        .base_operations = base_game->operations,
        .store = &store,
        .mutated = &mutated,
    };

    operations.current_actor = mutate_strategy_before_second_hidden_history;
    game.operations = &operations;
    game.context = &mutation;
    CHECK(traversal_game_state_init_shared(&state, false) ==
          CFR_STATUS_SUCCESS);
    root = state;
    initialize_store(&store);
    CHECK(cfr_info_store_get_or_create(&store, 501, 2, &opponent_node) ==
          CFR_STATUS_SUCCESS);
    CHECK(opponent_node != NULL);
    CHECK(cfr_info_node_apply_deltas(
              opponent_node, initial_regret, initial_strategy, 2) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_mccfr_rng_seed(&rng, seed) == CFR_STATUS_SUCCESS);

    CHECK(cfr_mccfr_external_traverse_with_stats(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &rng, &utility, &stats) == CFR_STATUS_SUCCESS);
    CHECK(mutated);
    CHECK(near(utility, -2.0));
    CHECK(stats.visited_nodes == 5);
    CHECK(state.phase == root.phase);
    CHECK(state.history_count == root.history_count);
    CHECK(rng.state == seed + UINT64_C(0x9e3779b97f4a7c15));
    CHECK(near(opponent_node->regret_sums[0], -1.0));
    CHECK(near(opponent_node->regret_sums[1], 1.0));
    CHECK(near(opponent_node->strategy_sums[0], 2.0));
    CHECK(near(opponent_node->strategy_sums[1], 0.0));
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

enum {
    PARALLEL_MCCFR_MAX_THREAD_COUNT = 4,
    PARALLEL_MCCFR_ITERATIONS = 2000
};

static size_t parallel_mccfr_thread_count(void) {
#if defined(_SC_NPROCESSORS_ONLN)
    const long processor_count = sysconf(_SC_NPROCESSORS_ONLN);

    if (processor_count > 1 &&
        processor_count < PARALLEL_MCCFR_MAX_THREAD_COUNT) {
        return (size_t)processor_count;
    }
    if (processor_count >= PARALLEL_MCCFR_MAX_THREAD_COUNT)
        return PARALLEL_MCCFR_MAX_THREAD_COUNT;
#endif
    return 2;
}

typedef struct {
    atomic_size_t *ready_count;
    atomic_bool *start;
    Trainer trainer;
    KuhnPokerState state;
    Status status;
} ParallelMccfrWorker;

static void *run_parallel_mccfr_worker(void *raw_worker) {
    ParallelMccfrWorker *worker = raw_worker;

    atomic_fetch_add_explicit(worker->ready_count, 1, memory_order_release);
    while (!atomic_load_explicit(worker->start, memory_order_acquire))
        (void)sched_yield();
    worker->status = cfr_trainer_run_concurrent(
        &worker->trainer, PARALLEL_MCCFR_ITERATIONS);
    return NULL;
}

static void test_parallel_trainers_share_store(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    InfoStore store;
    ParallelMccfrWorker workers[PARALLEL_MCCFR_MAX_THREAD_COUNT];
    pthread_t threads[PARALLEL_MCCFR_MAX_THREAD_COUNT];
    atomic_size_t ready_count;
    atomic_bool start;
    const size_t thread_count = parallel_mccfr_thread_count();
    size_t created = 0;

    initialize_store(&store);
    atomic_init(&ready_count, 0);
    atomic_init(&start, false);
    for (size_t index = 0; index < thread_count; index += 1) {
        workers[index] = (ParallelMccfrWorker){
            .ready_count = &ready_count,
            .start = &start,
            .status = CFR_STATUS_INVALID_ARGUMENT,
        };
        CHECK(cfr_kuhn_poker_state_init(&workers[index].state) ==
              CFR_STATUS_SUCCESS);
        CHECK(cfr_trainer_init_mccfr(
                  &workers[index].trainer, game,
                  cfr_kuhn_poker_state_as_game_state(&workers[index].state),
                  &store, UINT64_C(1000) + index) == CFR_STATUS_SUCCESS);
        if (pthread_create(&threads[index], NULL, run_parallel_mccfr_worker,
                           &workers[index]) != 0) {
            CHECK(false);
            break;
        }
        created += 1;
    }
    while (atomic_load_explicit(&ready_count, memory_order_acquire) < created) {
        (void)sched_yield();
    }
    atomic_store_explicit(&start, true, memory_order_release);
    for (size_t index = 0; index < created; index += 1) {
        CHECK(pthread_join(threads[index], NULL) == 0);
        CHECK(workers[index].status == CFR_STATUS_SUCCESS);
        CHECK(workers[index].trainer.stats.iterations ==
              PARALLEL_MCCFR_ITERATIONS);
        CHECK(workers[index].trainer.stats.traversals ==
              2 * PARALLEL_MCCFR_ITERATIONS);
    }
    CHECK(created == thread_count);

    InfoStoreStats store_stats = {0};
    CHECK(cfr_info_store_get_stats(&store, &store_stats) ==
          CFR_STATUS_SUCCESS);
    CHECK(store_stats.size == 12);
    for (InfoSetKey key = 0; key < 21; key += 1) {
        const InfoNode *node = NULL;
        Probability strategy[2] = {0.0, 0.0};
        const Status find_status =
            cfr_info_store_find_const(&store, key, &node);

        if (find_status == CFR_STATUS_NOT_FOUND)
            continue;
        CHECK(find_status == CFR_STATUS_SUCCESS);
        CHECK(node != NULL);
        CHECK(cfr_info_node_average_strategy(node, strategy, 2) ==
              CFR_STATUS_SUCCESS);
        CHECK(isfinite(strategy[0]));
        CHECK(isfinite(strategy[1]));
        CHECK(near(strategy[0] + strategy[1], 1.0));
    }
    destroy_store(&store);
}

enum {
    PARALLEL_EXACT_ITERATIONS = 3000,
    CONCURRENT_CHECKPOINT_CHUNK_ITERATIONS = 64,
    CONCURRENT_CHECKPOINT_ITERATION_LIMIT = 512
};

typedef struct {
    atomic_size_t *ready_count;
    atomic_bool *start;
    atomic_bool *stop;
    atomic_bool *done;
    atomic_size_t *completed_iterations;
    Trainer trainer;
    ChanceGameState state;
    size_t iterations;
    size_t iteration_limit;
    Status status;
} ParallelChanceWorker;

static void *run_parallel_chance_worker(void *raw_worker) {
    ParallelChanceWorker *worker = raw_worker;
    size_t completed = 0;

    atomic_fetch_add_explicit(worker->ready_count, 1, memory_order_release);
    while (!atomic_load_explicit(worker->start, memory_order_acquire))
        (void)sched_yield();
    do {
        worker->status = cfr_trainer_run_concurrent(
            &worker->trainer, worker->iterations);
        if (worker->status != CFR_STATUS_SUCCESS)
            break;
        if (worker->completed_iterations != NULL) {
            atomic_fetch_add_explicit(worker->completed_iterations,
                                      worker->iterations,
                                      memory_order_release);
        }
        completed += worker->iterations;
        if (worker->iteration_limit != 0 &&
            completed >= worker->iteration_limit) {
            break;
        }
    } while (worker->stop != NULL &&
             !atomic_load_explicit(worker->stop, memory_order_acquire));
    if (worker->done != NULL)
        atomic_store_explicit(worker->done, true, memory_order_release);
    return NULL;
}

static void test_parallel_mccfr_updates_are_exact(void) {
    Game game = *chance_game_descriptor();
    InfoStore store;
    ParallelChanceWorker workers[PARALLEL_MCCFR_MAX_THREAD_COUNT];
    pthread_t threads[PARALLEL_MCCFR_MAX_THREAD_COUNT];
    atomic_size_t ready_count;
    atomic_bool start;
    const size_t thread_count = parallel_mccfr_thread_count();
    size_t created = 0;

    game.strategic_player_count = 1;
    initialize_store(&store);
    atomic_init(&ready_count, 0);
    atomic_init(&start, false);
    for (size_t index = 0; index < thread_count; index += 1) {
        workers[index] = (ParallelChanceWorker){
            .ready_count = &ready_count,
            .start = &start,
            .iterations = PARALLEL_EXACT_ITERATIONS,
            .status = CFR_STATUS_INVALID_ARGUMENT,
        };
        CHECK(chance_game_state_init_coin(&workers[index].state) ==
              CFR_STATUS_SUCCESS);
        chance_game_set_probabilities(&workers[index].state, 0.0, 1.0);
        CHECK(cfr_trainer_init_mccfr(
                  &workers[index].trainer, &game,
                  chance_game_state_as_public(&workers[index].state), &store,
                  UINT64_C(2000) + index) == CFR_STATUS_SUCCESS);
        if (pthread_create(&threads[index], NULL, run_parallel_chance_worker,
                           &workers[index]) != 0) {
            CHECK(false);
            break;
        }
        created += 1;
    }
    while (atomic_load_explicit(&ready_count, memory_order_acquire) < created)
        (void)sched_yield();
    atomic_store_explicit(&start, true, memory_order_release);
    for (size_t index = 0; index < created; index += 1) {
        CHECK(pthread_join(threads[index], NULL) == 0);
        CHECK(workers[index].status == CFR_STATUS_SUCCESS);
        CHECK(workers[index].trainer.stats.iterations ==
              PARALLEL_EXACT_ITERATIONS);
        CHECK(workers[index].trainer.stats.traversals ==
              PARALLEL_EXACT_ITERATIONS);
    }
    CHECK(created == thread_count);
    if (created == thread_count) {
        InfoNode *node = find_node(&store, 800);
        const double expected =
            (double)(thread_count * PARALLEL_EXACT_ITERATIONS);
        const double accumulated =
            node->strategy_sums[0] + node->strategy_sums[1];

        CHECK(fabs(accumulated - expected) <= expected * 1e-12);
    }
    destroy_store(&store);
}

static void test_checkpoint_is_loadable_during_parallel_training(void) {
    Game game = *chance_game_descriptor();
    ChanceGameState warmup_state;
    ChanceGameState checkpoint_state;
    ChanceGameState restored_state;
    InfoStore store;
    Trainer warmup;
    Trainer checkpoint_trainer;
    ParallelChanceWorker worker;
    pthread_t thread;
    atomic_size_t ready_count;
    atomic_size_t completed_iterations;
    atomic_bool start;
    atomic_bool stop;
    atomic_bool done;
    size_t checkpoint_count = 0;
    bool created = false;

    game.strategic_player_count = 1;
    initialize_store(&store);
    CHECK(chance_game_state_init_coin(&warmup_state) == CFR_STATUS_SUCCESS);
    CHECK(chance_game_state_init_coin(&checkpoint_state) ==
          CFR_STATUS_SUCCESS);
    chance_game_set_probabilities(&warmup_state, 0.0, 1.0);
    chance_game_set_probabilities(&checkpoint_state, 0.0, 1.0);
    CHECK(cfr_trainer_init_mccfr(
              &warmup, &game, chance_game_state_as_public(&warmup_state),
              &store, UINT64_C(3000)) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&warmup, 1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_init_mccfr(
              &checkpoint_trainer, &game,
              chance_game_state_as_public(&checkpoint_state), &store,
              UINT64_C(3001)) == CFR_STATUS_SUCCESS);
    atomic_init(&ready_count, 0);
    atomic_init(&completed_iterations, 0);
    atomic_init(&start, false);
    atomic_init(&stop, false);
    atomic_init(&done, false);
    worker = (ParallelChanceWorker){
        .ready_count = &ready_count,
        .start = &start,
        .stop = &stop,
        .done = &done,
        .completed_iterations = &completed_iterations,
        .iterations = CONCURRENT_CHECKPOINT_CHUNK_ITERATIONS,
        .iteration_limit = CONCURRENT_CHECKPOINT_ITERATION_LIMIT,
        .status = CFR_STATUS_INVALID_ARGUMENT,
    };
    CHECK(chance_game_state_init_coin(&worker.state) == CFR_STATUS_SUCCESS);
    chance_game_set_probabilities(&worker.state, 0.0, 1.0);
    CHECK(cfr_trainer_init_mccfr(
              &worker.trainer, &game,
              chance_game_state_as_public(&worker.state), &store,
              UINT64_C(3002)) == CFR_STATUS_SUCCESS);
    if (pthread_create(&thread, NULL, run_parallel_chance_worker, &worker) ==
        0) {
        created = true;
    } else {
        CHECK(false);
    }
    if (created) {
        while (atomic_load_explicit(&ready_count, memory_order_acquire) == 0)
            (void)sched_yield();
        atomic_store_explicit(&start, true, memory_order_release);
        while (atomic_load_explicit(&completed_iterations,
                                    memory_order_acquire) == 0) {
            (void)sched_yield();
        }
        while (checkpoint_count < 4) {
            InfoStore restored_store = {0};
            Trainer restored = {0};
            FILE *stream = tmpfile();

            CHECK(stream != NULL);
            if (stream == NULL)
                break;
            CHECK(cfr_checkpoint_write(stream, &checkpoint_trainer) ==
                  CFR_STATUS_SUCCESS);
            CHECK(fflush(stream) == 0);
            rewind(stream);
            CHECK(chance_game_state_init_coin(&restored_state) ==
                  CFR_STATUS_SUCCESS);
            chance_game_set_probabilities(&restored_state, 0.0, 1.0);
            CHECK(cfr_checkpoint_read(
                      stream, &game,
                      chance_game_state_as_public(&restored_state),
                      &restored_store, &restored) == CFR_STATUS_SUCCESS);
            CHECK(cfr_info_store_destroy(&restored_store) ==
                  CFR_STATUS_SUCCESS);
            CHECK(fclose(stream) == 0);
            checkpoint_count += 1;
        }
        atomic_store_explicit(&stop, true, memory_order_release);
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK(worker.status == CFR_STATUS_SUCCESS);
        CHECK(atomic_load_explicit(&done, memory_order_acquire));
    }
    CHECK(checkpoint_count > 0);
    destroy_store(&store);
}

static void test_leduc_converges_across_seeds(void) {
    static const uint64_t seeds[] = {UINT64_C(0), UINT64_C(1), UINT64_C(2)};
    const Game *game = cfr_leduc_poker_descriptor();

    for (size_t index = 0; index < sizeof(seeds) / sizeof(seeds[0]);
         index += 1) {
        LeducPokerState state;
        InfoStore store;
        Trainer trainer;
        EvaluationMetrics metrics;

        CHECK(cfr_leduc_poker_state_init(&state) == CFR_STATUS_SUCCESS);
        initialize_store(&store);
        CHECK(cfr_trainer_init_mccfr(
                  &trainer, game,
                  cfr_leduc_poker_state_as_game_state(&state), &store,
                  seeds[index]) == CFR_STATUS_SUCCESS);
        CHECK(cfr_trainer_run(&trainer, 50000) == CFR_STATUS_SUCCESS);
        CHECK(cfr_evaluation_metrics(
                  game, cfr_leduc_poker_state_as_game_state(&state), &store,
                  &metrics) == CFR_STATUS_SUCCESS);
        CHECK(store.size == 288);
        CHECK(metrics.exploitability < 0.09);
        destroy_store(&store);
    }
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
static void test_trainer_reuses_workspace_across_traversals(void) {
    static const InfoSetKey keys[] = {0,  1,  2,  9,  10, 11,
                                      15, 16, 17, 18, 19, 20};
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState state;
    InfoStore store;
    Trainer trainer;
    size_t live_before;

    CHECK(cfr_kuhn_poker_state_init(&state) == CFR_STATUS_SUCCESS);
    initialize_store(&store);
    for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]);
         index += 1) {
        InfoNode *node = NULL;

        CHECK(cfr_info_store_get_or_create(&store, keys[index], 2, &node) ==
              CFR_STATUS_SUCCESS);
        CHECK(node != NULL);
    }
    CHECK(cfr_trainer_init_mccfr(
              &trainer, game, cfr_kuhn_poker_state_as_game_state(&state),
              &store, 107) == CFR_STATUS_SUCCESS);

    live_before = test_allocator_live_allocations();
    test_allocator_fail_after(6);
    CHECK(cfr_trainer_run(&trainer, 2) == CFR_STATUS_SUCCESS);
    test_allocator_disable_failures();
    CHECK(test_allocator_live_allocations() == live_before);
    CHECK(trainer.stats.iterations == 2);
    CHECK(trainer.stats.traversals == 4);

    destroy_store(&store);
    CHECK(test_allocator_live_allocations() == 0);
}

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
    test_sampler_uses_all_rng_bits();
    test_tiny_sample_reach_does_not_abort();
    test_chance_is_sampled_and_target_actions_are_expanded();
    test_opponent_sample_is_shared_by_information_set();
    test_strategy_snapshot_survives_shared_node_update();
    test_error_preserves_rng_outputs_and_learning();
    test_hidden_histories_require_identical_action_mapping();
    test_seeded_trainers_are_reproducible();
    test_sampled_player_average_matches_exact_cfr();
    test_single_strategic_player_accumulates_average();
    test_kuhn_converges();
    test_parallel_trainers_share_store();
    test_parallel_mccfr_updates_are_exact();
    test_checkpoint_is_loadable_during_parallel_training();
    test_leduc_converges_across_seeds();
    test_checkpoint_restores_random_stream();
#ifdef CFR_TEST_WRAP_ALLOCATOR
    test_trainer_reuses_workspace_across_traversals();
    test_allocation_failures_are_transactional();
#endif

    return failures;
}

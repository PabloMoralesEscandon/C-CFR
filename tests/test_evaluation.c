#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfr/evaluation.h"
#include "cfr/kuhn_poker.h"
#include "cfr/trainer.h"
#include "support/chance_game.h"
#include "support/test_allocator.h"
#include "support/traversal_game.h"
#include "test_suite.h"

enum {
    TEST_MAX_ACTIONS = 64,
    SNAPSHOT_MAX_NODES = 16,
    SNAPSHOT_MAX_ACTIONS = 6
};

static int failures;

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            failures += 1;                                                     \
        }                                                                      \
    } while (0)

typedef struct {
    InfoSetKey key;
    const InfoNode *node;
    size_t action_count;
    Utility regret_sums[SNAPSHOT_MAX_ACTIONS];
    double strategy_sums[SNAPSHOT_MAX_ACTIONS];
} NodeSnapshot;

typedef struct {
    InfoStoreStats stats;
    size_t node_count;
    NodeSnapshot nodes[SNAPSHOT_MAX_NODES];
} StoreSnapshot;

typedef struct {
    Utility player_1_utility;
} TerminalUtilityOverride;

typedef struct {
    Utility player_0_utility;
} ConstantTerminalUtility;

static bool near(double left, double right, double tolerance) {
    return fabs(left - right) <= tolerance;
}

static void initialize_store(InfoStore *store) {
    *store = (InfoStore){0};
    CHECK(cfr_info_store_init(store) == CFR_STATUS_SUCCESS);
}

static void destroy_store(InfoStore *store) {
    CHECK(cfr_info_store_destroy(store) == CFR_STATUS_SUCCESS);
}

static InfoNode *create_policy(InfoStore *store, InfoSetKey key,
                               double first_strategy_sum,
                               double second_strategy_sum) {
    InfoNode *node = NULL;

    CHECK(cfr_info_store_get_or_create(store, key, 2, &node) ==
          CFR_STATUS_SUCCESS);
    if (node != NULL) {
        node->strategy_sums[0] = first_strategy_sum;
        node->strategy_sums[1] = second_strategy_sum;
    }
    return node;
}

static InfoStoreStats get_store_stats(const InfoStore *store) {
    InfoStoreStats stats = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};

    CHECK(cfr_info_store_get_stats(store, &stats) == CFR_STATUS_SUCCESS);
    return stats;
}

static bool same_store_stats(const InfoStoreStats *left,
                             const InfoStoreStats *right) {
    return left->size == right->size && left->capacity == right->capacity &&
           left->collision_count == right->collision_count &&
           left->growth_count == right->growth_count;
}

static bool capture_store(const InfoStore *store, const InfoSetKey *keys,
                          size_t key_count, StoreSnapshot *snapshot) {
    size_t key_index;

    if (store == NULL || keys == NULL || snapshot == NULL ||
        key_count > SNAPSHOT_MAX_NODES)
        return false;
    *snapshot = (StoreSnapshot){0};
    snapshot->stats = get_store_stats(store);
    snapshot->node_count = key_count;
    for (key_index = 0; key_index < key_count; key_index += 1) {
        const InfoNode *node = NULL;
        size_t action_index;

        if (cfr_info_store_find_const(store, keys[key_index], &node) !=
                CFR_STATUS_SUCCESS ||
            node == NULL || node->action_count > SNAPSHOT_MAX_ACTIONS) {
            CHECK(false);
            return false;
        }
        snapshot->nodes[key_index].key = keys[key_index];
        snapshot->nodes[key_index].node = node;
        snapshot->nodes[key_index].action_count = node->action_count;
        for (action_index = 0; action_index < node->action_count;
             action_index += 1) {
            snapshot->nodes[key_index].regret_sums[action_index] =
                node->regret_sums[action_index];
            snapshot->nodes[key_index].strategy_sums[action_index] =
                node->strategy_sums[action_index];
        }
    }
    return true;
}

static bool store_matches_snapshot(const InfoStore *store,
                                   const StoreSnapshot *snapshot) {
    InfoStoreStats current_stats;
    size_t node_index;

    if (store == NULL || snapshot == NULL)
        return false;
    current_stats = get_store_stats(store);
    if (!same_store_stats(&current_stats, &snapshot->stats))
        return false;
    for (node_index = 0; node_index < snapshot->node_count; node_index += 1) {
        const NodeSnapshot *expected = &snapshot->nodes[node_index];
        const InfoNode *node = NULL;
        size_t action_index;

        if (cfr_info_store_find_const(store, expected->key, &node) !=
                CFR_STATUS_SUCCESS ||
            node == NULL || node != expected->node ||
            node->action_count != expected->action_count) {
            return false;
        }
        for (action_index = 0; action_index < node->action_count;
             action_index += 1) {
            if (node->regret_sums[action_index] !=
                    expected->regret_sums[action_index] ||
                node->strategy_sums[action_index] !=
                    expected->strategy_sums[action_index]) {
                return false;
            }
        }
    }
    return true;
}

static bool same_traversal_state(const TraversalGameState *left,
                                 const TraversalGameState *right) {
    size_t index;

    if (left->phase != right->phase ||
        left->terminal_utility_player_0 != right->terminal_utility_player_0 ||
        left->last_action != right->last_action ||
        left->history_count != right->history_count ||
        left->reverse_shared_root_actions !=
            right->reverse_shared_root_actions ||
        left->fail_after_any_action != right->fail_after_any_action ||
        left->fail_after_selected_action != right->fail_after_selected_action ||
        left->selected_failure_action != right->selected_failure_action ||
        left->failure_after_apply != right->failure_after_apply ||
        left->undo_failure != right->undo_failure ||
        left->force_required_count != right->force_required_count ||
        left->forced_required_count != right->forced_required_count) {
        return false;
    }
    for (index = 0; index < left->history_count; index += 1) {
        if (left->history[index].phase != right->history[index].phase ||
            left->history[index].terminal_utility_player_0 !=
                right->history[index].terminal_utility_player_0 ||
            left->history[index].last_action !=
                right->history[index].last_action) {
            return false;
        }
    }
    return true;
}

static EvaluationMetrics sentinel_metrics(void) {
    return (EvaluationMetrics){
        .profile_value_player_0 = 11.0,
        .profile_value_player_1 = 12.0,
        .best_response_value_player_0 = 13.0,
        .best_response_value_player_1 = 14.0,
        .improvement_player_0 = 15.0,
        .improvement_player_1 = 16.0,
        .nash_conv = 17.0,
        .exploitability = 18.0,
    };
}

static bool same_metrics(const EvaluationMetrics *left,
                         const EvaluationMetrics *right) {
    return left->profile_value_player_0 == right->profile_value_player_0 &&
           left->profile_value_player_1 == right->profile_value_player_1 &&
           left->best_response_value_player_0 ==
               right->best_response_value_player_0 &&
           left->best_response_value_player_1 ==
               right->best_response_value_player_1 &&
           left->improvement_player_0 == right->improvement_player_0 &&
           left->improvement_player_1 == right->improvement_player_1 &&
           left->nash_conv == right->nash_conv &&
           left->exploitability == right->exploitability;
}

static bool key_is_present(const InfoSetKey *keys, size_t key_count,
                           InfoSetKey key) {
    size_t index;

    for (index = 0; index < key_count; index += 1) {
        if (keys[index] == key)
            return true;
    }
    return false;
}

static Status populate_uniform_store(const Game *game, GameState *state,
                                     InfoStore *store, InfoSetKey *keys,
                                     size_t key_capacity, size_t *key_count) {
    bool terminal;
    Actor actor;
    Action actions[TEST_MAX_ACTIONS];
    size_t action_count;
    size_t action_index;
    Status status;

    if (game == NULL || state == NULL || store == NULL || keys == NULL ||
        key_count == NULL || game->max_legal_actions == 0 ||
        game->max_legal_actions > ARRAY_COUNT(actions)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    status = cfr_game_is_terminal(game, state, &terminal);
    if (status != CFR_STATUS_SUCCESS || terminal)
        return status;
    status = cfr_game_current_actor(game, state, &actor);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    status = cfr_game_legal_actions(game, state, actions,
                                    game->max_legal_actions, &action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (action_count == 0 || action_count > game->max_legal_actions)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (actor.kind == CFR_ACTOR_PLAYER) {
        InfoSetKey key;
        const InfoNode *existing = NULL;
        InfoNode *created = NULL;

        status = cfr_game_information_set_key(game, state, &key);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        status = cfr_info_store_find_const(store, key, &existing);
        if (status == CFR_STATUS_NOT_FOUND) {
            status = cfr_info_store_get_or_create(store, key, action_count,
                                                  &created);
            if (status != CFR_STATUS_SUCCESS)
                return status;
        } else if (status != CFR_STATUS_SUCCESS || existing == NULL ||
                   existing->action_count != action_count) {
            return status == CFR_STATUS_SUCCESS ? CFR_STATUS_INVALID_ARGUMENT
                                                : status;
        }
        if (!key_is_present(keys, *key_count, key)) {
            if (*key_count >= key_capacity)
                return CFR_STATUS_BUFFER_TOO_SMALL;
            keys[*key_count] = key;
            *key_count += 1;
        }
    } else if (actor.kind != CFR_ACTOR_CHANCE) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    for (action_index = 0; action_index < action_count; action_index += 1) {
        Status child_status;
        Status undo_status;

        status = cfr_game_apply_action(game, state, actions[action_index]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        child_status = populate_uniform_store(game, state, store, keys,
                                              key_capacity, key_count);
        undo_status = cfr_game_undo_action(game, state);
        if (undo_status != CFR_STATUS_SUCCESS)
            return undo_status;
        if (child_status != CFR_STATUS_SUCCESS)
            return child_status;
    }
    return CFR_STATUS_SUCCESS;
}

static Status reversed_shared_actions(const void *context,
                                      const GameState *state, Action *actions,
                                      size_t capacity, size_t *required_count) {
    const TraversalGameState *traversal_state =
        (const TraversalGameState *)state;
    Status status = traversal_game_descriptor()->operations->legal_actions(
        context, state, actions, capacity, required_count);

    if (status == CFR_STATUS_SUCCESS &&
        traversal_state->phase == TRAVERSAL_PHASE_SHARED_RIGHT_PLAYER_1 &&
        *required_count == 2) {
        Action temporary = actions[0];
        actions[0] = actions[1];
        actions[1] = temporary;
    }
    return status;
}

static Status invalid_actor(const void *context, const GameState *state,
                            Actor *result) {
    Status status = traversal_game_descriptor()->operations->current_actor(
        context, state, result);

    if (status == CFR_STATUS_SUCCESS)
        result->kind = (ActorKind)99;
    return status;
}

static Status invalid_player_actor(const void *context, const GameState *state,
                                   Actor *result) {
    Status status = traversal_game_descriptor()->operations->current_actor(
        context, state, result);

    if (status == CFR_STATUS_SUCCESS && result->kind == CFR_ACTOR_PLAYER)
        result->player = (Player)99;
    return status;
}

static Status circular_information_key(const void *context,
                                       const GameState *state,
                                       InfoSetKey *result) {
    const TraversalGameState *traversal_state =
        (const TraversalGameState *)state;

    if (traversal_state->phase == TRAVERSAL_PHASE_REACH_ROOT_PLAYER_1 ||
        traversal_state->phase == TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1) {
        *result = 300;
        return CFR_STATUS_SUCCESS;
    }
    return traversal_game_descriptor()->operations->information_set_key(
        context, state, result);
}

static Status circular_legal_actions(const void *context,
                                     const GameState *state, Action *actions,
                                     size_t capacity, size_t *required_count) {
    const TraversalGameState *traversal_state =
        (const TraversalGameState *)state;
    Status status = traversal_game_descriptor()->operations->legal_actions(
        context, state, actions, capacity, required_count);

    if (status == CFR_STATUS_SUCCESS &&
        traversal_state->phase == TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1 &&
        *required_count == 2) {
        actions[0] = TRAVERSAL_ACTION_STOP;
        actions[1] = TRAVERSAL_ACTION_CONTINUE;
    }
    return status;
}

static Status circular_apply_action(const void *context, GameState *state,
                                    Action action) {
    const TraversalGameState *traversal_state =
        (const TraversalGameState *)state;

    if (traversal_state->phase == TRAVERSAL_PHASE_REACH_SECOND_PLAYER_1) {
        if (action == TRAVERSAL_ACTION_STOP)
            action = TRAVERSAL_ACTION_BAD;
        else if (action == TRAVERSAL_ACTION_CONTINUE)
            action = TRAVERSAL_ACTION_GOOD;
    }
    return traversal_game_descriptor()->operations->apply_action(context, state,
                                                                 action);
}

static Status overridden_terminal_utility(const void *context,
                                          const GameState *state, Player player,
                                          Utility *result) {
    const TerminalUtilityOverride *override =
        (const TerminalUtilityOverride *)context;

    if (player == CFR_PLAYER_1) {
        *result = override->player_1_utility;
        return CFR_STATUS_SUCCESS;
    }
    return traversal_game_descriptor()->operations->terminal_utility(
        context, state, player, result);
}

static Status constant_terminal_utility(const void *context,
                                        const GameState *state, Player player,
                                        Utility *result) {
    const ConstantTerminalUtility *utility =
        (const ConstantTerminalUtility *)context;
    const TraversalGameState *traversal_state =
        (const TraversalGameState *)state;

    if (traversal_state->phase != TRAVERSAL_PHASE_TERMINAL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (player == CFR_PLAYER_0) {
        *result = utility->player_0_utility;
        return CFR_STATUS_SUCCESS;
    }
    if (player == CFR_PLAYER_1) {
        *result = -utility->player_0_utility;
        return CFR_STATUS_SUCCESS;
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static void test_const_lookup_and_average_strategy(void) {
    InfoStore store;
    InfoNode *node_0;
    InfoNode *node_5;
    InfoNode *node_13;
    const InfoNode *found = (const InfoNode *)(uintptr_t)1;
    InfoStoreStats before;
    InfoStoreStats after;
    Probability strategy[2] = {91.0, 92.0};
    size_t required_count = 93;

    initialize_store(&store);
    node_0 = create_policy(&store, 0, 1.0, 1.0);
    node_5 = create_policy(&store, 5, 1.0, 1.0);
    node_13 = create_policy(&store, 13, 3.0, 1.0);
    CHECK(node_0 != NULL && node_5 != NULL && node_13 != NULL);
    node_13->regret_sums[0] = -10.0;
    node_13->regret_sums[1] = 10.0;
    before = get_store_stats(&store);

    CHECK(cfr_info_store_find_const(&store, 13, &found) == CFR_STATUS_SUCCESS);
    CHECK(found == node_13);
    found = (const InfoNode *)(uintptr_t)1;
    CHECK(cfr_info_store_find_const(&store, 18, &found) ==
          CFR_STATUS_NOT_FOUND);
    CHECK(found == NULL);
    after = get_store_stats(&store);
    CHECK(same_store_stats(&before, &after));

    found = (const InfoNode *)(uintptr_t)1;
    CHECK(cfr_info_store_find_const(NULL, 13, &found) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(found == (const InfoNode *)(uintptr_t)1);
    CHECK(cfr_info_store_find_const(&store, 13, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    {
        const InfoStore invalid_store = {0};

        found = (const InfoNode *)(uintptr_t)1;
        CHECK(cfr_info_store_find_const(&invalid_store, 13, &found) ==
              CFR_STATUS_INVALID_ARGUMENT);
        CHECK(found == (const InfoNode *)(uintptr_t)1);
    }

    CHECK(cfr_evaluation_average_strategy(&store, 13, strategy, 1,
                                          &required_count) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(required_count == 2);
    CHECK(strategy[0] == 91.0 && strategy[1] == 92.0);
    CHECK(cfr_evaluation_average_strategy(
              &store, 13, strategy, ARRAY_COUNT(strategy), &required_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(required_count == 2);
    CHECK(near(strategy[0], 0.75, 1e-15));
    CHECK(near(strategy[1], 0.25, 1e-15));

    node_13->strategy_sums[0] = 0.0;
    node_13->strategy_sums[1] = 0.0;
    CHECK(cfr_evaluation_average_strategy(
              &store, 13, strategy, ARRAY_COUNT(strategy), &required_count) ==
          CFR_STATUS_SUCCESS);
    CHECK(near(strategy[0], 0.5, 1e-15));
    CHECK(near(strategy[1], 0.5, 1e-15));

    strategy[0] = 81.0;
    strategy[1] = 82.0;
    required_count = 83;
    CHECK(cfr_evaluation_average_strategy(
              &store, 18, strategy, ARRAY_COUNT(strategy), &required_count) ==
          CFR_STATUS_NOT_FOUND);
    CHECK(strategy[0] == 81.0 && strategy[1] == 82.0);
    CHECK(required_count == 83);
    CHECK(store.size == 3);

    CHECK(cfr_evaluation_average_strategy(
              NULL, 13, strategy, ARRAY_COUNT(strategy), &required_count) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(
        cfr_evaluation_average_strategy(&store, 13, NULL, 0, &required_count) ==
        CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_average_strategy(&store, 13, strategy,
                                          ARRAY_COUNT(strategy),
                                          NULL) == CFR_STATUS_INVALID_ARGUMENT);

    node_13->strategy_sums[0] = -1.0;
    node_13->strategy_sums[1] = 1.0;
    strategy[0] = 71.0;
    strategy[1] = 72.0;
    required_count = 73;
    CHECK(cfr_evaluation_average_strategy(
              &store, 13, strategy, ARRAY_COUNT(strategy), &required_count) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(strategy[0] == 71.0 && strategy[1] == 72.0);
    CHECK(required_count == 73);
    after = get_store_stats(&store);
    CHECK(same_store_stats(&before, &after));
    destroy_store(&store);
}

static void test_fixed_profile_and_metrics(void) {
    static const InfoSetKey keys[] = {100, 200};
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    StoreSnapshot store_snapshot;
    EvaluationMetrics metrics = sentinel_metrics();
    Utility profile_0 = 61.0;
    Utility profile_1 = 62.0;
    Utility best_response_0 = 63.0;
    Utility best_response_1 = 64.0;

    initialize_store(&store);
    create_policy(&store, 100, 3.0, 1.0);
    create_policy(&store, 200, 1.0, 3.0);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(capture_store(&store, keys, ARRAY_COUNT(keys), &store_snapshot));

    CHECK(cfr_evaluation_profile_value(
              game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &profile_0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_profile_value(
              game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_1, &profile_1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_best_response_value(
              game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &best_response_0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_best_response_value(
              game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_1, &best_response_1) == CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_metrics(game, traversal_game_state_as_public(&state),
                                 &store, &metrics) == CFR_STATUS_SUCCESS);

    CHECK(near(profile_0, 1.375, 1e-12));
    CHECK(near(profile_1, -1.375, 1e-12));
    CHECK(near(best_response_0, 2.0, 1e-12));
    CHECK(near(best_response_1, -1.25, 1e-12));
    CHECK(near(metrics.profile_value_player_0, profile_0, 1e-12));
    CHECK(near(metrics.profile_value_player_1, profile_1, 1e-12));
    CHECK(near(metrics.best_response_value_player_0, best_response_0, 1e-12));
    CHECK(near(metrics.best_response_value_player_1, best_response_1, 1e-12));
    CHECK(near(metrics.improvement_player_0, 0.625, 1e-12));
    CHECK(near(metrics.improvement_player_1, 0.125, 1e-12));
    CHECK(near(metrics.nash_conv, 0.75, 1e-12));
    CHECK(near(metrics.exploitability, 0.375, 1e-12));
    CHECK(metrics.nash_conv >= 0.0 && metrics.exploitability >= 0.0);
    CHECK(isfinite(metrics.nash_conv) && isfinite(metrics.exploitability));
    CHECK(same_traversal_state(&state, &snapshot));
    CHECK(store_matches_snapshot(&store, &store_snapshot));
    destroy_store(&store);
}

static void test_shared_information_and_deep_response(void) {
    {
        static const InfoSetKey keys[] = {500, 501};
        const Game *game = traversal_game_descriptor();
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        StoreSnapshot store_snapshot;
        Utility profile = 71.0;
        Utility best_response = 72.0;

        initialize_store(&store);
        create_policy(&store, 500, 3.0, 1.0);
        create_policy(&store, 501, 1.0, 1.0);
        CHECK(traversal_game_state_init_shared(&state, false) ==
              CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(capture_store(&store, keys, ARRAY_COUNT(keys), &store_snapshot));
        CHECK(cfr_evaluation_profile_value(
                  game, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_1, &profile) == CFR_STATUS_SUCCESS);
        CHECK(cfr_evaluation_best_response_value(
                  game, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_1, &best_response) == CFR_STATUS_SUCCESS);
        CHECK(near(profile, 2.0, 1e-12));
        CHECK(near(best_response, 3.0, 1e-12));
        CHECK(best_response < 4.0);
        CHECK(same_traversal_state(&state, &snapshot));
        CHECK(store_matches_snapshot(&store, &store_snapshot));
        destroy_store(&store);
    }

    {
        static const InfoSetKey keys[] = {300, 400};
        const Game *game = traversal_game_descriptor();
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        StoreSnapshot store_snapshot;
        EvaluationMetrics metrics = sentinel_metrics();
        Utility profile = 73.0;
        Utility best_response = 74.0;

        initialize_store(&store);
        create_policy(&store, 300, 1.0, 1.0);
        create_policy(&store, 400, 1.0, 1.0);
        CHECK(traversal_game_state_init_reach(&state) == CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(capture_store(&store, keys, ARRAY_COUNT(keys), &store_snapshot));
        CHECK(cfr_evaluation_profile_value(
                  game, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_1, &profile) == CFR_STATUS_SUCCESS);
        CHECK(cfr_evaluation_best_response_value(
                  game, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_1, &best_response) == CFR_STATUS_SUCCESS);
        CHECK(cfr_evaluation_metrics(game,
                                     traversal_game_state_as_public(&state),
                                     &store, &metrics) == CFR_STATUS_SUCCESS);
        CHECK(near(profile, 0.0, 1e-12));
        CHECK(near(best_response, 1.0, 1e-12));
        CHECK(near(metrics.improvement_player_1, 1.0, 1e-12));
        CHECK(near(metrics.nash_conv, 1.0, 1e-12));
        CHECK(near(metrics.exploitability, 0.5, 1e-12));
        CHECK(same_traversal_state(&state, &snapshot));
        CHECK(store_matches_snapshot(&store, &store_snapshot));
        destroy_store(&store);
    }
}

static void test_chance_profile_and_exact_branches(void) {
    static const InfoSetKey key = 800;
    const Game *game = chance_game_descriptor();
    ChanceGameState state;
    ChanceGameState snapshot;
    InfoStore store;
    StoreSnapshot store_snapshot;
    EvaluationMetrics metrics = sentinel_metrics();
    Utility profile = 81.0;
    Utility best_response = 82.0;

    initialize_store(&store);
    create_policy(&store, key, 1.0, 3.0);
    CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(capture_store(&store, &key, 1, &store_snapshot));
    CHECK(cfr_evaluation_profile_value(
              game, chance_game_state_as_public(&state), &store, CFR_PLAYER_0,
              &profile) == CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_best_response_value(
              game, chance_game_state_as_public(&state), &store, CFR_PLAYER_0,
              &best_response) == CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_metrics(game, chance_game_state_as_public(&state),
                                 &store, &metrics) == CFR_STATUS_SUCCESS);
    CHECK(near(profile, 0.25, 1e-12));
    CHECK(near(best_response, 0.5, 1e-12));
    CHECK(near(metrics.profile_value_player_0, 0.25, 1e-12));
    CHECK(near(metrics.profile_value_player_1, -0.25, 1e-12));
    CHECK(near(metrics.improvement_player_0, 0.25, 1e-12));
    CHECK(near(metrics.improvement_player_1, 0.0, 1e-12));
    CHECK(near(metrics.nash_conv, 0.25, 1e-12));
    CHECK(near(metrics.exploitability, 0.125, 1e-12));
    CHECK(chance_game_state_equal(&state, &snapshot));
    CHECK(store_matches_snapshot(&store, &store_snapshot));

    chance_game_set_probabilities(&state, 0.0, 1.0);
    snapshot = state;
    profile = 83.0;
    CHECK(cfr_evaluation_profile_value(
              game, chance_game_state_as_public(&state), &store, CFR_PLAYER_0,
              &profile) == CFR_STATUS_SUCCESS);
    CHECK(near(profile, 1.5, 1e-12));
    CHECK(chance_game_state_equal(&state, &snapshot));

    chance_game_fail_terminal_for_player(&state, CFR_PLAYER_0,
                                         CFR_STATUS_NUMERIC_ERROR);
    snapshot = state;
    profile = 84.0;
    CHECK(cfr_evaluation_profile_value(
              game, chance_game_state_as_public(&state), &store, CFR_PLAYER_0,
              &profile) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(profile == 84.0);
    CHECK(chance_game_state_equal(&state, &snapshot));
    CHECK(store_matches_snapshot(&store, &store_snapshot));
    destroy_store(&store);
}

static void test_chance_distributions_and_depth(void) {
    const Probability invalid[][2] = {
        {-0.1, 1.1}, {NAN, 1.0}, {INFINITY, 0.0}, {0.4, 0.4}, {0.5, 0.50000002},
    };
    size_t index;

    for (index = 0; index < ARRAY_COUNT(invalid); index += 1) {
        const InfoSetKey key = 800;
        ChanceGameState state;
        ChanceGameState snapshot;
        InfoStore store;
        StoreSnapshot store_snapshot;
        Utility utility = 85.0;

        initialize_store(&store);
        create_policy(&store, key, 1.0, 1.0);
        CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
        chance_game_set_probabilities(&state, invalid[index][0],
                                      invalid[index][1]);
        snapshot = state;
        CHECK(capture_store(&store, &key, 1, &store_snapshot));
        CHECK(cfr_evaluation_profile_value(chance_game_descriptor(),
                                           chance_game_state_as_public(&state),
                                           &store, CFR_PLAYER_0, &utility) ==
              CFR_STATUS_INVALID_ARGUMENT);
        CHECK(utility == 85.0);
        CHECK(chance_game_state_equal(&state, &snapshot));
        CHECK(store_matches_snapshot(&store, &store_snapshot));
        destroy_store(&store);
    }

    {
        const InfoSetKey key = 800;
        ChanceGameState state;
        ChanceGameState snapshot;
        InfoStore store;
        StoreSnapshot store_snapshot;
        Utility utility = 86.0;

        initialize_store(&store);
        create_policy(&store, key, 1.0, 1.0);
        CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
        chance_game_set_probabilities(&state, 0.5, 0.500000000002);
        snapshot = state;
        CHECK(capture_store(&store, &key, 1, &store_snapshot));
        CHECK(cfr_evaluation_profile_value(
                  chance_game_descriptor(), chance_game_state_as_public(&state),
                  &store, CFR_PLAYER_0, &utility) == CFR_STATUS_SUCCESS);
        CHECK(isfinite(utility));
        CHECK(chance_game_state_equal(&state, &snapshot));
        CHECK(store_matches_snapshot(&store, &store_snapshot));
        destroy_store(&store);
    }

    {
        const InfoSetKey key = 801;
        ChanceGameState state;
        ChanceGameState snapshot;
        InfoStore store;
        StoreSnapshot store_snapshot;
        Utility utility = 87.0;

        initialize_store(&store);
        create_policy(&store, key, 1.0, 1.0);
        CHECK(chance_game_state_init_deep(&state, 40) == CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(capture_store(&store, &key, 1, &store_snapshot));
        CHECK(cfr_evaluation_profile_value(
                  chance_game_descriptor(), chance_game_state_as_public(&state),
                  &store, CFR_PLAYER_0, &utility) == CFR_STATUS_SUCCESS);
        CHECK(near(utility, 1.0, 1e-12));
        CHECK(chance_game_state_equal(&state, &snapshot));
        CHECK(store_matches_snapshot(&store, &store_snapshot));
        destroy_store(&store);
    }
}

static void test_public_invalid_arguments(void) {
    const Game *descriptor = traversal_game_descriptor();
    Game game = *descriptor;
    GameOperations operations = *descriptor->operations;
    TraversalGameState state;
    InfoStore store;
    Utility utility = 91.0;
    EvaluationMetrics metrics = sentinel_metrics();
    EvaluationMetrics expected_metrics = metrics;

    initialize_store(&store);
    create_policy(&store, 100, 1.0, 1.0);
    create_policy(&store, 200, 1.0, 1.0);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);

    CHECK(cfr_evaluation_profile_value(
              NULL, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_profile_value(&game, NULL, &store, CFR_PLAYER_0,
                                       &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), NULL, CFR_PLAYER_0,
              &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), &store, (Player)99,
              &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, NULL) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 91.0);

    CHECK(cfr_evaluation_best_response_value(
              NULL, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_best_response_value(
              &game, traversal_game_state_as_public(&state), &store, (Player)-1,
              &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_best_response_value(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, NULL) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 91.0);

    CHECK(cfr_evaluation_metrics(NULL, traversal_game_state_as_public(&state),
                                 &store,
                                 &metrics) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_metrics(&game, NULL, &store, &metrics) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_metrics(&game, traversal_game_state_as_public(&state),
                                 NULL,
                                 &metrics) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_metrics(&game, traversal_game_state_as_public(&state),
                                 &store, NULL) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(same_metrics(&metrics, &expected_metrics));

    game.max_legal_actions = 0;
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 91.0);
    game.max_legal_actions = SIZE_MAX;
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &utility) == CFR_STATUS_OUT_OF_MEMORY);
    CHECK(utility == 91.0);

    game = *descriptor;
    operations.apply_action = NULL;
    game.operations = &operations;
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 91.0);
    destroy_store(&store);
}

static void test_missing_and_inconsistent_information(void) {
    const Game *descriptor = traversal_game_descriptor();

    {
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        Utility utility = 92.0;

        initialize_store(&store);
        CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(cfr_evaluation_profile_value(
                  descriptor, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_0, &utility) == CFR_STATUS_NOT_FOUND);
        CHECK(utility == 92.0);
        CHECK(store.size == 0);
        CHECK(same_traversal_state(&state, &snapshot));
        destroy_store(&store);
    }

    {
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        Utility utility = 94.5;

        initialize_store(&store);
        create_policy(&store, 100, 1.0, 1.0);
        create_policy(&store, 200, 1.0, 1.0);
        CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
        traversal_game_force_required_count(&state, 0);
        snapshot = state;
        CHECK(cfr_evaluation_profile_value(
                  descriptor, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
        CHECK(utility == 94.5);
        CHECK(same_traversal_state(&state, &snapshot));
        destroy_store(&store);
    }

    {
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        InfoNode *node = NULL;
        Utility utility = 93.0;

        initialize_store(&store);
        CHECK(cfr_info_store_get_or_create(&store, 100, 1, &node) ==
              CFR_STATUS_SUCCESS);
        CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(cfr_evaluation_profile_value(
                  descriptor, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
        CHECK(utility == 93.0);
        CHECK(same_traversal_state(&state, &snapshot));
        destroy_store(&store);
    }

    {
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        Utility utility = 94.0;

        initialize_store(&store);
        create_policy(&store, 100, 1.0, 1.0);
        create_policy(&store, 200, 1.0, 1.0);
        CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
        traversal_game_force_required_count(&state, 3);
        snapshot = state;
        CHECK(cfr_evaluation_profile_value(
                  descriptor, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
        CHECK(utility == 94.0);
        CHECK(same_traversal_state(&state, &snapshot));
        destroy_store(&store);
    }

    {
        Game game = *descriptor;
        GameOperations operations = *descriptor->operations;
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        Utility utility = 95.0;

        operations.current_actor = invalid_actor;
        game.operations = &operations;
        initialize_store(&store);
        create_policy(&store, 100, 1.0, 1.0);
        create_policy(&store, 200, 1.0, 1.0);
        CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(cfr_evaluation_profile_value(
                  &game, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
        CHECK(utility == 95.0);
        CHECK(same_traversal_state(&state, &snapshot));
        destroy_store(&store);
    }

    {
        Game game = *descriptor;
        GameOperations operations = *descriptor->operations;
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        Utility utility = 95.5;

        operations.current_actor = invalid_player_actor;
        game.operations = &operations;
        initialize_store(&store);
        create_policy(&store, 100, 1.0, 1.0);
        create_policy(&store, 200, 1.0, 1.0);
        CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(cfr_evaluation_profile_value(
                  &game, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
        CHECK(utility == 95.5);
        CHECK(same_traversal_state(&state, &snapshot));
        destroy_store(&store);
    }

    {
        Game game = *descriptor;
        GameOperations operations = *descriptor->operations;
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        EvaluationMetrics metrics = sentinel_metrics();
        EvaluationMetrics expected = metrics;

        operations.legal_actions = reversed_shared_actions;
        game.operations = &operations;
        initialize_store(&store);
        create_policy(&store, 500, 1.0, 1.0);
        create_policy(&store, 501, 1.0, 1.0);
        CHECK(traversal_game_state_init_shared(&state, false) ==
              CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(cfr_evaluation_metrics(
                  &game, traversal_game_state_as_public(&state), &store,
                  &metrics) == CFR_STATUS_INVALID_ARGUMENT);
        CHECK(same_metrics(&metrics, &expected));
        CHECK(same_traversal_state(&state, &snapshot));
        destroy_store(&store);
    }

    {
        Game game = *descriptor;
        GameOperations operations = *descriptor->operations;
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        Utility utility = 96.0;

        operations.information_set_key = circular_information_key;
        operations.legal_actions = circular_legal_actions;
        operations.apply_action = circular_apply_action;
        game.operations = &operations;
        initialize_store(&store);
        create_policy(&store, 300, 1.0, 1.0);
        CHECK(traversal_game_state_init_reach(&state) == CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(cfr_evaluation_best_response_value(
                  &game, traversal_game_state_as_public(&state), &store,
                  CFR_PLAYER_1, &utility) == CFR_STATUS_INVALID_ARGUMENT);
        CHECK(utility == 96.0);
        CHECK(same_traversal_state(&state, &snapshot));
        destroy_store(&store);
    }
}

static void test_terminal_numeric_contract(void) {
    const InfoSetKey unused_key = 0;
    const Game *descriptor = traversal_game_descriptor();
    Game game = *descriptor;
    GameOperations operations = *descriptor->operations;
    TerminalUtilityOverride override;
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    StoreSnapshot store_snapshot;
    Utility utility;

    operations.terminal_utility = overridden_terminal_utility;
    game.operations = &operations;
    game.context = &override;
    initialize_store(&store);

    CHECK(traversal_game_state_init_terminal(&state, 1e100) ==
          CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(capture_store(&store, &unused_key, 0, &store_snapshot));
    override.player_1_utility = -1e100 + 2e88;
    utility = 101.0;
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &utility) == CFR_STATUS_SUCCESS);
    CHECK(utility == 1e100);
    CHECK(same_traversal_state(&state, &snapshot));
    CHECK(store_matches_snapshot(&store, &store_snapshot));

    {
        EvaluationMetrics metrics = sentinel_metrics();

        CHECK(cfr_evaluation_metrics(&game,
                                     traversal_game_state_as_public(&state),
                                     &store, &metrics) == CFR_STATUS_SUCCESS);
        CHECK(metrics.profile_value_player_0 == 1e100);
        CHECK(metrics.profile_value_player_1 == override.player_1_utility);
        CHECK(metrics.best_response_value_player_0 == 1e100);
        CHECK(metrics.best_response_value_player_1 ==
              override.player_1_utility);
        CHECK(metrics.improvement_player_0 == 0.0);
        CHECK(metrics.improvement_player_1 == 0.0);
        CHECK(metrics.nash_conv == 0.0);
        CHECK(metrics.exploitability == 0.0);
        CHECK(same_traversal_state(&state, &snapshot));
        CHECK(store_matches_snapshot(&store, &store_snapshot));
    }

    override.player_1_utility = -1e100 + 2e92;
    utility = 102.0;
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 102.0);
    CHECK(same_traversal_state(&state, &snapshot));
    CHECK(store_matches_snapshot(&store, &store_snapshot));

    CHECK(traversal_game_state_init_terminal(&state, DBL_MAX) ==
          CFR_STATUS_SUCCESS);
    snapshot = state;
    override.player_1_utility = DBL_MAX;
    utility = 103.0;
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &utility) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(utility == 103.0);
    CHECK(same_traversal_state(&state, &snapshot));
    CHECK(store_matches_snapshot(&store, &store_snapshot));

    game = *descriptor;
    CHECK(traversal_game_state_init_terminal(&state, NAN) ==
          CFR_STATUS_SUCCESS);
    snapshot = state;
    utility = 104.0;
    CHECK(cfr_evaluation_profile_value(
              &game, traversal_game_state_as_public(&state), &store,
              CFR_PLAYER_0, &utility) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(utility == 104.0);
    CHECK(memcmp(&state, &snapshot, sizeof(state)) == 0);
    CHECK(store_matches_snapshot(&store, &store_snapshot));
    destroy_store(&store);
}

static void test_negative_improvement_rounding_adjustment(void) {
    static const InfoSetKey keys[] = {100, 200};
    const Game *descriptor = traversal_game_descriptor();
    Game game = *descriptor;
    GameOperations operations = *descriptor->operations;
    ConstantTerminalUtility utility = {.player_0_utility = 1e10};
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    StoreSnapshot store_snapshot;
    EvaluationMetrics metrics = sentinel_metrics();

    operations.terminal_utility = constant_terminal_utility;
    game.operations = &operations;
    game.context = &utility;
    initialize_store(&store);
    create_policy(&store, 100, 1.0, 12.0);
    create_policy(&store, 200, 1.0, 1.0);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(capture_store(&store, keys, ARRAY_COUNT(keys), &store_snapshot));

    CHECK(cfr_evaluation_metrics(&game, traversal_game_state_as_public(&state),
                                 &store, &metrics) == CFR_STATUS_SUCCESS);
    CHECK(metrics.profile_value_player_0 >
          metrics.best_response_value_player_0);
    CHECK(metrics.improvement_player_0 == 0.0);
    CHECK(metrics.improvement_player_1 == 0.0);
    CHECK(metrics.nash_conv == 0.0);
    CHECK(metrics.exploitability == 0.0);
    CHECK(same_traversal_state(&state, &snapshot));
    CHECK(store_matches_snapshot(&store, &store_snapshot));
    destroy_store(&store);
}

static void test_error_restoration_and_undo_priority(void) {
    static const InfoSetKey traversal_keys[] = {100, 200};

    {
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        StoreSnapshot store_snapshot;
        Utility utility = 111.0;

        initialize_store(&store);
        create_policy(&store, 100, 1.0, 1.0);
        create_policy(&store, 200, 1.0, 1.0);
        CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
        traversal_game_fail_after_action(&state, TRAVERSAL_ACTION_ENTER,
                                         CFR_STATUS_NUMERIC_ERROR);
        snapshot = state;
        CHECK(capture_store(&store, traversal_keys, ARRAY_COUNT(traversal_keys),
                            &store_snapshot));
        CHECK(cfr_evaluation_profile_value(
                  traversal_game_descriptor(),
                  traversal_game_state_as_public(&state), &store, CFR_PLAYER_0,
                  &utility) == CFR_STATUS_NUMERIC_ERROR);
        CHECK(utility == 111.0);
        CHECK(same_traversal_state(&state, &snapshot));
        CHECK(store_matches_snapshot(&store, &store_snapshot));
        destroy_store(&store);
    }

    {
        TraversalGameState state;
        InfoStore store;
        Utility utility = 112.0;

        initialize_store(&store);
        create_policy(&store, 100, 1.0, 1.0);
        create_policy(&store, 200, 1.0, 1.0);
        CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
        traversal_game_fail_after_apply(&state, CFR_STATUS_NUMERIC_ERROR);
        traversal_game_fail_undo(&state, CFR_STATUS_BUFFER_TOO_SMALL);
        CHECK(cfr_evaluation_profile_value(
                  traversal_game_descriptor(),
                  traversal_game_state_as_public(&state), &store, CFR_PLAYER_0,
                  &utility) == CFR_STATUS_BUFFER_TOO_SMALL);
        CHECK(utility == 112.0);
        CHECK(state.phase == TRAVERSAL_PHASE_TERMINAL);
        CHECK(state.history_count == 1);
        traversal_game_fail_undo(&state, CFR_STATUS_SUCCESS);
        CHECK(cfr_game_undo_action(traversal_game_descriptor(),
                                   traversal_game_state_as_public(&state)) ==
              CFR_STATUS_SUCCESS);
        destroy_store(&store);
    }

    {
        const InfoSetKey key = 800;
        ChanceGameState state;
        ChanceGameState snapshot;
        InfoStore store;
        Utility utility = 113.0;

        initialize_store(&store);
        create_policy(&store, key, 1.0, 1.0);
        CHECK(chance_game_state_init_coin(&state) == CFR_STATUS_SUCCESS);
        chance_game_fail_probability(&state, CHANCE_GAME_ACTION_HEADS,
                                     CFR_STATUS_ILLEGAL_ACTION);
        snapshot = state;
        CHECK(cfr_evaluation_profile_value(
                  chance_game_descriptor(), chance_game_state_as_public(&state),
                  &store, CFR_PLAYER_0, &utility) == CFR_STATUS_ILLEGAL_ACTION);
        CHECK(utility == 113.0);
        CHECK(chance_game_state_equal(&state, &snapshot));

        chance_game_fail_probability(&state, CHANCE_GAME_ACTION_HEADS,
                                     CFR_STATUS_SUCCESS);
        chance_game_fail_after_action(&state, CHANCE_GAME_ACTION_HEADS,
                                      CFR_STATUS_NUMERIC_ERROR);
        snapshot = state;
        CHECK(cfr_evaluation_profile_value(
                  chance_game_descriptor(), chance_game_state_as_public(&state),
                  &store, CFR_PLAYER_0, &utility) == CFR_STATUS_NUMERIC_ERROR);
        CHECK(utility == 113.0);
        CHECK(chance_game_state_equal(&state, &snapshot));
        destroy_store(&store);
    }
}

static void initialize_kuhn(KuhnPokerState *state) {
    *state = (KuhnPokerState){0};
    CHECK(cfr_kuhn_poker_state_init(state) == CFR_STATUS_SUCCESS);
}

static void test_kuhn_uniform_and_trained_metrics(void) {
    const size_t iteration_count = 100000;
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState state;
    KuhnPokerState root;
    InfoStore store;
    InfoSetKey keys[SNAPSHOT_MAX_NODES] = {0};
    size_t key_count = 0;
    StoreSnapshot uniform_snapshot;
    StoreSnapshot trained_snapshot;
    EvaluationMetrics uniform_metrics = sentinel_metrics();
    EvaluationMetrics trained_metrics = sentinel_metrics();
    Trainer trainer;
    TrainerStats stats_before;
    TrainerStats stats_after;
    Utility profile_0 = 121.0;
    Utility profile_1 = 122.0;

    initialize_store(&store);
    initialize_kuhn(&state);
    root = state;
    CHECK(populate_uniform_store(
              game, cfr_kuhn_poker_state_as_game_state(&state), &store, keys,
              ARRAY_COUNT(keys), &key_count) == CFR_STATUS_SUCCESS);
    CHECK(key_count == 12);
    CHECK(store.size == 12);
    CHECK(memcmp(&state, &root, sizeof(state)) == 0);
    CHECK(capture_store(&store, keys, key_count, &uniform_snapshot));

    CHECK(cfr_evaluation_profile_value(
              game, cfr_kuhn_poker_state_as_game_state(&state), &store,
              CFR_PLAYER_0, &profile_0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_evaluation_profile_value(
              game, cfr_kuhn_poker_state_as_game_state(&state), &store,
              CFR_PLAYER_1, &profile_1) == CFR_STATUS_SUCCESS);
    CHECK(
        cfr_evaluation_metrics(game, cfr_kuhn_poker_state_as_game_state(&state),
                               &store, &uniform_metrics) == CFR_STATUS_SUCCESS);
    CHECK(near(profile_0, 1.0 / 8.0, 1e-12));
    CHECK(near(profile_1, -1.0 / 8.0, 1e-12));
    CHECK(near(uniform_metrics.profile_value_player_0, 1.0 / 8.0, 1e-12));
    CHECK(near(uniform_metrics.profile_value_player_1, -1.0 / 8.0, 1e-12));
    CHECK(near(uniform_metrics.nash_conv, 11.0 / 12.0, 1e-12));
    CHECK(near(uniform_metrics.exploitability, 11.0 / 24.0, 1e-12));
    CHECK(uniform_metrics.improvement_player_0 >= 0.0);
    CHECK(uniform_metrics.improvement_player_1 >= 0.0);
    CHECK(isfinite(uniform_metrics.best_response_value_player_0));
    CHECK(isfinite(uniform_metrics.best_response_value_player_1));
    CHECK(memcmp(&state, &root, sizeof(state)) == 0);
    CHECK(store_matches_snapshot(&store, &uniform_snapshot));

    CHECK(cfr_trainer_init(&trainer, game,
                           cfr_kuhn_poker_state_as_game_state(&state),
                           &store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_run(&trainer, iteration_count) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats_before) == CFR_STATUS_SUCCESS);
    CHECK(capture_store(&store, keys, key_count, &trained_snapshot));
    CHECK(
        cfr_evaluation_metrics(game, cfr_kuhn_poker_state_as_game_state(&state),
                               &store, &trained_metrics) == CFR_STATUS_SUCCESS);
    CHECK(cfr_trainer_get_stats(&trainer, &stats_after) == CFR_STATUS_SUCCESS);
    CHECK(near(trained_metrics.profile_value_player_0, -1.0 / 18.0, 0.0001));
    CHECK(near(trained_metrics.profile_value_player_1, 1.0 / 18.0, 0.0001));
    CHECK(trained_metrics.exploitability < uniform_metrics.exploitability);
    CHECK(trained_metrics.exploitability >= 0.0);
    CHECK(trained_metrics.nash_conv >= 0.0);
    CHECK(isfinite(trained_metrics.exploitability));
    CHECK(isfinite(trained_metrics.nash_conv));
    CHECK(memcmp(&stats_before, &stats_after, sizeof(stats_before)) == 0);
    CHECK(memcmp(&state, &root, sizeof(state)) == 0);
    CHECK(store_matches_snapshot(&store, &trained_snapshot));
    destroy_store(&store);
}

#ifdef CFR_TEST_WRAP_ALLOCATOR
static void test_evaluation_allocation_failures_are_clean(void) {
    const Game *game = cfr_kuhn_poker_descriptor();
    KuhnPokerState state;
    KuhnPokerState root;
    InfoStore store;
    InfoSetKey keys[SNAPSHOT_MAX_NODES] = {0};
    size_t key_count = 0;
    StoreSnapshot store_snapshot;
    size_t failure_index;
    bool reached_success = false;

    initialize_store(&store);
    initialize_kuhn(&state);
    root = state;
    CHECK(populate_uniform_store(
              game, cfr_kuhn_poker_state_as_game_state(&state), &store, keys,
              ARRAY_COUNT(keys), &key_count) == CFR_STATUS_SUCCESS);
    CHECK(capture_store(&store, keys, key_count, &store_snapshot));

    for (failure_index = 0; failure_index < 128; failure_index += 1) {
        const size_t live_before = test_allocator_live_allocations();
        EvaluationMetrics metrics = sentinel_metrics();
        EvaluationMetrics expected = metrics;
        Status status;

        test_allocator_fail_after(failure_index);
        status = cfr_evaluation_metrics(
            game, cfr_kuhn_poker_state_as_game_state(&state), &store, &metrics);
        test_allocator_disable_failures();
        CHECK(test_allocator_live_allocations() == live_before);
        CHECK(memcmp(&state, &root, sizeof(state)) == 0);
        CHECK(store_matches_snapshot(&store, &store_snapshot));
        if (status == CFR_STATUS_SUCCESS) {
            reached_success = true;
            CHECK(isfinite(metrics.exploitability));
            break;
        }
        CHECK(status == CFR_STATUS_OUT_OF_MEMORY);
        CHECK(same_metrics(&metrics, &expected));
    }
    CHECK(reached_success);
    CHECK(failure_index >= 10);
    destroy_store(&store);
    CHECK(test_allocator_live_allocations() == 0);
}
#endif

int test_evaluation(void) {
    failures = 0;

    test_const_lookup_and_average_strategy();
    test_fixed_profile_and_metrics();
    test_shared_information_and_deep_response();
    test_chance_profile_and_exact_branches();
    test_chance_distributions_and_depth();
    test_public_invalid_arguments();
    test_missing_and_inconsistent_information();
    test_terminal_numeric_contract();
    test_negative_improvement_rounding_adjustment();
    test_error_restoration_and_undo_priority();
    test_kuhn_uniform_and_trained_metrics();
#ifdef CFR_TEST_WRAP_ALLOCATOR
    test_evaluation_allocation_failures_are_clean();
#endif

    return failures;
}

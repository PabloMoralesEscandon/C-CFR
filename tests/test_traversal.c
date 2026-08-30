#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "cfr/traversal.h"
#include "support/test_allocator.h"
#include "support/traversal_game.h"
#include "test_suite.h"

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            failures += 1;                                                      \
        }                                                                      \
    } while (0)

static bool near(double left, double right) {
    return fabs(left - right) <= 1e-12;
}

static bool same_utility(Utility left, Utility right) {
    return left == right || (isnan(left) && isnan(right));
}

static void initialize_store(InfoStore *store) {
    *store = (InfoStore){0};
    CHECK(cfr_info_store_init(store) == CFR_STATUS_SUCCESS);
}

static void destroy_store(InfoStore *store) {
    CHECK(cfr_info_store_destroy(store) == CFR_STATUS_SUCCESS);
}

static bool same_state(const TraversalGameState *left,
                       const TraversalGameState *right) {
    size_t index;

    if (left->phase != right->phase ||
        !same_utility(left->terminal_utility_player_0,
                      right->terminal_utility_player_0) ||
        left->last_action != right->last_action ||
        left->history_count != right->history_count ||
        left->reverse_shared_root_actions !=
            right->reverse_shared_root_actions ||
        left->fail_after_any_action != right->fail_after_any_action ||
        left->fail_after_selected_action !=
            right->fail_after_selected_action ||
        left->selected_failure_action != right->selected_failure_action ||
        left->failure_after_apply != right->failure_after_apply ||
        left->undo_failure != right->undo_failure ||
        left->force_required_count != right->force_required_count ||
        left->forced_required_count != right->forced_required_count) {
        return false;
    }
    for (index = 0; index < left->history_count; index += 1) {
        if (left->history[index].phase != right->history[index].phase ||
            !same_utility(
                left->history[index].terminal_utility_player_0,
                right->history[index].terminal_utility_player_0) ||
            left->history[index].last_action !=
                right->history[index].last_action) {
            return false;
        }
    }
    return true;
}

static InfoNode *find_node(InfoStore *store, InfoSetKey key) {
    InfoNode *node = NULL;

    CHECK(cfr_info_store_find(store, key, &node) == CFR_STATUS_SUCCESS);
    CHECK(node != NULL);
    return node;
}

static void check_zero_learning(const InfoNode *node) {
    size_t index;

    for (index = 0; index < node->action_count; index += 1) {
        CHECK(node->regret_sums[index] == 0.0);
        CHECK(node->strategy_sums[index] == 0.0);
    }
}

static size_t validation_calls;
static size_t trusted_terminal_queries;
static const GameOperations *trusted_delegate;

static Status accept_root_state(const void *context, const GameState *state) {
    (void)context;
    (void)state;
    validation_calls += 1;
    return CFR_STATUS_SUCCESS;
}

static Status reject_root_state(const void *context, const GameState *state) {
    (void)context;
    (void)state;
    validation_calls += 1;
    return CFR_STATUS_NUMERIC_ERROR;
}

static Status count_trusted_terminal_query(const void *context,
                                           const GameState *state,
                                           bool *result) {
    trusted_terminal_queries += 1;
    return trusted_delegate->is_terminal(context, state, result);
}

static void test_terminal_utility_and_signs(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    Utility utility = 91.0;

    initialize_store(&store);
    CHECK(traversal_game_state_init_terminal(&state, 3.5) ==
          CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, 3.5));
    CHECK(store.size == 0);
    CHECK(same_state(&state, &snapshot));

    utility = 91.0;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_1, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, -3.5));
    CHECK(store.size == 0);
    CHECK(same_state(&state, &snapshot));
    destroy_store(&store);
}

static void test_player_1_learning(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    InfoNode *player_0_node;
    InfoNode *player_1_node;
    Utility utility = 92.0;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_1, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, -1.0));
    CHECK(same_state(&state, &snapshot));
    CHECK(store.size == 2);

    player_0_node = find_node(&store, 100);
    player_1_node = find_node(&store, 200);
    check_zero_learning(player_0_node);
    CHECK(near(player_1_node->regret_sums[0], -0.5));
    CHECK(near(player_1_node->regret_sums[1], 0.5));
    CHECK(near(player_1_node->strategy_sums[0], 0.5));
    CHECK(near(player_1_node->strategy_sums[1], 0.5));
    destroy_store(&store);
}

static void test_player_0_learning(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    InfoNode *player_0_node;
    InfoNode *player_1_node;
    Utility utility = 93.0;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, 1.0));
    CHECK(same_state(&state, &snapshot));
    CHECK(store.size == 2);

    player_0_node = find_node(&store, 100);
    player_1_node = find_node(&store, 200);
    CHECK(near(player_0_node->regret_sums[0], 1.0));
    CHECK(near(player_0_node->regret_sums[1], -1.0));
    CHECK(near(player_0_node->strategy_sums[0], 0.5));
    CHECK(near(player_0_node->strategy_sums[1], 0.5));
    check_zero_learning(player_1_node);
    destroy_store(&store);
}

static void test_own_reach_weights_average_strategy(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    InfoNode *root_node;
    InfoNode *second_node;
    Utility utility = 94.0;

    initialize_store(&store);
    CHECK(traversal_game_state_init_reach(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_1, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, 0.0));
    CHECK(same_state(&state, &snapshot));

    root_node = find_node(&store, 300);
    second_node = find_node(&store, 400);
    CHECK(near(root_node->regret_sums[0], 0.0));
    CHECK(near(root_node->regret_sums[1], 0.0));
    CHECK(near(root_node->strategy_sums[0], 0.5));
    CHECK(near(root_node->strategy_sums[1], 0.5));
    CHECK(near(second_node->regret_sums[0], -1.0));
    CHECK(near(second_node->regret_sums[1], 1.0));
    CHECK(near(second_node->strategy_sums[0], 0.25));
    CHECK(near(second_node->strategy_sums[1], 0.25));
    destroy_store(&store);
}

static void check_shared_information_set(bool reverse_root_actions) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    InfoNode *root_node;
    InfoNode *shared_node;
    Utility utility = 94.5;

    initialize_store(&store);
    CHECK(traversal_game_state_init_shared(&state, reverse_root_actions) ==
          CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_1, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, 2.0));
    CHECK(same_state(&state, &snapshot));
    CHECK(store.size == 2);

    root_node = find_node(&store, 500);
    shared_node = find_node(&store, 501);
    check_zero_learning(root_node);
    CHECK(near(shared_node->regret_sums[0], 0.0));
    CHECK(near(shared_node->regret_sums[1], 0.0));
    CHECK(near(shared_node->strategy_sums[0], 1.0));
    CHECK(near(shared_node->strategy_sums[1], 1.0));
    destroy_store(&store);
}

static void test_shared_information_set_uses_fixed_strategy(void) {
    check_shared_information_set(false);
    check_shared_information_set(true);
}

static void test_learning_update_is_atomic(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    InfoNode *node = NULL;
    Utility utility = 94.75;

    initialize_store(&store);
    CHECK(cfr_info_store_get_or_create(&store, 600, 2, &node) ==
          CFR_STATUS_SUCCESS);
    node->regret_sums[0] = 0.0;
    node->regret_sums[1] = -DBL_MAX;
    node->strategy_sums[0] = 0.25;
    node->strategy_sums[1] = 0.75;
    CHECK(traversal_game_state_init_atomic(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;

    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(utility == 94.75);
    CHECK(same_state(&state, &snapshot));
    CHECK(store.size == 1);
    CHECK(node->regret_sums[0] == 0.0);
    CHECK(node->regret_sums[1] == -DBL_MAX);
    CHECK(node->strategy_sums[0] == 0.25);
    CHECK(node->strategy_sums[1] == 0.75);
    destroy_store(&store);
}

static void test_strategy_validation_precedes_regret_publish(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    InfoNode *node = NULL;
    Utility utility = 94.8;

    initialize_store(&store);
    CHECK(cfr_info_store_get_or_create(&store, 100, 2, &node) ==
          CFR_STATUS_SUCCESS);
    node->strategy_sums[0] = -1.0;
    node->strategy_sums[1] = 0.0;
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;

    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(utility == 94.8);
    CHECK(same_state(&state, &snapshot));
    CHECK(node->regret_sums[0] == 0.0);
    CHECK(node->regret_sums[1] == 0.0);
    CHECK(node->strategy_sums[0] == -1.0);
    CHECK(node->strategy_sums[1] == 0.0);
    destroy_store(&store);
}

static void test_excessive_action_limit_is_rejected(void) {
    Game game = *traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    Utility utility = 94.875;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    game.max_legal_actions = 65;
    CHECK(cfr_traverse(&game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 94.875);
    CHECK(store.size == 0);
    CHECK(same_state(&state, &snapshot));
    destroy_store(&store);
}

static void test_root_validation_precedes_traversal(void) {
    const Game *descriptor = traversal_game_descriptor();
    Game game = *descriptor;
    GameOperations operations = *descriptor->operations;
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    Utility utility = 94.9;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    operations.validate_state = reject_root_state;
    game.operations = &operations;
    validation_calls = 0;

    CHECK(cfr_traverse(&game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(validation_calls == 1);
    CHECK(utility == 94.9);
    CHECK(store.size == 0);
    CHECK(same_state(&state, &snapshot));
    destroy_store(&store);
}

static void test_trusted_operations_follow_root_validation(void) {
    const Game *descriptor = traversal_game_descriptor();
    Game game = *descriptor;
    GameOperations public_operations = *descriptor->operations;
    GameOperations trusted_operations = *descriptor->operations;
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    Utility utility = 94.95;
    bool terminal = true;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    public_operations.validate_state = accept_root_state;
    trusted_operations.is_terminal = count_trusted_terminal_query;
    game.operations = &public_operations;
    game.trusted_operations = &trusted_operations;
    trusted_delegate = descriptor->operations;
    validation_calls = 0;
    trusted_terminal_queries = 0;

    CHECK(cfr_game_is_terminal(&game,
                               traversal_game_state_as_public_const(&state),
                               &terminal) == CFR_STATUS_SUCCESS);
    CHECK(!terminal);
    CHECK(validation_calls == 0);
    CHECK(trusted_terminal_queries == 0);

    CHECK(cfr_traverse(&game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_SUCCESS);
    CHECK(validation_calls == 1);
    CHECK(trusted_terminal_queries > 0);
    CHECK(near(utility, 1.0));
    CHECK(same_state(&state, &snapshot));

    trusted_delegate = NULL;
    destroy_store(&store);
}

static void test_error_after_apply_restores_state(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    Utility utility = 95.0;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    traversal_game_fail_after_apply(&state, CFR_STATUS_NUMERIC_ERROR);
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(utility == 95.0);
    CHECK(same_state(&state, &snapshot));
    destroy_store(&store);
}

static void test_error_after_completed_branch_restores_state(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    InfoNode *root_node;
    Utility utility = 95.5;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    traversal_game_fail_after_action(&state, TRAVERSAL_ACTION_ENTER,
                                     CFR_STATUS_BUFFER_TOO_SMALL);
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(utility == 95.5);
    CHECK(same_state(&state, &snapshot));
    CHECK(store.size == 1);
    root_node = find_node(&store, 100);
    check_zero_learning(root_node);
    destroy_store(&store);
}

static void test_undo_error_is_propagated(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    InfoStore store;
    Utility utility = 95.75;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    traversal_game_fail_undo(&state, CFR_STATUS_NUMERIC_ERROR);
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_NUMERIC_ERROR);
    CHECK(utility == 95.75);
    CHECK(state.phase == TRAVERSAL_PHASE_TERMINAL);
    CHECK(state.history_count == 1);
    CHECK(state.last_action == TRAVERSAL_ACTION_EXIT);
    destroy_store(&store);
}

static void test_chance_is_rejected(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    Utility utility = 96.0;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    state.phase = TRAVERSAL_PHASE_CHANCE;
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 96.0);
    CHECK(store.size == 0);
    CHECK(same_state(&state, &snapshot));
    destroy_store(&store);
}

static void test_invalid_arguments_and_outputs(void) {
    const Game *game = traversal_game_descriptor();
    Game zero_actions_game = *game;
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    InfoNode *node = NULL;
    Utility utility = 97.0;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse(NULL, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse(game, NULL, &store, CFR_PLAYER_0, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), NULL,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, NULL) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       (Player)99, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 97.0);
    CHECK(store.size == 0);
    CHECK(same_state(&state, &snapshot));

    zero_actions_game.max_legal_actions = 0;
    CHECK(cfr_traverse(&zero_actions_game,
                       traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 97.0);
    CHECK(store.size == 0);
    CHECK(same_state(&state, &snapshot));

    CHECK(cfr_info_store_get_or_create(&store, 100, 1, &node) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 97.0);
    CHECK(node->action_count == 1);
    CHECK(same_state(&state, &snapshot));
    destroy_store(&store);
}

static void test_inconsistent_required_count_is_rejected(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    Utility utility = 97.5;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    traversal_game_force_required_count(&state, 3);
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 97.5);
    CHECK(store.size == 0);
    CHECK(same_state(&state, &snapshot));
    destroy_store(&store);
}

static void test_successive_players_share_store_without_cross_updates(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    TraversalGameState snapshot;
    InfoStore store;
    InfoNode *player_0_node;
    InfoNode *player_1_node;
    Utility utility = 97.75;

    initialize_store(&store);
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    snapshot = state;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_1, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, -1.0));
    CHECK(same_state(&state, &snapshot));
    player_0_node = find_node(&store, 100);
    player_1_node = find_node(&store, 200);
    check_zero_learning(player_0_node);
    CHECK(near(player_1_node->regret_sums[0], -0.5));
    CHECK(near(player_1_node->regret_sums[1], 0.5));
    CHECK(near(player_1_node->strategy_sums[0], 0.5));
    CHECK(near(player_1_node->strategy_sums[1], 0.5));

    utility = 97.75;
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_0, &utility) == CFR_STATUS_SUCCESS);
    CHECK(near(utility, 0.5));
    CHECK(same_state(&state, &snapshot));
    CHECK(near(player_0_node->regret_sums[0], 1.5));
    CHECK(near(player_0_node->regret_sums[1], -1.5));
    CHECK(near(player_0_node->strategy_sums[0], 0.5));
    CHECK(near(player_0_node->strategy_sums[1], 0.5));
    CHECK(near(player_1_node->regret_sums[0], -0.5));
    CHECK(near(player_1_node->regret_sums[1], 0.5));
    CHECK(near(player_1_node->strategy_sums[0], 0.5));
    CHECK(near(player_1_node->strategy_sums[1], 0.5));
    destroy_store(&store);
}

static void test_non_finite_terminal_utility(void) {
    const Game *game = traversal_game_descriptor();
    const Utility invalid_utilities[] = {NAN, INFINITY, -INFINITY};
    size_t index;

    for (index = 0;
         index < sizeof(invalid_utilities) / sizeof(invalid_utilities[0]);
         index += 1) {
        TraversalGameState state;
        TraversalGameState snapshot;
        InfoStore store;
        Utility utility = 98.0;

        initialize_store(&store);
        CHECK(traversal_game_state_init_terminal(&state,
                                                 invalid_utilities[index]) ==
              CFR_STATUS_SUCCESS);
        snapshot = state;
        CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                           CFR_PLAYER_0, &utility) ==
              CFR_STATUS_NUMERIC_ERROR);
        CHECK(utility == 98.0);
        CHECK(store.size == 0);
        CHECK(same_state(&state, &snapshot));
        destroy_store(&store);
    }
}

#ifdef CFR_TEST_WRAP_ALLOCATOR
static void test_no_temporary_allocations(void) {
    const Game *game = traversal_game_descriptor();
    TraversalGameState state;
    InfoStore store;
    InfoNode *node = NULL;
    Utility utility = 99.0;
    size_t live_before;

    initialize_store(&store);
    CHECK(cfr_info_store_get_or_create(&store, 100, 2, &node) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_get_or_create(&store, 200, 2, &node) ==
          CFR_STATUS_SUCCESS);
    live_before = test_allocator_live_allocations();
    CHECK(traversal_game_state_init(&state) == CFR_STATUS_SUCCESS);
    CHECK(cfr_traverse(game, traversal_game_state_as_public(&state), &store,
                       CFR_PLAYER_1, &utility) == CFR_STATUS_SUCCESS);
    CHECK(test_allocator_live_allocations() == live_before);
    destroy_store(&store);
    CHECK(test_allocator_live_allocations() == 0);
}
#endif

int test_traversal(void) {
    failures = 0;

    test_terminal_utility_and_signs();
    test_player_1_learning();
    test_player_0_learning();
    test_own_reach_weights_average_strategy();
    test_shared_information_set_uses_fixed_strategy();
    test_learning_update_is_atomic();
    test_strategy_validation_precedes_regret_publish();
    test_excessive_action_limit_is_rejected();
    test_root_validation_precedes_traversal();
    test_trusted_operations_follow_root_validation();
    test_error_after_apply_restores_state();
    test_error_after_completed_branch_restores_state();
    test_undo_error_is_propagated();
    test_chance_is_rejected();
    test_invalid_arguments_and_outputs();
    test_inconsistent_required_count_is_rejected();
    test_successive_players_share_store_without_cross_updates();
    test_non_finite_terminal_utility();
#ifdef CFR_TEST_WRAP_ALLOCATOR
    test_no_temporary_allocations();
#endif

    return failures;
}

#ifdef CFR_TEST_TRAVERSAL_STANDALONE
int main(void) {
    const int result = test_traversal();

    if (result != 0) {
        fprintf(stderr, "%d traversal checks failed.\n", result);
        return 1;
    }
    puts("All traversal tests completed successfully.");
    return 0;
}
#endif

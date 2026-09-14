#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/mccfr.h"
#include "cfr/trainer.h"
#include "../src/mccfr_sequential_internal.h"
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

typedef struct {
    size_t depth;
    unsigned int prefix;
} MultiplayerState;

typedef struct {
    Player actors[5];
    size_t decisions;
    bool zero_utility;
    size_t *terminal_calls;
    size_t fail_after;
} MultiplayerContext;

static GameState *public_state(MultiplayerState *state) {
    return (GameState *)state;
}

static Status is_terminal(const void *raw_context, const GameState *raw_state,
                          bool *result) {
    const MultiplayerContext *context = raw_context;
    const MultiplayerState *state = (const MultiplayerState *)raw_state;
    *result = state->depth == context->decisions;
    return CFR_STATUS_SUCCESS;
}

static Status terminal_utility(const void *raw_context,
                               const GameState *raw_state, Player player,
                               Utility *result) {
    const MultiplayerContext *context = raw_context;
    const MultiplayerState *state = (const MultiplayerState *)raw_state;
    if (state->depth != context->decisions || (unsigned int)player >= 4)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (context->terminal_calls != NULL) {
        *context->terminal_calls += 1;
        if (*context->terminal_calls > context->fail_after)
            return CFR_STATUS_ILLEGAL_ACTION;
    }
    double value = 0.0;
    if (!context->zero_utility) {
        for (size_t index = 0; index < context->decisions; index += 1) {
            const unsigned int bit =
                (state->prefix >> (context->decisions - index - 1)) & 1;
            const double team_sign = (context->actors[index] % 2) ? -1.0 : 1.0;
            value += team_sign * (bit ? 1.0 : -1.0);
        }
    }
    *result = (player % 2) ? -value : value;
    return CFR_STATUS_SUCCESS;
}

static Status current_actor(const void *raw_context, const GameState *raw_state,
                            Actor *result) {
    const MultiplayerContext *context = raw_context;
    const MultiplayerState *state = (const MultiplayerState *)raw_state;
    if (state->depth >= context->decisions)
        return CFR_STATUS_INVALID_ARGUMENT;
    *result = (Actor){.kind = CFR_ACTOR_PLAYER,
                      .player = context->actors[state->depth]};
    return CFR_STATUS_SUCCESS;
}

static Status legal_actions(const void *context, const GameState *state,
                            Action *actions, size_t capacity, size_t *count) {
    (void)context;
    (void)state;
    *count = 2;
    if (capacity < 2)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    actions[0] = 0;
    actions[1] = 1;
    return CFR_STATUS_SUCCESS;
}

static Status apply_action(const void *raw_context, GameState *raw_state,
                           Action action) {
    const MultiplayerContext *context = raw_context;
    MultiplayerState *state = (MultiplayerState *)raw_state;
    if (state->depth >= context->decisions || (action != 0 && action != 1))
        return CFR_STATUS_ILLEGAL_ACTION;
    state->prefix = state->prefix * 2 + (unsigned int)action;
    state->depth += 1;
    return CFR_STATUS_SUCCESS;
}

static Status undo_action(const void *context, GameState *raw_state) {
    (void)context;
    MultiplayerState *state = (MultiplayerState *)raw_state;
    if (state->depth == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    state->prefix /= 2;
    state->depth -= 1;
    return CFR_STATUS_SUCCESS;
}

static Status chance_probability(const void *context, const GameState *state,
                                 Action action, Probability *result) {
    (void)context;
    (void)state;
    (void)action;
    (void)result;
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status information_set_key(const void *context, const GameState *raw_state,
                                  InfoSetKey *result) {
    (void)context;
    const MultiplayerState *state = (const MultiplayerState *)raw_state;
    *result = (InfoSetKey)((1U << state->depth) + state->prefix);
    return CFR_STATUS_SUCCESS;
}

static const GameOperations operations = {
    .is_terminal = is_terminal,
    .terminal_utility = terminal_utility,
    .current_actor = current_actor,
    .legal_actions = legal_actions,
    .apply_action = apply_action,
    .undo_action = undo_action,
    .chance_probability = chance_probability,
    .information_set_key = information_set_key,
};

static Game game_for(const MultiplayerContext *context) {
    return (Game){.operations = &operations,
                  .context = context,
                  .strategic_player_count = 4,
                  .max_legal_actions = 2,
                  .strategy_schema_id = "test-four-player-teams-v1"};
}

static InfoNode *node_for(InfoStore *store, InfoSetKey key) {
    InfoNode *node = NULL;
    CHECK(cfr_info_store_get_or_create(store, key, 2, &node) ==
          CFR_STATUS_SUCCESS);
    return node;
}

static void set_policies(InfoStore *store, const MultiplayerContext *context,
                         const double *probability_zero) {
    for (size_t depth = 0; depth < context->decisions; depth += 1) {
        const InfoSetKey first = (InfoSetKey)(1U << depth);
        for (InfoSetKey key = first; key < 2 * first; key += 1) {
            InfoNode *node = node_for(store, key);
            node->regret_sums[0] = probability_zero[depth];
            node->regret_sums[1] = 1.0 - probability_zero[depth];
        }
    }
}

static void compare_stores(InfoStore *left, InfoStore *right) {
    InfoStoreStats left_stats;
    InfoStoreStats right_stats;
    CHECK(cfr_info_store_get_stats(left, &left_stats) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_store_get_stats(right, &right_stats) == CFR_STATUS_SUCCESS);
    CHECK(left_stats.size == right_stats.size);
    for (InfoSetKey key = 1; key < 16; key += 1) {
        InfoNode *a = node_for(left, key);
        InfoNode *b = node_for(right, key);
        for (size_t action = 0; action < 2; action += 1) {
            CHECK(a->regret_sums[action] == b->regret_sums[action]);
            CHECK(a->strategy_sums[action] == b->strategy_sums[action]);
        }
    }
}

static void test_exact_counterfactual_reach(void) {
    const MultiplayerContext context = {.actors = {0, 1, 2, 3}, .decisions = 4};
    const Game game = game_for(&context);
    MultiplayerState state = {0};
    InfoStore store = {0};
    CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
    const double policies[] = {0.8, 0.7, 0.6, 0.5};
    set_policies(&store, &context, policies);
    Utility utility = 0.0;
    CHECK(cfr_traverse(&game, public_state(&state), &store, CFR_PLAYER_3,
                       &utility) == CFR_STATUS_SUCCESS);
    InfoNode *last = node_for(&store, 15);
    CHECK(fabs(last->regret_sums[0] - (0.5 - 0.2 * 0.3 * 0.4)) < 1e-12);
    CHECK(fabs(last->regret_sums[1] - (0.5 + 0.2 * 0.3 * 0.4)) < 1e-12);
    CHECK(last->strategy_sums[0] == 0.5);
    CHECK(last->strategy_sums[1] == 0.5);
    CHECK(node_for(&store, 7)->strategy_sums[0] == 0.0);
    CHECK(state.depth == 0 && state.prefix == 0);
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
}

static Status sampled_traverse(bool concurrent, const Game *game,
                               MultiplayerState *state, InfoStore *store,
                               MccfrRng *rng, Utility *utility,
                               TraversalStats *stats) {
    if (concurrent) {
        return cfr_mccfr_external_traverse_with_stats(
            game, public_state(state), store, CFR_PLAYER_3, rng, utility, stats);
    }
    return cfr_mccfr_sequential_external_traverse_with_stats(
        game, public_state(state), store, CFR_PLAYER_3, rng, utility, stats);
}

static void test_multiplayer_average(bool concurrent, bool zero_opponent_reach,
                                     bool repeated_target) {
    MultiplayerContext context = {
        .actors = {0, 1, 2, 3}, .decisions = 4, .zero_utility = true};
    if (repeated_target) {
        context = (MultiplayerContext){.actors = {3, 0, 1, 2, 3},
                                       .decisions = 5,
                                       .zero_utility = true};
    }
    const Game game = game_for(&context);
    MultiplayerState state = {0};
    InfoStore store = {0};
    MccfrRng rng;
    CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_mccfr_rng_seed(&rng, 314159) == CFR_STATUS_SUCCESS);
    for (size_t phase = 0; phase < 2; phase += 1) {
        const double external = zero_opponent_reach ? (phase ? 0.0 : 1.0)
                                                   : (phase ? 0.05 : 0.95);
        double policies[] = {external, external, external,
                              phase ? 0.2 : 0.8, 0.0};
        if (repeated_target) {
            policies[0] = phase ? 0.9 : 0.1;
            policies[1] = policies[2] = policies[3] = external;
            policies[4] = phase ? 0.1 : 0.9;
        }
        set_policies(&store, &context, policies);
        for (size_t iteration = 0; iteration < 8000; iteration += 1) {
            Utility utility = 123.0;
            TraversalStats stats = {0};
            const Status status = sampled_traverse(
                concurrent, &game, &state, &store, &rng, &utility, &stats);
            CHECK(status == CFR_STATUS_SUCCESS);
            if (status != CFR_STATUS_SUCCESS)
                break;
            CHECK(utility == 0.0);
            CHECK(state.depth == 0 && state.prefix == 0);
        }
    }
    const InfoSetKey first = (InfoSetKey)(1U << (context.decisions - 1));
    for (InfoSetKey key = first; key < 2 * first; key += 1) {
        InfoNode *node = node_for(&store, key);
        Probability average[2];
        CHECK(cfr_info_node_average_strategy(node, average, 2) ==
              CFR_STATUS_SUCCESS);
        const double expected = repeated_target ? (key < 24 ? 0.18 : 0.82) : 0.5;
        CHECK(node->strategy_sums[0] + node->strategy_sums[1] > 1000.0);
        CHECK(fabs(average[0] - expected) < 0.035);
    }
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
}

static void test_average_failure_is_atomic(bool concurrent) {
    size_t terminal_calls = 0;
    MultiplayerContext context = {.actors = {0, 1, 2, 3},
                                   .decisions = 4,
                                   .terminal_calls = &terminal_calls,
                                   .fail_after = 2};
    const Game game = game_for(&context);
    MultiplayerState state = {0};
    InfoStore store = {0};
    MccfrRng rng;
    CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_mccfr_rng_seed(&rng, 42) == CFR_STATUS_SUCCESS);
    const MccfrRng before = rng;
    Utility utility = 123.0;
    TraversalStats stats = {.visited_nodes = 456};
    CHECK(sampled_traverse(concurrent, &game, &state, &store, &rng, &utility,
                           &stats) == CFR_STATUS_ILLEGAL_ACTION);
    CHECK(terminal_calls == 3);
    CHECK(state.depth == 0 && state.prefix == 0);
    CHECK(rng.state == before.state);
    CHECK(utility == 123.0 && stats.visited_nodes == 456);
    for (InfoSetKey key = 1; key < 16; key += 1) {
        InfoNode *node = node_for(&store, key);
        CHECK(node->regret_sums[0] == 0.0 && node->regret_sums[1] == 0.0);
        CHECK(node->strategy_sums[0] == 0.0 && node->strategy_sums[1] == 0.0);
    }
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
}

static void test_trainers_and_checkpoint(void) {
    const MultiplayerContext context = {.actors = {0, 1, 2, 3}, .decisions = 4};
    const Game game = game_for(&context);
    for (size_t variant = 0; variant < 4; variant += 1) {
        MultiplayerState state = {0};
        InfoStore store = {0};
        Trainer trainer;
        CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
        Status status;
        if (variant == 0)
            status = cfr_trainer_init(&trainer, &game, public_state(&state), &store);
        else if (variant == 1)
            status = cfr_trainer_init_plus(&trainer, &game, public_state(&state),
                                           &store);
        else
            status = cfr_trainer_init_mccfr(&trainer, &game, public_state(&state),
                                            &store, 101);
        CHECK(status == CFR_STATUS_SUCCESS);
        status = variant == 3 ? cfr_trainer_run_concurrent(&trainer, 400)
                              : cfr_trainer_run(&trainer, 400);
        CHECK(status == CFR_STATUS_SUCCESS);
        CHECK(trainer.stats.iterations == 400 && trainer.stats.traversals == 1600);
        CHECK(trainer.stats.errors == 0);
        for (InfoSetKey key = 1; key < 16; key = key * 2 + 1) {
            Probability average[2];
            CHECK(cfr_info_node_average_strategy(node_for(&store, key), average,
                                                 2) == CFR_STATUS_SUCCESS);
            CHECK(average[1] > 0.95);
        }
        FILE *checkpoint = tmpfile();
        CHECK(checkpoint != NULL);
        if (checkpoint != NULL) {
            CHECK(cfr_checkpoint_write(checkpoint, &trainer) == CFR_STATUS_SUCCESS);
            CHECK(fseek(checkpoint, 0, SEEK_SET) == 0);
            MultiplayerState loaded_state = {0};
            InfoStore loaded_store = {0};
            Trainer loaded;
            CHECK(cfr_checkpoint_read(checkpoint, &game, public_state(&loaded_state),
                                       &loaded_store, &loaded) == CFR_STATUS_SUCCESS);
            CHECK(cfr_trainer_run(&trainer, 20) == CFR_STATUS_SUCCESS);
            CHECK(cfr_trainer_run(&loaded, 20) == CFR_STATUS_SUCCESS);
            CHECK(trainer.mccfr_rng.state == loaded.mccfr_rng.state);
            CHECK(trainer.stats.visited_nodes == loaded.stats.visited_nodes);
            compare_stores(&store, &loaded_store);
            CHECK(cfr_info_store_destroy(&loaded_store) == CFR_STATUS_SUCCESS);
            CHECK(fclose(checkpoint) == 0);
        }
        CHECK(state.depth == 0 && state.prefix == 0);
        CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
    }
}

typedef struct {
    Trainer trainer;
    MultiplayerState state;
    Status status;
} MultiplayerWorker;

static void *run_multiplayer_worker(void *raw_worker) {
    MultiplayerWorker *worker = raw_worker;
    worker->status = cfr_trainer_run_concurrent(&worker->trainer, 200);
    return NULL;
}

static void test_shared_multiplayer_store(void) {
    const MultiplayerContext context = {.actors = {0, 1, 2, 3}, .decisions = 4};
    const Game game = game_for(&context);
    InfoStore store = {0};
    MultiplayerWorker workers[2] = {0};
    pthread_t threads[2];
    size_t created = 0;
    CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
    for (size_t index = 0; index < 2; index += 1) {
        CHECK(cfr_trainer_init_mccfr(
                  &workers[index].trainer, &game,
                  public_state(&workers[index].state), &store, 500 + index) ==
              CFR_STATUS_SUCCESS);
        const int status = pthread_create(&threads[index], NULL,
                                           run_multiplayer_worker, &workers[index]);
        CHECK(status == 0);
        if (status != 0)
            break;
        created += 1;
    }
    for (size_t index = 0; index < created; index += 1) {
        CHECK(pthread_join(threads[index], NULL) == 0);
        CHECK(workers[index].status == CFR_STATUS_SUCCESS);
        CHECK(workers[index].trainer.stats.traversals == 800);
        CHECK(workers[index].state.depth == 0 && workers[index].state.prefix == 0);
    }
    InfoNode *root = node_for(&store, 1);
    /* The root's own-reach and sampling-reach weights are both exactly one. */
    CHECK(root->strategy_sums[0] + root->strategy_sums[1] ==
          (double)(created * 200));
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
}

static void test_invalid_players_and_evaluation(void) {
    MultiplayerContext context = {.actors = {0, 1, 2, 3}, .decisions = 4};
    Game game = game_for(&context);
    MultiplayerState state = {0};
    InfoStore store = {0};
    MccfrRng rng;
    Utility utility = 123.0;
    EvaluationMetrics metrics = {.nash_conv = 456.0};
    CHECK(cfr_info_store_init(&store) == CFR_STATUS_SUCCESS);
    CHECK(cfr_mccfr_rng_seed(&rng, 42) == CFR_STATUS_SUCCESS);
    CHECK(cfr_traverse(&game, public_state(&state), &store, (Player)4,
                       &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_mccfr_external_traverse(&game, public_state(&state), &store,
                                      (Player)4, &rng, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    context.actors[0] = (Player)4;
    CHECK(cfr_traverse(&game, public_state(&state), &store, CFR_PLAYER_3,
                       &utility) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_mccfr_external_traverse(&game, public_state(&state), &store,
                                      CFR_PLAYER_3, &rng, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    context.actors[0] = CFR_PLAYER_0;
    /* Even a terminal four-player root must be rejected by the two-player API. */
    state.depth = context.decisions;
    CHECK(cfr_evaluation_profile_value(&game, public_state(&state), &store,
                                        CFR_PLAYER_0, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_best_response_value(&game, public_state(&state), &store,
                                              CFR_PLAYER_0, &utility) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_metrics(&game, public_state(&state), &store,
                                 &metrics) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_evaluation_metrics_with_unvisited_uniform(
              &game, public_state(&state), &store, &metrics) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(utility == 123.0 && metrics.nash_conv == 456.0 && store.size == 0);
    CHECK(cfr_info_store_destroy(&store) == CFR_STATUS_SUCCESS);
}

int test_multiplayer(void) {
    failures = 0;
    test_exact_counterfactual_reach();
    for (size_t concurrent = 0; concurrent < 2; concurrent += 1) {
        test_multiplayer_average(concurrent != 0, false, false);
        test_multiplayer_average(concurrent != 0, true, false);
        test_multiplayer_average(concurrent != 0, false, true);
        test_average_failure_is_atomic(concurrent != 0);
    }
    test_trainers_and_checkpoint();
    test_shared_multiplayer_store();
    test_invalid_players_and_evaluation();
    return failures;
}

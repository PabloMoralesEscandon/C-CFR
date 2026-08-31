#ifndef CFR_EVALUATION_H
#define CFR_EVALUATION_H

#include <stddef.h>

#include "cfr/game.h"
#include "cfr/info_store.h"
#include "cfr/types.h"

/*
 * Contains the metrics from a point-in-time evaluation.
 *
 * The structure contains values and owns no dynamic memory. Each value uses the
 * corresponding player's utility.
 */
typedef struct {
    /* Player zero's value in the average-strategy profile. */
    Utility profile_value_player_0;
    /* Player one's value in the average-strategy profile. */
    Utility profile_value_player_1;
    /* Player zero's best value against the opponent's average strategy. */
    Utility best_response_value_player_0;
    /* Player one's best value against the opponent's average strategy. */
    Utility best_response_value_player_1;
    /* Difference between best response and profile value for player zero. */
    Utility improvement_player_0;
    /* Difference between best response and profile value for player one. */
    Utility improvement_player_1;
    /* Sum of both players' improvements. */
    Utility nash_conv;
    /* NashConv divided by two, following this project's convention. */
    Utility exploitability;
} EvaluationMetrics;

/*
 * Copies the average strategy for the information set identified by key.
 *
 * The function normalizes strategy_sums and does not use the current strategy.
 * store must be initialized. strategy_out and required_count are required even
 * when capacity is zero. capacity counts elements.
 *
 * Success publishes the strategy and action count. Insufficient capacity
 * preserves strategy_out. In that case, the function publishes the required
 * count and returns CFR_STATUS_BUFFER_TOO_SMALL.
 *
 * A missing key returns CFR_STATUS_NOT_FOUND. The function does not create a
 * node. An invalid argument returns CFR_STATUS_INVALID_ARGUMENT. A negative or
 * non-finite accumulator returns CFR_STATUS_NUMERIC_ERROR. These errors
 * preserve both outputs.
 *
 * The function does not modify the store or its statistics.
 */
Status cfr_evaluation_average_strategy(const InfoStore *store, InfoSetKey key,
                                       Probability *strategy_out,
                                       size_t capacity, size_t *required_count);

/*
 * Evaluates the average-strategy profile from player's perspective.
 *
 * game, state, store, and utility_out are required. player must identify a
 * valid player. game->max_legal_actions must be greater than zero.
 *
 * The function enumerates all branches, including zero-probability branches.
 * It does not use sampling or the current strategy. Temporary memory grows with
 * the complete tree.
 *
 * Success publishes utility_out and restores state. The function does not
 * modify store. An error preserves utility_out.
 *
 * A missing key produces CFR_STATUS_NOT_FOUND. An allocation failure produces
 * CFR_STATUS_OUT_OF_MEMORY. An invalid argument or inconsistent model produces
 * CFR_STATUS_INVALID_ARGUMENT. A non-finite computation produces
 * CFR_STATUS_NUMERIC_ERROR.
 *
 * The function propagates errors from game operations. It restores state before
 * propagating an error from a branch. If undo_action fails, that error takes
 * priority and state can remain unrestored.
 */
Status cfr_evaluation_profile_value(const Game *game, GameState *state,
                                    const InfoStore *store, Player player,
                                    Utility *utility_out);

/*
 * Computes player's best response against the opponent's average strategy.
 *
 * game, state, store, and utility_out are required. player must identify a
 * valid player. game->max_legal_actions must be greater than zero.
 *
 * The best response uses one deterministic action per information set. Every
 * occurrence of the set uses the same action. The function enumerates the
 * complete tree without sampling. Temporary memory grows with the tree.
 *
 * Success publishes utility_out and restores state. The function does not
 * modify store. An error preserves utility_out.
 *
 * A missing key produces CFR_STATUS_NOT_FOUND. An allocation failure produces
 * CFR_STATUS_OUT_OF_MEMORY. An invalid argument or inconsistent model produces
 * CFR_STATUS_INVALID_ARGUMENT. A non-finite computation produces
 * CFR_STATUS_NUMERIC_ERROR.
 *
 * The function propagates errors from game operations. It restores state before
 * propagating an error from a branch. If undo_action fails, that error takes
 * priority and state can remain unrestored.
 */
Status cfr_evaluation_best_response_value(const Game *game, GameState *state,
                                          const InfoStore *store, Player player,
                                          Utility *utility_out);

/*
 * Computes all metrics in a single tree evaluation.
 *
 * game, state, store, and eval_out are required. game->max_legal_actions must be
 * greater than zero. The function uses the average strategy and does not use
 * the current strategy.
 *
 * The function builds one snapshot of the complete tree. It enumerates
 * zero-probability branches without sampling. Temporary memory grows with the
 * complete tree.
 *
 * NashConv is the sum of both improvements. Exploitability is NashConv divided
 * by two, following this project's convention.
 *
 * Success publishes eval_out and restores state. The function does not modify
 * store. An error preserves eval_out.
 *
 * A missing key produces CFR_STATUS_NOT_FOUND. An allocation failure produces
 * CFR_STATUS_OUT_OF_MEMORY. An invalid argument or inconsistent model produces
 * CFR_STATUS_INVALID_ARGUMENT. A non-finite computation produces
 * CFR_STATUS_NUMERIC_ERROR.
 *
 * The function propagates errors from game operations. It restores state before
 * propagating an error from a branch. If undo_action fails, that error takes
 * priority and state can remain unrestored.
 */
Status cfr_evaluation_metrics(const Game *game, GameState *state,
                              const InfoStore *store,
                              EvaluationMetrics *eval_out);

/*
 * Computes all metrics while treating unvisited information sets as uniform.
 *
 * This operation is intended for sampled trainers, whose store can be sparse
 * early in training. It has the cfr_evaluation_metrics contract except that a
 * missing key uses a temporary uniform policy with the state's legal action
 * count. The function does not add nodes to store. Different ordered legal
 * actions for occurrences of the same missing key are rejected.
 */
Status cfr_evaluation_metrics_with_unvisited_uniform(
    const Game *game, GameState *state, const InfoStore *store,
    EvaluationMetrics *eval_out);

#endif

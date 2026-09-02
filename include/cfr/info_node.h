#ifndef CFR_INFO_NODE_H
#define CFR_INFO_NODE_H

#include <stddef.h>

#include "cfr/types.h"

CFR_EXTERN_C_BEGIN

/*
 * Stores the learning data for an information set.
 *
 * The caller owns the structure. The node owns the regret_sums and
 * strategy_sums storage. The caller must destroy the node before discarding
 * the structure. Destruction frees any allocated arrays but not the structure.
 *
 * The caller must not copy an initialized node by assignment. A copy would
 * contain the same pointers and would not have independent ownership.
 *
 * action_count fixes the number of valid indices. Valid indices range from
 * zero through action_count minus one. The adapter must maintain a stable
 * mapping between each index and its legal action. The node does not store that
 * mapping.
 *
 * Strategy and regret operations do not allocate memory. The caller provides
 * the input and output arrays. An operation that returns an error preserves the
 * node and output arrays.
 *
 * All cfr_info_node_* operations can run concurrently on an initialized node.
 * Direct access to the fields or arrays is not synchronized. Initialization
 * and destruction require exclusive ownership of the node.
 *
 * A non-finite numeric argument produces CFR_STATUS_INVALID_ARGUMENT. A
 * non-finite accumulator or arithmetic result produces
 * CFR_STATUS_NUMERIC_ERROR.
 */
typedef struct {
    /* Identifies the observable decision represented by the node. */
    InfoSetKey key;
    /* Number of actions and elements in each internal array. */
    size_t action_count;
    /* Owned array containing one cumulative regret per action. */
    Utility *regret_sums;
    /* Owned array containing one weighted strategy sum per action. */
    double *strategy_sums;
    /* Private lock storage. The caller must not access this field. */
    unsigned char synchronization;
} InfoNode;

/*
 * Initializes node with key and action_count.
 *
 * node must be zero-initialized or previously destroyed. action_count must be
 * greater than zero. The function allocates both internal arrays and sets all
 * elements to zero.
 *
 * An invalid argument produces CFR_STATUS_INVALID_ARGUMENT. An allocation
 * failure produces CFR_STATUS_OUT_OF_MEMORY. An error preserves node.
 */
Status cfr_info_node_init(InfoNode *node, InfoSetKey key, size_t action_count);

/*
 * Destroys node and sets all its fields to zero.
 *
 * node must not be null. A previously destroyed node produces
 * CFR_STATUS_SUCCESS. The function does not free the structure containing the
 * node.
 */
Status cfr_info_node_destroy(InfoNode *node);

/*
 * Computes the current strategy and writes it to strategy_array.
 *
 * node must be initialized. strategy_array must not be null. strategy_capacity
 * counts elements and must be at least node->action_count. A smaller capacity
 * produces CFR_STATUS_BUFFER_TOO_SMALL.
 *
 * The function uses positive regrets. It uses a uniform distribution when no
 * positive regret exists. A stored non-finite regret produces
 * CFR_STATUS_NUMERIC_ERROR.
 */
Status cfr_info_node_current_strategy(const InfoNode *node,
                                      Probability *strategy_array,
                                      size_t strategy_capacity);

/*
 * Checks learning deltas without modifying node.
 *
 * node must be initialized. delta_regret and delta_strategy_sum must contain
 * action_count elements. action_count must equal node->action_count. Every
 * delta must be finite. Every strategy-sum delta must be greater than or equal
 * to zero.
 *
 * The function also checks the resulting accumulators. A non-finite result or
 * a negative strategy sum produces CFR_STATUS_NUMERIC_ERROR. An invalid
 * argument produces CFR_STATUS_INVALID_ARGUMENT. An error preserves node and
 * the input arrays.
 */
Status cfr_info_node_check_deltas(const InfoNode *node,
                                  const Utility *delta_regret,
                                  const double *delta_strategy_sum,
                                  size_t action_count);

/*
 * Validates and applies learning deltas to node.
 *
 * The parameters have the same requirements as cfr_info_node_check_deltas.
 * The function adds each delta to the accumulator at the same index. It does
 * not modify the input arrays.
 *
 * An error preserves all node accumulators. The function does not allocate
 * memory.
 */
Status cfr_info_node_apply_deltas(InfoNode *node, const Utility *delta_regret,
                                  const double *delta_strategy_sum,
                                  size_t action_count);

/*
 * Adds regret_change to the regret at action_index.
 *
 * node must be initialized. action_index must be less than node->action_count.
 * regret_change must be finite. A non-finite arithmetic result produces
 * CFR_STATUS_NUMERIC_ERROR.
 */
Status cfr_info_node_add_regret(InfoNode *node, size_t action_index,
                                Utility regret_change);

/*
 * Accumulates strategy_array with weight.
 *
 * strategy_count must equal node->action_count. Each probability must be finite
 * and in the closed interval from zero to one. The sum must equal one within
 * the module's numeric tolerance. weight must be finite and in the closed
 * interval from zero to one.
 *
 * Invalid input produces CFR_STATUS_INVALID_ARGUMENT. A non-finite or negative
 * accumulator produces CFR_STATUS_NUMERIC_ERROR. A non-finite arithmetic result
 * also produces CFR_STATUS_NUMERIC_ERROR. An error preserves all accumulators.
 */
Status cfr_info_node_accumulate_strategy(InfoNode *node,
                                         const Probability *strategy_array,
                                         size_t strategy_count,
                                         Probability weight);

/*
 * Computes the average strategy and writes it to strategy_array.
 *
 * node must be initialized. strategy_array must not be null. strategy_capacity
 * counts elements and must be at least node->action_count. A smaller capacity
 * produces CFR_STATUS_BUFFER_TOO_SMALL.
 *
 * The function normalizes strategy_sums. It uses a uniform distribution when
 * all accumulators are zero. A non-finite or negative accumulator produces
 * CFR_STATUS_NUMERIC_ERROR.
 */
Status cfr_info_node_average_strategy(const InfoNode *node,
                                      Probability *strategy_array,
                                      size_t strategy_capacity);

CFR_EXTERN_C_END

#endif

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/info_node.h"
#include "info_node_internal.h"

/* Tolerances for validating that a distribution sums to one. */
static const double REL_EPSILON = 1e-8;
static const double ABS_EPSILON = 1e-12;

void cfr_info_node_lock(const InfoNode *node) {
    atomic_bool *lock = (atomic_bool *)&node->synchronization;

    if (!atomic_exchange_explicit(lock, true, memory_order_acquire))
        return;
    do {
        while (atomic_load_explicit(lock, memory_order_relaxed)) {
        }
    } while (atomic_exchange_explicit(lock, true, memory_order_acquire));
}

void cfr_info_node_unlock(const InfoNode *node) {
    atomic_store_explicit((atomic_bool *)&node->synchronization, false,
                          memory_order_release);
}

static bool valid_probability(double probability) {
    if (fabs(probability - 1.0) <= ABS_EPSILON)
        return 1;
    double max = (probability > 1.0) ? probability : 1.0;
    return (fabs(probability - 1.0) <= (max * REL_EPSILON));
}

Status cfr_info_node_init(InfoNode *node, InfoSetKey key, size_t action_count) {
    if (node == NULL || action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action_count > (SIZE_MAX / sizeof(Utility)))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action_count > (SIZE_MAX / sizeof(double)))
        return CFR_STATUS_INVALID_ARGUMENT;

    if (action_count <= CFR_INFO_NODE_INLINE_ACTION_CAPACITY) {
        node->key = key;
        node->action_count = action_count;
        node->regret_sums = node->inline_regret_sums;
        node->strategy_sums = node->inline_strategy_sums;
        for (size_t i = 0; i < CFR_INFO_NODE_INLINE_ACTION_CAPACITY; i++) {
            node->inline_regret_sums[i] = 0.0;
            node->inline_strategy_sums[i] = 0.0;
        }
        atomic_init(&node->synchronization, false);
        return CFR_STATUS_SUCCESS;
    }

    Utility *regret_sums = malloc(sizeof(Utility) * action_count);
    if (regret_sums == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    double *strategy_sums = malloc(sizeof(double) * action_count);
    if (strategy_sums == NULL) {
        free(regret_sums);
        regret_sums = NULL;
        return CFR_STATUS_OUT_OF_MEMORY;
    }
    node->key = key;
    node->action_count = action_count;
    for (size_t i = 0; i < action_count; i++) {
        regret_sums[i] = 0.0;
        strategy_sums[i] = 0.0;
    }
    node->regret_sums = regret_sums;
    node->strategy_sums = strategy_sums;
    for (size_t i = 0; i < CFR_INFO_NODE_INLINE_ACTION_CAPACITY; i++) {
        node->inline_regret_sums[i] = 0.0;
        node->inline_strategy_sums[i] = 0.0;
    }
    atomic_init(&node->synchronization, false);
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_destroy(InfoNode *node) {
    if (node == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (node->regret_sums != node->inline_regret_sums)
        free(node->regret_sums);
    if (node->strategy_sums != node->inline_strategy_sums)
        free(node->strategy_sums);
    node->key = 0;
    node->action_count = 0;
    node->regret_sums = NULL;
    node->strategy_sums = NULL;
    for (size_t i = 0; i < CFR_INFO_NODE_INLINE_ACTION_CAPACITY; i++) {
        node->inline_regret_sums[i] = 0.0;
        node->inline_strategy_sums[i] = 0.0;
    }
    atomic_init(&node->synchronization, false);

    return CFR_STATUS_SUCCESS;
}

static Status current_strategy_locked(const InfoNode *node,
                                      Probability *strategy_array) {
    double maximum = 0.0;
    for (size_t i = 0; i < node->action_count; i++) {
        if (!isfinite(node->regret_sums[i]))
            return CFR_STATUS_NUMERIC_ERROR;
        if (node->regret_sums[i] > maximum)
            maximum = node->regret_sums[i];
    }
    if (maximum == 0.0) {
        for (size_t i = 0; i < node->action_count; i++) {
            strategy_array[i] = 1.0 / node->action_count;
        }
        return CFR_STATUS_SUCCESS;
    }
    double sum = 0.0;
    for (size_t i = 0; i < node->action_count; i++) {
        if (node->regret_sums[i] > 0.0) {
            sum += node->regret_sums[i] / maximum;
        }
    }
    if (!isfinite(sum) || sum <= 0.0)
        return CFR_STATUS_NUMERIC_ERROR;

    for (size_t i = 0; i < node->action_count; i++) {
        if (node->regret_sums[i] > 0.0) {
            strategy_array[i] = (node->regret_sums[i] / maximum) / sum;
        } else
            strategy_array[i] = 0.0;
    }

    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_current_strategy(const InfoNode *node,
                                      Probability *strategy_array,
                                      size_t strategy_capacity) {
    Status status;

    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || strategy_array == NULL ||
        node->action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (strategy_capacity < node->action_count)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    cfr_info_node_lock(node);
    status = current_strategy_locked(node, strategy_array);
    cfr_info_node_unlock(node);
    return status;
}

Status cfr_info_node_check_deltas_locked(
    const InfoNode *node, const Utility *delta_regret,
    const double *delta_strategy_sum, size_t action_count) {
    for (size_t i = 0; i < action_count; i++) {
        if (!isfinite(delta_regret[i]) || !isfinite(delta_strategy_sum[i]) ||
            (delta_strategy_sum[i] < 0))
            return CFR_STATUS_NUMERIC_ERROR;
        Utility regret_candidate = node->regret_sums[i] + delta_regret[i];
        if (!isfinite(regret_candidate))
            return CFR_STATUS_NUMERIC_ERROR;
        double strategy_candidate =
            node->strategy_sums[i] + delta_strategy_sum[i];
        if (!isfinite(strategy_candidate) || (strategy_candidate < 0))
            return CFR_STATUS_NUMERIC_ERROR;
    }
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_check_deltas(const InfoNode *node,
                                  const Utility *delta_regret,
                                  const double *delta_strategy_sum,
                                  size_t action_count) {
    Status status;

    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || action_count == 0 ||
        delta_regret == NULL || delta_strategy_sum == NULL ||
        action_count != node->action_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    cfr_info_node_lock(node);
    status = cfr_info_node_check_deltas_locked(
        node, delta_regret, delta_strategy_sum, action_count);
    cfr_info_node_unlock(node);
    return status;
}

Status cfr_info_node_apply_deltas(InfoNode *node, const Utility *delta_regret,
                                  const double *delta_strategy_sum,
                                  size_t action_count) {
    Status status;

    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || action_count == 0 ||
        delta_regret == NULL || delta_strategy_sum == NULL ||
        action_count != node->action_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    cfr_info_node_lock(node);
    status = cfr_info_node_check_deltas_locked(
        node, delta_regret, delta_strategy_sum, action_count);
    if (status == CFR_STATUS_SUCCESS) {
        cfr_info_node_apply_validated_deltas(
            node, delta_regret, delta_strategy_sum, action_count);
    }
    cfr_info_node_unlock(node);
    return status;
}

void cfr_info_node_apply_validated_deltas(
    InfoNode *node, const Utility *delta_regret,
    const double *delta_strategy_sum, size_t action_count) {
    for (size_t i = 0; i < action_count; i++) {
        node->regret_sums[i] += delta_regret[i];
        node->strategy_sums[i] += delta_strategy_sum[i];
    }
}

Status cfr_info_node_add_regret(InfoNode *node, size_t action_index,
                                Utility regret_change) {
    Status status = CFR_STATUS_SUCCESS;

    if (node == NULL || node->regret_sums == NULL || node->action_count == 0 ||
        node->strategy_sums == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action_index >= node->action_count || !isfinite(regret_change))
        return CFR_STATUS_INVALID_ARGUMENT;
    cfr_info_node_lock(node);
    Utility new_regret = node->regret_sums[action_index] + regret_change;
    if (!isfinite(new_regret)) {
        status = CFR_STATUS_NUMERIC_ERROR;
    } else {
        node->regret_sums[action_index] = new_regret;
    }
    cfr_info_node_unlock(node);
    return status;
}

Status cfr_info_node_accumulate_strategy(InfoNode *node,
                                         const Probability *strategy_array,
                                         size_t strategy_count,
                                         Probability weight) {
    Status status = CFR_STATUS_SUCCESS;

    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || strategy_array == NULL ||
        node->action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (strategy_count != node->action_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!isfinite(weight) || (0.0 > weight) || (1.0 < weight))
        return CFR_STATUS_INVALID_ARGUMENT;

    cfr_info_node_lock(node);
    double sum = 0.0;
    for (size_t i = 0; i < node->action_count; i++) {
        if (!isfinite(node->strategy_sums[i]) || node->strategy_sums[i] < 0.0) {
            status = CFR_STATUS_NUMERIC_ERROR;
            goto cleanup;
        }
        if (!isfinite(strategy_array[i]) || strategy_array[i] < 0.0 ||
            strategy_array[i] > 1.0) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        double increment = weight * strategy_array[i];
        double candidate = node->strategy_sums[i] + increment;
        if (!(isfinite(increment) && isfinite(candidate))) {
            status = CFR_STATUS_NUMERIC_ERROR;
            goto cleanup;
        }
        sum += strategy_array[i];
    }
    if (!isfinite(sum) || !valid_probability(sum)) {
        status = CFR_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }
    for (size_t i = 0; i < node->action_count; i++) {
        node->strategy_sums[i] += weight * strategy_array[i];
    }

cleanup:
    cfr_info_node_unlock(node);
    return status;
}

static Status average_strategy_locked(const InfoNode *node,
                                      Probability *strategy_array) {
    double maximum = 0.0;
    for (size_t i = 0; i < node->action_count; i++) {
        if (!isfinite(node->strategy_sums[i]) || (node->strategy_sums[i] < 0.0))
            return CFR_STATUS_NUMERIC_ERROR;
        if (node->strategy_sums[i] > maximum)
            maximum = node->strategy_sums[i];
    }
    if (maximum == 0.0) {
        for (size_t i = 0; i < node->action_count; i++) {
            strategy_array[i] = 1.0 / node->action_count;
        }
        return CFR_STATUS_SUCCESS;
    }
    double sum = 0.0;
    for (size_t i = 0; i < node->action_count; i++) {
        if (node->strategy_sums[i] > 0.0) {
            sum += node->strategy_sums[i] / maximum;
        }
    }
    if (!isfinite(sum) || sum <= 0.0)
        return CFR_STATUS_NUMERIC_ERROR;

    for (size_t i = 0; i < node->action_count; i++) {
        if (node->strategy_sums[i] > 0.0) {
            strategy_array[i] = (node->strategy_sums[i] / maximum) / sum;
        } else
            strategy_array[i] = 0.0;
    }

    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_average_strategy(const InfoNode *node,
                                      Probability *strategy_array,
                                      size_t strategy_capacity) {
    Status status;

    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || strategy_array == NULL ||
        node->action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (strategy_capacity < node->action_count)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    cfr_info_node_lock(node);
    status = average_strategy_locked(node, strategy_array);
    cfr_info_node_unlock(node);
    return status;
}

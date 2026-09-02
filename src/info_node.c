#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/info_node.h"
#include "info_node_internal.h"
#include "spin_wait_internal.h"

/* Tolerances for validating that a distribution sums to one. */
static const double REL_EPSILON = 1e-8;
static const double ABS_EPSILON = 1e-12;

void cfr_info_node_lock(const InfoNode *node) {
    unsigned char *lock = (unsigned char *)&node->synchronization;
    unsigned char expected = 0;
    size_t spin_count = 0;

    while (!__atomic_compare_exchange_n(lock, &expected, 1, true,
                                         __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED)) {
        expected = 0;
        while (__atomic_load_n(lock, __ATOMIC_RELAXED) != 0)
            cfr_spin_wait(&spin_count);
    }
}

void cfr_info_node_unlock(const InfoNode *node) {
    __atomic_store_n((unsigned char *)&node->synchronization, 0,
                     __ATOMIC_RELEASE);
}

static bool valid_probability(double probability) {
    if (fabs(probability - 1.0) <= ABS_EPSILON)
        return 1;
    double max = (probability > 1.0) ? probability : 1.0;
    return (fabs(probability - 1.0) <= (max * REL_EPSILON));
}

static Utility *attached_regret_sums(InfoNode *node) {
    return (Utility *)(void *)((unsigned char *)node + sizeof(*node));
}

static const Utility *attached_regret_sums_const(const InfoNode *node) {
    return (const Utility *)(const void *)((const unsigned char *)node +
                                           sizeof(*node));
}

static double *attached_strategy_sums(InfoNode *node, size_t action_count) {
    return (double *)(void *)(attached_regret_sums(node) + action_count);
}

static const double *
attached_strategy_sums_const(const InfoNode *node, size_t action_count) {
    return (const double *)(const void *)(attached_regret_sums_const(node) +
                                         action_count);
}

static bool uses_attached_storage(const InfoNode *node) {
    return node->action_count > 0 &&
           node->regret_sums == attached_regret_sums_const(node) &&
           node->strategy_sums ==
               attached_strategy_sums_const(node, node->action_count);
}

Status cfr_info_node_create(InfoSetKey key, size_t action_count,
                            InfoNode **node_out) {
    if (node_out == NULL || action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    size_t allocation_bytes;
    Status status =
        cfr_info_node_owned_size(action_count, &allocation_bytes);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    InfoNode *node = malloc(allocation_bytes);
    if (node == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    status = cfr_info_node_init_owned(
        node, allocation_bytes, key, action_count, node_out);
    if (status == CFR_STATUS_SUCCESS)
        return status;
    free(node);
    return status;
}

Status cfr_info_node_owned_size(size_t action_count, size_t *size_out) {
    if (action_count == 0 || size_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action_count >
        (SIZE_MAX - sizeof(InfoNode)) / (2 * sizeof(double))) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    *size_out = sizeof(InfoNode) + 2 * action_count * sizeof(double);
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_init_owned(void *storage, size_t storage_size,
                                InfoSetKey key, size_t action_count,
                                InfoNode **node_out) {
    size_t required_size;
    Status status = cfr_info_node_owned_size(action_count, &required_size);

    if (status != CFR_STATUS_SUCCESS || storage == NULL || node_out == NULL)
        return status == CFR_STATUS_SUCCESS ? CFR_STATUS_INVALID_ARGUMENT
                                            : status;
    if (storage_size < required_size)
        return CFR_STATUS_BUFFER_TOO_SMALL;
    InfoNode *node = storage;
    *node = (InfoNode){0};
    node->key = key;
    node->action_count = action_count;
    node->regret_sums = attached_regret_sums(node);
    node->strategy_sums = attached_strategy_sums(node, action_count);
    for (size_t index = 0; index < action_count; index += 1) {
        node->regret_sums[index] = 0.0;
        node->strategy_sums[index] = 0.0;
    }
    node->synchronization = 0;
    *node_out = node;
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_init(InfoNode *node, InfoSetKey key, size_t action_count) {
    if (node == NULL || action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action_count > (SIZE_MAX / sizeof(Utility)))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action_count > (SIZE_MAX / sizeof(double)))
        return CFR_STATUS_INVALID_ARGUMENT;

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
    node->synchronization = 0;
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_destroy(InfoNode *node) {
    if (node == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    const bool attached = uses_attached_storage(node);
    if (!attached)
        free(node->regret_sums);
    if (!attached)
        free(node->strategy_sums);
    node->key = 0;
    node->action_count = 0;
    node->regret_sums = NULL;
    node->strategy_sums = NULL;
    node->synchronization = 0;

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

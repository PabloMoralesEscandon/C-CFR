#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/info_node.h"
#include "info_node_internal.h"

/* Tolerances for validating that a distribution sums to one. */
#define REL_EPSILON 1e-8
#define ABS_EPSILON 1e-12

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
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_destroy(InfoNode *node) {
    if (node == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    free(node->regret_sums);
    node->regret_sums = NULL;
    free(node->strategy_sums);
    node->strategy_sums = NULL;
    node->key = 0;
    node->action_count = 0;

    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_current_strategy(const InfoNode *node,
                                      Probability *strategy_array,
                                      size_t strategy_capacity) {
    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || strategy_array == NULL ||
        node->action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (strategy_capacity < node->action_count)
        return CFR_STATUS_BUFFER_TOO_SMALL;
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

Status cfr_info_node_check_deltas(const InfoNode *node,
                                  const Utility *delta_regret,
                                  const double *delta_strategy_sum,
                                  size_t action_count) {
    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || action_count == 0 ||
        delta_regret == NULL || delta_strategy_sum == NULL ||
        action_count != node->action_count)
        return CFR_STATUS_INVALID_ARGUMENT;
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

Status cfr_info_node_apply_deltas(InfoNode *node, const Utility *delta_regret,
                                  const double *delta_strategy_sum,
                                  size_t action_count) {
    Status status = cfr_info_node_check_deltas(
        node, delta_regret, delta_strategy_sum, action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    cfr_info_node_apply_validated_deltas(node, delta_regret,
                                         delta_strategy_sum, action_count);
    return CFR_STATUS_SUCCESS;
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
    if (node == NULL || node->regret_sums == NULL || node->action_count == 0 ||
        node->strategy_sums == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (action_index >= node->action_count || !isfinite(regret_change))
        return CFR_STATUS_INVALID_ARGUMENT;
    Utility new_regret = node->regret_sums[action_index] + regret_change;
    if (!isfinite(new_regret))
        return CFR_STATUS_NUMERIC_ERROR;
    node->regret_sums[action_index] = new_regret;
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_accumulate_strategy(InfoNode *node,
                                         const Probability *strategy_array,
                                         size_t strategy_count,
                                         Probability weight) {
    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || strategy_array == NULL ||
        node->action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (strategy_count != node->action_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!isfinite(weight) || (0.0 > weight) || (1.0 < weight))
        return CFR_STATUS_INVALID_ARGUMENT;

    double sum = 0.0;
    for (size_t i = 0; i < node->action_count; i++) {
        if (!isfinite(node->strategy_sums[i]) || node->strategy_sums[i] < 0.0)
            return CFR_STATUS_NUMERIC_ERROR;
        if (!isfinite(strategy_array[i]) || strategy_array[i] < 0.0 ||
            strategy_array[i] > 1.0)
            return CFR_STATUS_INVALID_ARGUMENT;
        double increment = weight * strategy_array[i];
        double candidate = node->strategy_sums[i] + increment;
        if (!(isfinite(increment) && isfinite(candidate)))
            return CFR_STATUS_NUMERIC_ERROR;
        sum += strategy_array[i];
    }
    if (!isfinite(sum) || !valid_probability(sum))
        return CFR_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < node->action_count; i++) {
        node->strategy_sums[i] += weight * strategy_array[i];
    }
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_node_average_strategy(const InfoNode *node,
                                      Probability *strategy_array,
                                      size_t strategy_capacity) {
    if (node == NULL || node->regret_sums == NULL ||
        node->strategy_sums == NULL || strategy_array == NULL ||
        node->action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (strategy_capacity < node->action_count)
        return CFR_STATUS_BUFFER_TOO_SMALL;
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

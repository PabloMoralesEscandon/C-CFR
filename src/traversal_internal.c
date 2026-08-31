#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "traversal_internal.h"

static constexpr double TRAVERSAL_REL_EPSILON = 1e-8;
static constexpr double TRAVERSAL_ABS_EPSILON = 1e-12;

static bool probability_sum_is_one(double sum) {
    if (fabs(sum - 1.0) <= TRAVERSAL_ABS_EPSILON)
        return true;
    const double scale = sum > 1.0 ? sum : 1.0;
    return fabs(sum - 1.0) <= scale * TRAVERSAL_REL_EPSILON;
}

bool cfr_traversal_operations_supported(const GameOperations *operations) {
    return operations != NULL && operations->is_terminal != NULL &&
           operations->terminal_utility != NULL &&
           operations->current_actor != NULL &&
           operations->legal_actions != NULL &&
           operations->apply_action != NULL &&
           operations->undo_action != NULL &&
           (operations->chance_outcomes != NULL ||
            operations->chance_probability != NULL) &&
           operations->information_set_key != NULL;
}

Status cfr_traversal_collect_chance_outcomes(
    const CfrTraversalAdapter *adapter, const GameState *state,
    Action *actions, Probability *probabilities, size_t capacity,
    size_t *count_out) {
    size_t count;
    Status status;

    if (adapter == NULL || adapter->operations == NULL || state == NULL ||
        actions == NULL || probabilities == NULL || count_out == NULL ||
        capacity == 0 || adapter->max_legal_actions == 0 ||
        adapter->max_legal_actions > capacity) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    const bool batched = adapter->operations->chance_outcomes != NULL;
    if (batched) {
        status = adapter->operations->chance_outcomes(
            adapter->context, state, actions, probabilities, capacity, &count);
    } else {
        status = adapter->operations->legal_actions(
            adapter->context, state, actions, capacity, &count);
    }
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (count == 0 || count > adapter->max_legal_actions)
        return CFR_STATUS_INVALID_ARGUMENT;

    double probability_sum = 0.0;
    for (size_t action = 0; action < count; action += 1) {
        Probability probability;

        if (batched) {
            probability = probabilities[action];
        } else {
            status = adapter->operations->chance_probability(
                adapter->context, state, actions[action], &probability);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            probabilities[action] = probability;
        }
        if (!isfinite(probability) || probability < 0.0)
            return CFR_STATUS_INVALID_ARGUMENT;
        probability_sum += probability;
    }
    if (!isfinite(probability_sum) ||
        !probability_sum_is_one(probability_sum)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    *count_out = count;
    return CFR_STATUS_SUCCESS;
}

size_t cfr_traversal_hash_node(const InfoNode *node) {
    const uintptr_t value = (uintptr_t)node >> 4;
    return (size_t)(value * UINT64_C(11400714819323198485));
}

void cfr_traversal_initialize_index_table(size_t *table, size_t capacity,
                                          size_t empty_value) {
    for (size_t index = 0; index < capacity; index += 1)
        table[index] = empty_value;
}

Status cfr_traversal_grow_array(void *array, size_t element_size,
                                size_t *capacity, void **grown_out) {
    if (array == NULL || element_size == 0 || capacity == NULL ||
        *capacity == 0 || grown_out == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (*capacity > SIZE_MAX / 2)
        return CFR_STATUS_OUT_OF_MEMORY;
    const size_t grown_capacity = *capacity * 2;
    if (grown_capacity > SIZE_MAX / element_size)
        return CFR_STATUS_OUT_OF_MEMORY;

    void *grown = realloc(array, grown_capacity * element_size);
    if (grown == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    *capacity = grown_capacity;
    *grown_out = grown;
    return CFR_STATUS_SUCCESS;
}

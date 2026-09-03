#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/mccfr.h"
#include "info_node_internal.h"
#include "info_store_internal.h"
#include "mccfr_sequential_internal.h"
#include "traversal_internal.h"

#define MCCFR_CELL_EMPTY SIZE_MAX
#define MCCFR_INITIAL_FRAME_CAPACITY ((size_t)32)
#define MCCFR_INITIAL_TABLE_CAPACITY ((size_t)64)
#define MCCFR_INITIAL_ENTRY_CAPACITY ((size_t)16)

static Status traverse_branch(const CfrTraversalAdapter *adapter,
                              GameState *state, InfoStore *store,
                              Player target_player, size_t depth,
                              Probability own_reach,
                              MccfrSequentialWorkspace *workspace,
                              Utility *utility_out);

/* SplitMix64 supplies a small, completely specified random stream. */
static uint64_t rng_next(MccfrRng *rng) {
    uint64_t value;

    rng->state += UINT64_C(0x9e3779b97f4a7c15);
    value = rng->state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

void cfr_mccfr_sequential_workspace_destroy(
    MccfrSequentialWorkspace *workspace) {
    if (workspace == NULL)
        return;
    free(workspace->frames);
    free(workspace->delta_table);
    free(workspace->delta_entries);
    free(workspace->sample_table);
    free(workspace->sample_entries);
    free(workspace->arena);
    *workspace = (MccfrSequentialWorkspace){0};
}

Status cfr_mccfr_sequential_workspace_init(MccfrSequentialWorkspace *workspace,
                                           size_t max_legal_actions) {
    MccfrSequentialWorkspace temporary = {0};

    if (workspace == NULL || max_legal_actions == 0 ||
        max_legal_actions > CFR_TRAVERSAL_MAX_ACTIONS ||
        max_legal_actions > SIZE_MAX / (2 * MCCFR_INITIAL_ENTRY_CAPACITY)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    temporary.frames =
        malloc(MCCFR_INITIAL_FRAME_CAPACITY * sizeof(*temporary.frames));
    temporary.delta_table =
        malloc(MCCFR_INITIAL_TABLE_CAPACITY * sizeof(*temporary.delta_table));
    temporary.delta_entries =
        malloc(MCCFR_INITIAL_ENTRY_CAPACITY * sizeof(*temporary.delta_entries));
    temporary.sample_table =
        malloc(MCCFR_INITIAL_TABLE_CAPACITY * sizeof(*temporary.sample_table));
    temporary.sample_entries = malloc(MCCFR_INITIAL_ENTRY_CAPACITY *
                                      sizeof(*temporary.sample_entries));
    temporary.arena_capacity =
        2 * MCCFR_INITIAL_ENTRY_CAPACITY * max_legal_actions;
    temporary.arena =
        malloc(temporary.arena_capacity * sizeof(*temporary.arena));

    if (temporary.frames == NULL || temporary.delta_table == NULL ||
        temporary.delta_entries == NULL || temporary.sample_table == NULL ||
        temporary.sample_entries == NULL || temporary.arena == NULL) {
        cfr_mccfr_sequential_workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.frame_capacity = MCCFR_INITIAL_FRAME_CAPACITY;
    temporary.delta_table_capacity = MCCFR_INITIAL_TABLE_CAPACITY;
    temporary.delta_entry_capacity = MCCFR_INITIAL_ENTRY_CAPACITY;
    temporary.sample_table_capacity = MCCFR_INITIAL_TABLE_CAPACITY;
    temporary.sample_entry_capacity = MCCFR_INITIAL_ENTRY_CAPACITY;
    cfr_traversal_initialize_index_table(temporary.delta_table,
                                         temporary.delta_table_capacity,
                                         MCCFR_CELL_EMPTY);
    cfr_traversal_initialize_index_table(temporary.sample_table,
                                         temporary.sample_table_capacity,
                                         MCCFR_CELL_EMPTY);
    *workspace = temporary;
    return CFR_STATUS_SUCCESS;
}

static void workspace_reset(MccfrSequentialWorkspace *workspace,
                            const MccfrRng *rng) {
    for (size_t index = 0; index < workspace->delta_entry_count; index += 1) {
        workspace->delta_table[workspace->delta_entries[index].table_cell] =
            MCCFR_CELL_EMPTY;
    }
    for (size_t index = 0; index < workspace->sample_entry_count; index += 1) {
        workspace->sample_table[workspace->sample_entries[index].table_cell] =
            MCCFR_CELL_EMPTY;
    }
    workspace->delta_table_used = 0;
    workspace->delta_entry_count = 0;
    workspace->sample_table_used = 0;
    workspace->sample_entry_count = 0;
    workspace->arena_used = 0;
    workspace->visits = 0;
    workspace->rng = *rng;
}

static Status ensure_frame(MccfrSequentialWorkspace *workspace, size_t depth) {
    if (depth < workspace->frame_capacity)
        return CFR_STATUS_SUCCESS;
    void *grown;
    const Status status =
        cfr_traversal_grow_array(workspace->frames, sizeof(*workspace->frames),
                                 &workspace->frame_capacity, &grown);

    if (status == CFR_STATUS_SUCCESS)
        workspace->frames = grown;
    return status;
}

static Status allocate_grown_table(size_t capacity, size_t **table_out,
                                   size_t *capacity_out) {
    size_t *grown;

    if (capacity > SIZE_MAX / 2)
        return CFR_STATUS_OUT_OF_MEMORY;
    const size_t grown_capacity = capacity * 2;
    if (grown_capacity > SIZE_MAX / sizeof(*grown))
        return CFR_STATUS_OUT_OF_MEMORY;
    grown = malloc(grown_capacity * sizeof(*grown));
    if (grown == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    cfr_traversal_initialize_index_table(grown, grown_capacity,
                                         MCCFR_CELL_EMPTY);
    *table_out = grown;
    *capacity_out = grown_capacity;
    return CFR_STATUS_SUCCESS;
}

static Status grow_delta_table(MccfrSequentialWorkspace *workspace) {
    size_t *grown;
    size_t capacity;
    Status status = allocate_grown_table(workspace->delta_table_capacity,
                                         &grown, &capacity);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    const size_t mask = capacity - 1;
    for (size_t index = 0; index < workspace->delta_entry_count; index += 1) {
        size_t cell =
            cfr_traversal_hash_node(workspace->delta_entries[index].node) &
            mask;

        while (grown[cell] != MCCFR_CELL_EMPTY)
            cell = (cell + 1) & mask;
        grown[cell] = index;
        workspace->delta_entries[index].table_cell = cell;
    }
    free(workspace->delta_table);
    workspace->delta_table = grown;
    workspace->delta_table_capacity = capacity;
    return CFR_STATUS_SUCCESS;
}

static Status grow_sample_table(MccfrSequentialWorkspace *workspace) {
    size_t *grown;
    size_t capacity;
    Status status = allocate_grown_table(workspace->sample_table_capacity,
                                         &grown, &capacity);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    const size_t mask = capacity - 1;
    for (size_t index = 0; index < workspace->sample_entry_count; index += 1) {
        size_t cell =
            cfr_traversal_hash_node(workspace->sample_entries[index].node) &
            mask;

        while (grown[cell] != MCCFR_CELL_EMPTY)
            cell = (cell + 1) & mask;
        grown[cell] = index;
        workspace->sample_entries[index].table_cell = cell;
    }
    free(workspace->sample_table);
    workspace->sample_table = grown;
    workspace->sample_table_capacity = capacity;
    return CFR_STATUS_SUCCESS;
}

static Status ensure_delta_entries(MccfrSequentialWorkspace *workspace) {
    if (workspace->delta_entry_count < workspace->delta_entry_capacity)
        return CFR_STATUS_SUCCESS;
    void *grown;
    const Status status = cfr_traversal_grow_array(
        workspace->delta_entries, sizeof(*workspace->delta_entries),
        &workspace->delta_entry_capacity, &grown);

    if (status == CFR_STATUS_SUCCESS)
        workspace->delta_entries = grown;
    return status;
}

static Status ensure_sample_entries(MccfrSequentialWorkspace *workspace) {
    if (workspace->sample_entry_count < workspace->sample_entry_capacity)
        return CFR_STATUS_SUCCESS;
    void *grown;
    const Status status = cfr_traversal_grow_array(
        workspace->sample_entries, sizeof(*workspace->sample_entries),
        &workspace->sample_entry_capacity, &grown);

    if (status == CFR_STATUS_SUCCESS)
        workspace->sample_entries = grown;
    return status;
}

static Status ensure_arena(MccfrSequentialWorkspace *workspace,
                           size_t action_count) {
    double *grown;
    size_t needed;
    size_t capacity;

    if (action_count > (SIZE_MAX - workspace->arena_used) / 2)
        return CFR_STATUS_OUT_OF_MEMORY;
    needed = workspace->arena_used + 2 * action_count;
    if (needed <= workspace->arena_capacity)
        return CFR_STATUS_SUCCESS;
    capacity = workspace->arena_capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return CFR_STATUS_OUT_OF_MEMORY;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*grown))
        return CFR_STATUS_OUT_OF_MEMORY;
    grown = realloc(workspace->arena, capacity * sizeof(*grown));
    if (grown == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    workspace->arena = grown;
    workspace->arena_capacity = capacity;
    return CFR_STATUS_SUCCESS;
}

static Status find_or_create_delta(MccfrSequentialWorkspace *workspace,
                                   InfoNode *node, size_t action_count,
                                   size_t *index_out) {
    size_t mask;
    size_t cell;
    Status status;

    mask = workspace->delta_table_capacity - 1;
    cell = cfr_traversal_hash_node(node) & mask;
    while (workspace->delta_table[cell] != MCCFR_CELL_EMPTY) {
        const size_t candidate = workspace->delta_table[cell];

        if (workspace->delta_entries[candidate].node == node) {
            *index_out = candidate;
            return CFR_STATUS_SUCCESS;
        }
        cell = (cell + 1) & mask;
    }

    if (workspace->delta_table_used + 1 >
        workspace->delta_table_capacity - workspace->delta_table_capacity / 4) {
        status = grow_delta_table(workspace);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        mask = workspace->delta_table_capacity - 1;
        cell = cfr_traversal_hash_node(node) & mask;
        while (workspace->delta_table[cell] != MCCFR_CELL_EMPTY)
            cell = (cell + 1) & mask;
    }

    status = ensure_delta_entries(workspace);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    status = ensure_arena(workspace, action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    const size_t index = workspace->delta_entry_count;
    workspace->delta_entries[index] =
        (MccfrSequentialDeltaEntry){.node = node,
                                    .action_count = action_count,
                                    .arena_offset = workspace->arena_used,
                                    .table_cell = cell};
    for (size_t offset = 0; offset < 2 * action_count; offset += 1)
        workspace->arena[workspace->arena_used + offset] = 0.0;
    workspace->arena_used += 2 * action_count;
    workspace->delta_entry_count += 1;
    workspace->delta_table[cell] = index;
    workspace->delta_table_used += 1;
    *index_out = index;
    return CFR_STATUS_SUCCESS;
}

static Status sample_index(const Probability *probabilities, size_t count,
                           MccfrRng *rng, size_t *index_out) {
    long double sum = 0.0L;
    size_t last_positive = SIZE_MAX;
    size_t positive_count = 0;

    if (probabilities == NULL || count == 0 || rng == NULL ||
        index_out == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < count; index += 1) {
        if (!isfinite(probabilities[index]) || probabilities[index] < 0.0)
            return CFR_STATUS_INVALID_ARGUMENT;
        sum += (long double)probabilities[index];
        if (probabilities[index] > 0.0) {
            last_positive = index;
            positive_count += 1;
        }
    }
    if (!isfinite(sum) || sum <= 0.0 || last_positive == SIZE_MAX)
        return CFR_STATUS_INVALID_ARGUMENT;

    /*
     * Partition all 2^64 random values into monotonic integer intervals. Each
     * positive action receives at least one value, including probabilities
     * below the fixed-point resolution. Reserving values for later actions
     * keeps every positive action reachable.
     */
    const uint64_t draw = rng_next(rng);
    long double cumulative = 0.0L;
    uint64_t previous_endpoint = 0;
    size_t positives_seen = 0;

    for (size_t index = 0; index < count; index += 1) {
        if (!(probabilities[index] > 0.0))
            continue;
        if (index == last_positive)
            break;

        cumulative += (long double)probabilities[index];
        positives_seen += 1;
        const size_t positive_remaining = positive_count - positives_seen;
        const uint64_t minimum_endpoint = previous_endpoint + 1;
        const uint64_t maximum_endpoint =
            UINT64_MAX - (uint64_t)(positive_remaining - 1);
        const long double scaled_endpoint = cumulative / sum * 0x1.0p64L;
        uint64_t endpoint;

        if (!(scaled_endpoint > 0.0L)) {
            endpoint = 0;
        } else if (scaled_endpoint >= (long double)UINT64_MAX) {
            endpoint = UINT64_MAX;
        } else {
            endpoint = (uint64_t)scaled_endpoint;
        }
        if (endpoint < minimum_endpoint)
            endpoint = minimum_endpoint;
        if (endpoint > maximum_endpoint)
            endpoint = maximum_endpoint;

        if (draw < endpoint) {
            *index_out = index;
            return CFR_STATUS_SUCCESS;
        }
        previous_endpoint = endpoint;
    }
    *index_out = last_positive;
    return CFR_STATUS_SUCCESS;
}

static Status get_sampled_action(MccfrSequentialWorkspace *workspace,
                                 const InfoNode *node, const Action *actions,
                                 const Probability *strategy,
                                 size_t action_count, size_t *index_out) {
    size_t mask;
    size_t cell;
    Status status;

    mask = workspace->sample_table_capacity - 1;
    cell = cfr_traversal_hash_node(node) & mask;
    while (workspace->sample_table[cell] != MCCFR_CELL_EMPTY) {
        const size_t candidate = workspace->sample_table[cell];

        if (workspace->sample_entries[candidate].node == node) {
            if (workspace->sample_entries[candidate].action_count !=
                action_count) {
                return CFR_STATUS_INVALID_ARGUMENT;
            }
            for (size_t action = 0; action < action_count; action += 1) {
                if (workspace->sample_entries[candidate].actions[action] !=
                    actions[action]) {
                    return CFR_STATUS_INVALID_ARGUMENT;
                }
            }
            *index_out = workspace->sample_entries[candidate].action_index;
            return *index_out < action_count ? CFR_STATUS_SUCCESS
                                             : CFR_STATUS_INVALID_ARGUMENT;
        }
        cell = (cell + 1) & mask;
    }

    if (workspace->sample_table_used + 1 >
        workspace->sample_table_capacity -
            workspace->sample_table_capacity / 4) {
        status = grow_sample_table(workspace);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        mask = workspace->sample_table_capacity - 1;
        cell = cfr_traversal_hash_node(node) & mask;
        while (workspace->sample_table[cell] != MCCFR_CELL_EMPTY)
            cell = (cell + 1) & mask;
    }

    status = ensure_sample_entries(workspace);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    size_t sampled;
    status = sample_index(strategy, action_count, &workspace->rng, &sampled);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    const size_t entry = workspace->sample_entry_count;
    workspace->sample_entries[entry] =
        (MccfrSequentialSampleEntry){.node = node,
                                     .action_index = sampled,
                                     .action_count = action_count,
                                     .table_cell = cell};
    for (size_t action = 0; action < action_count; action += 1)
        workspace->sample_entries[entry].actions[action] = actions[action];
    workspace->sample_entry_count += 1;
    workspace->sample_table[cell] = entry;
    workspace->sample_table_used += 1;
    *index_out = sampled;
    return CFR_STATUS_SUCCESS;
}

static Status
workspace_check_deltas(const MccfrSequentialWorkspace *workspace) {
    for (size_t index = 0; index < workspace->delta_entry_count; index += 1) {
        const MccfrSequentialDeltaEntry *entry =
            &workspace->delta_entries[index];
        const Utility *regret = workspace->arena + entry->arena_offset;
        const double *strategy = regret + entry->action_count;
        const Status status = cfr_info_node_check_deltas_sequential(
            entry->node, regret, strategy, entry->action_count);

        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    return CFR_STATUS_SUCCESS;
}

static Status workspace_apply_deltas(MccfrSequentialWorkspace *workspace) {
    for (size_t index = 0; index < workspace->delta_entry_count; index += 1) {
        MccfrSequentialDeltaEntry *entry = &workspace->delta_entries[index];
        Utility *regret = workspace->arena + entry->arena_offset;
        double *strategy = regret + entry->action_count;
        cfr_info_node_apply_validated_deltas(entry->node, regret, strategy,
                                             entry->action_count);
    }
    return CFR_STATUS_SUCCESS;
}

/*
 * Records sampled counterfactual regrets for a target-player information set.
 *
 * External sampling draws the chance and opponent actions from their own
 * probabilities, so the sampling distribution already supplies the external
 * reach that weights a counterfactual regret. The per-visit difference is
 * therefore recorded without any importance weight.
 *
 * accumulate_strategy adds the own-reach-weighted strategy for a game with a
 * single strategic player, where no opponent node can carry the average. Only
 * chance then separates the traversal from the information set, and chance
 * does not depend on the strategy, so the expected delta stays proportional to
 * the full CFR strategy sums. A second strategic player makes that factor
 * strategy-dependent, so the caller leaves the average to that player's own
 * traversal instead.
 */
static Status record_target_deltas(MccfrSequentialWorkspace *workspace,
                                   InfoNode *node,
                                   const MccfrSequentialFrame *frame,
                                   size_t action_count, Utility node_utility,
                                   bool accumulate_strategy,
                                   Probability own_reach) {
    size_t entry_index;
    Status status =
        find_or_create_delta(workspace, node, action_count, &entry_index);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (accumulate_strategy && (!isfinite(own_reach) || own_reach < 0.0))
        return CFR_STATUS_NUMERIC_ERROR;

    MccfrSequentialDeltaEntry *entry = &workspace->delta_entries[entry_index];
    Utility *delta_regret = workspace->arena + entry->arena_offset;
    double *delta_strategy = delta_regret + action_count;

    for (size_t action = 0; action < action_count; action += 1) {
        const Utility regret_change = frame->utilities[action] - node_utility;

        if (!isfinite(regret_change))
            return CFR_STATUS_NUMERIC_ERROR;
        delta_regret[action] += regret_change;
        if (!isfinite(delta_regret[action]))
            return CFR_STATUS_NUMERIC_ERROR;
        if (!accumulate_strategy)
            continue;
        delta_strategy[action] += own_reach * frame->probabilities[action];
        if (!isfinite(delta_strategy[action]))
            return CFR_STATUS_NUMERIC_ERROR;
    }
    return CFR_STATUS_SUCCESS;
}

/*
 * Records an average-strategy contribution for a sampled player's information
 * set.
 *
 * The traversal reaches a history of this information set with exactly the
 * probability that the sampled player and chance reach it, so accumulating the
 * unweighted strategy reproduces the full CFR strategy sums up to one constant
 * per information set. cfr_info_node_average_strategy normalizes that constant
 * away.
 */
static Status record_sampled_strategy(MccfrSequentialWorkspace *workspace,
                                      InfoNode *node,
                                      const MccfrSequentialFrame *frame,
                                      size_t action_count) {
    size_t entry_index;
    Status status =
        find_or_create_delta(workspace, node, action_count, &entry_index);

    if (status != CFR_STATUS_SUCCESS)
        return status;

    MccfrSequentialDeltaEntry *entry = &workspace->delta_entries[entry_index];
    double *delta_strategy =
        workspace->arena + entry->arena_offset + action_count;

    for (size_t action = 0; action < action_count; action += 1) {
        const Probability probability = frame->probabilities[action];

        if (!isfinite(probability) || probability < 0.0)
            return CFR_STATUS_NUMERIC_ERROR;
        delta_strategy[action] += probability;
        if (!isfinite(delta_strategy[action]))
            return CFR_STATUS_NUMERIC_ERROR;
    }
    return CFR_STATUS_SUCCESS;
}

static Status traverse_target_node(const CfrTraversalAdapter *adapter,
                                   GameState *state, InfoStore *store,
                                   Player target_player, size_t depth,
                                   Probability own_reach,
                                   MccfrSequentialWorkspace *workspace,
                                   InfoNode *node, size_t action_count,
                                   Utility *utility_out) {
    Utility node_utility = 0.0;

    for (size_t action = 0; action < action_count; action += 1) {
        MccfrSequentialFrame *frame = &workspace->frames[depth];
        const Probability child_own_reach =
            own_reach * frame->probabilities[action];
        Status status = adapter->operations->apply_action(
            adapter->context, state, frame->actions[action]);

        if (status != CFR_STATUS_SUCCESS)
            return status;

        Utility branch_utility;
        const Status branch_status =
            traverse_branch(adapter, state, store, target_player, depth + 1,
                            child_own_reach, workspace, &branch_utility);
        status = adapter->operations->undo_action(adapter->context, state);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        frame = &workspace->frames[depth];
        if (branch_status != CFR_STATUS_SUCCESS)
            return branch_status;

        frame->utilities[action] = branch_utility;
        const Utility candidate =
            node_utility + frame->probabilities[action] * branch_utility;
        if (!isfinite(candidate))
            return CFR_STATUS_NUMERIC_ERROR;
        node_utility = candidate;
    }

    const Status status = record_target_deltas(
        workspace, node, &workspace->frames[depth], action_count, node_utility,
        adapter->strategic_player_count == 1, own_reach);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *utility_out = node_utility;
    return CFR_STATUS_SUCCESS;
}

static Status traverse_opponent_node(const CfrTraversalAdapter *adapter,
                                     GameState *state, InfoStore *store,
                                     Player target_player, size_t depth,
                                     Probability own_reach,
                                     MccfrSequentialWorkspace *workspace,
                                     InfoNode *node, size_t action_count,
                                     Utility *utility_out) {
    MccfrSequentialFrame *frame = &workspace->frames[depth];
    size_t sampled_action;
    Status status =
        get_sampled_action(workspace, node, frame->actions,
                           frame->probabilities, action_count, &sampled_action);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (!(frame->probabilities[sampled_action] > 0.0))
        return CFR_STATUS_NUMERIC_ERROR;
    status = record_sampled_strategy(workspace, node, frame, action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    frame = &workspace->frames[depth];
    status = adapter->operations->apply_action(adapter->context, state,
                                               frame->actions[sampled_action]);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    Utility branch_utility;
    const Status branch_status =
        traverse_branch(adapter, state, store, target_player, depth + 1,
                        own_reach, workspace, &branch_utility);
    status = adapter->operations->undo_action(adapter->context, state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (branch_status != CFR_STATUS_SUCCESS)
        return branch_status;
    *utility_out = branch_utility;
    return CFR_STATUS_SUCCESS;
}

static Status traverse_chance_node(const CfrTraversalAdapter *adapter,
                                   GameState *state, InfoStore *store,
                                   Player target_player, size_t depth,
                                   Probability own_reach,
                                   MccfrSequentialWorkspace *workspace,
                                   Utility *utility_out) {
    Status status = ensure_frame(workspace, depth);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    MccfrSequentialFrame *frame = &workspace->frames[depth];
    size_t action_count;
    status = cfr_traversal_collect_chance_outcomes(
        adapter, state, frame->actions, frame->probabilities,
        CFR_TRAVERSAL_MAX_ACTIONS, &action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    size_t sampled_action;
    status = sample_index(frame->probabilities, action_count, &workspace->rng,
                          &sampled_action);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (!(frame->probabilities[sampled_action] > 0.0))
        return CFR_STATUS_NUMERIC_ERROR;

    status = adapter->operations->apply_action(adapter->context, state,
                                               frame->actions[sampled_action]);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    Utility branch_utility;
    const Status branch_status =
        traverse_branch(adapter, state, store, target_player, depth + 1,
                        own_reach, workspace, &branch_utility);
    status = adapter->operations->undo_action(adapter->context, state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (branch_status != CFR_STATUS_SUCCESS)
        return branch_status;
    *utility_out = branch_utility;
    return CFR_STATUS_SUCCESS;
}

static Status traverse_branch(const CfrTraversalAdapter *adapter,
                              GameState *state, InfoStore *store,
                              Player target_player, size_t depth,
                              Probability own_reach,
                              MccfrSequentialWorkspace *workspace,
                              Utility *utility_out) {
    if (workspace->visits != SIZE_MAX)
        workspace->visits += 1;

    bool terminal;
    Status status =
        adapter->operations->is_terminal(adapter->context, state, &terminal);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (terminal) {
        Utility utility;
        status = adapter->operations->terminal_utility(adapter->context, state,
                                                       target_player, &utility);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (!isfinite(utility))
            return CFR_STATUS_NUMERIC_ERROR;
        *utility_out = utility;
        return CFR_STATUS_SUCCESS;
    }

    Actor actor;
    status =
        adapter->operations->current_actor(adapter->context, state, &actor);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (actor.kind == CFR_ACTOR_CHANCE) {
        return traverse_chance_node(adapter, state, store, target_player, depth,
                                    own_reach, workspace, utility_out);
    }
    if (actor.kind != CFR_ACTOR_PLAYER ||
        (actor.player != CFR_PLAYER_0 && actor.player != CFR_PLAYER_1)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    status = ensure_frame(workspace, depth);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    MccfrSequentialFrame *frame = &workspace->frames[depth];
    size_t action_count;
    status = adapter->operations->legal_actions(
        adapter->context, state, frame->actions, CFR_TRAVERSAL_MAX_ACTIONS,
        &action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (action_count == 0 || action_count > adapter->max_legal_actions)
        return CFR_STATUS_INVALID_ARGUMENT;

    InfoSetKey key;
    status =
        adapter->operations->information_set_key(adapter->context, state, &key);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    InfoNode *node;
    status = cfr_info_store_get_or_create_sequential(store, key, action_count,
                                                     &node);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    status = cfr_info_node_current_strategy_sequential(
        node, frame->probabilities, action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    if (actor.player == target_player) {
        return traverse_target_node(adapter, state, store, target_player, depth,
                                    own_reach, workspace, node, action_count,
                                    utility_out);
    }
    return traverse_opponent_node(adapter, state, store, target_player, depth,
                                  own_reach, workspace, node, action_count,
                                  utility_out);
}

Status
cfr_mccfr_sequential_external_traverse(const Game *game, GameState *state,
                                       InfoStore *store, Player target_player,
                                       MccfrRng *rng, Utility *utility_out) {
    TraversalStats discarded = {0};
    return cfr_mccfr_sequential_external_traverse_with_stats(
        game, state, store, target_player, rng, utility_out, &discarded);
}

static Status configure_adapter(const Game *game, GameState *state,
                                InfoStore *store, Player target_player,
                                MccfrRng *rng, Utility *utility_out,
                                TraversalStats *stats_out,
                                CfrTraversalAdapter *adapter_out) {
    if (game == NULL || state == NULL || store == NULL || rng == NULL ||
        utility_out == NULL || stats_out == NULL || adapter_out == NULL ||
        game->max_legal_actions == 0 ||
        game->max_legal_actions > CFR_TRAVERSAL_MAX_ACTIONS ||
        game->strategic_player_count == 0 || game->strategic_player_count > 2 ||
        (target_player != CFR_PLAYER_0 && target_player != CFR_PLAYER_1)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    Status status = cfr_game_validate_state(game, state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    const GameOperations *operations = game->operations;
    if (game->trusted_operations != NULL)
        operations = game->trusted_operations;
    if (!cfr_traversal_operations_supported(operations))
        return CFR_STATUS_INVALID_ARGUMENT;

    *adapter_out = (CfrTraversalAdapter){
        .operations = operations,
        .context = game->context,
        .max_legal_actions = game->max_legal_actions,
        .strategic_player_count = game->strategic_player_count};
    return CFR_STATUS_SUCCESS;
}

static Status traverse_in_workspace(const CfrTraversalAdapter *adapter,
                                    GameState *state, InfoStore *store,
                                    Player target_player, MccfrRng *rng,
                                    Utility *utility_out,
                                    TraversalStats *stats_out,
                                    MccfrSequentialWorkspace *workspace) {
    workspace_reset(workspace, rng);
    Utility temporary_utility;
    Status status = traverse_branch(adapter, state, store, target_player, 0,
                                    1.0, workspace, &temporary_utility);
    if (status == CFR_STATUS_SUCCESS)
        status = workspace_check_deltas(workspace);
    if (status == CFR_STATUS_SUCCESS)
        status = workspace_apply_deltas(workspace);
    if (status == CFR_STATUS_SUCCESS) {
        *rng = workspace->rng;
        *utility_out = temporary_utility;
        stats_out->visited_nodes = workspace->visits;
    }
    return status;
}

Status cfr_mccfr_sequential_external_traverse_in_workspace(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    MccfrRng *rng, Utility *utility_out, TraversalStats *stats_out,
    MccfrSequentialWorkspace *workspace) {
    CfrTraversalAdapter adapter;
    Status status = configure_adapter(game, state, store, target_player, rng,
                                      utility_out, stats_out, &adapter);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (workspace == NULL || workspace->frames == NULL ||
        workspace->delta_table == NULL || workspace->delta_entries == NULL ||
        workspace->sample_table == NULL || workspace->sample_entries == NULL ||
        workspace->arena == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    return traverse_in_workspace(&adapter, state, store, target_player, rng,
                                 utility_out, stats_out, workspace);
}

Status cfr_mccfr_sequential_external_traverse_with_stats(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    MccfrRng *rng, Utility *utility_out, TraversalStats *stats_out) {
    CfrTraversalAdapter adapter;
    Status status = configure_adapter(game, state, store, target_player, rng,
                                      utility_out, stats_out, &adapter);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    MccfrSequentialWorkspace workspace = {0};
    status = cfr_mccfr_sequential_workspace_init(&workspace,
                                                 adapter.max_legal_actions);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    status = traverse_in_workspace(&adapter, state, store, target_player, rng,
                                   utility_out, stats_out, &workspace);
    cfr_mccfr_sequential_workspace_destroy(&workspace);
    return status;
}

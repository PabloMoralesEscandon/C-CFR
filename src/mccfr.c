#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/mccfr.h"

#define MCCFR_CELL_EMPTY SIZE_MAX
#define MCCFR_INITIAL_FRAME_CAPACITY 32
#define MCCFR_INITIAL_TABLE_CAPACITY 64
#define MCCFR_INITIAL_ENTRY_CAPACITY 16
#define MCCFR_REL_EPSILON 1e-8
#define MCCFR_ABS_EPSILON 1e-12

typedef struct {
    Action actions[CFR_TRAVERSAL_MAX_ACTIONS];
    Probability probabilities[CFR_TRAVERSAL_MAX_ACTIONS];
    Utility utilities[CFR_TRAVERSAL_MAX_ACTIONS];
} MccfrFrame;

typedef struct {
    InfoNode *node;
    size_t action_count;
    size_t arena_offset;
} DeltaEntry;

typedef struct {
    const InfoNode *node;
    size_t action_index;
    size_t action_count;
    Action actions[CFR_TRAVERSAL_MAX_ACTIONS];
} SampleEntry;

typedef struct {
    const GameOperations *operations;
    const void *context;
    size_t max_legal_actions;
} MccfrAdapter;

typedef struct {
    MccfrFrame *frames;
    size_t frame_capacity;

    size_t *delta_table;
    size_t delta_table_capacity;
    size_t delta_table_used;
    DeltaEntry *delta_entries;
    size_t delta_entry_count;
    size_t delta_entry_capacity;

    size_t *sample_table;
    size_t sample_table_capacity;
    size_t sample_table_used;
    SampleEntry *sample_entries;
    size_t sample_entry_count;
    size_t sample_entry_capacity;

    double *arena;
    size_t arena_used;
    size_t arena_capacity;

    size_t visits;
    MccfrRng rng;
} MccfrWorkspace;

static Status traverse_branch(const MccfrAdapter *adapter, GameState *state,
                              InfoStore *store, Player target_player,
                              size_t depth, long double own_reach,
                              long double external_sample_reach,
                              MccfrWorkspace *workspace,
                              Utility *utility_out);

static bool operations_support_mccfr(const GameOperations *operations) {
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

static bool sum_is_one(double sum) {
    if (fabs(sum - 1.0) <= MCCFR_ABS_EPSILON)
        return true;
    const double scale = sum > 1.0 ? sum : 1.0;
    return fabs(sum - 1.0) <= scale * MCCFR_REL_EPSILON;
}

Status cfr_mccfr_rng_seed(MccfrRng *rng, uint64_t seed) {
    if (rng == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    rng->state = seed;
    return CFR_STATUS_SUCCESS;
}

/* SplitMix64 supplies a small, completely specified random stream. */
static uint64_t rng_next(MccfrRng *rng) {
    uint64_t value;

    rng->state += UINT64_C(0x9e3779b97f4a7c15);
    value = rng->state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static double rng_unit(MccfrRng *rng) {
    /* The upper 53 bits map exactly into [0, 1) in binary64. */
    return (double)(rng_next(rng) >> 11) * 0x1.0p-53;
}

static size_t hash_node(const InfoNode *node) {
    const uintptr_t value = (uintptr_t)node >> 4;
    return (size_t)(value * UINT64_C(11400714819323198485));
}

static void initialize_table(size_t *table, size_t capacity) {
    for (size_t index = 0; index < capacity; index += 1)
        table[index] = MCCFR_CELL_EMPTY;
}

static void workspace_destroy(MccfrWorkspace *workspace) {
    if (workspace == NULL)
        return;
    free(workspace->frames);
    free(workspace->delta_table);
    free(workspace->delta_entries);
    free(workspace->sample_table);
    free(workspace->sample_entries);
    free(workspace->arena);
    *workspace = (MccfrWorkspace){0};
}

static Status workspace_init(MccfrWorkspace *workspace,
                             size_t max_legal_actions,
                             const MccfrRng *rng) {
    MccfrWorkspace temporary = {0};

    if (workspace == NULL || rng == NULL || max_legal_actions == 0 ||
        max_legal_actions > CFR_TRAVERSAL_MAX_ACTIONS ||
        max_legal_actions > SIZE_MAX / (2 * MCCFR_INITIAL_ENTRY_CAPACITY)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    temporary.frames =
        malloc(MCCFR_INITIAL_FRAME_CAPACITY * sizeof(*temporary.frames));
    temporary.delta_table =
        malloc(MCCFR_INITIAL_TABLE_CAPACITY * sizeof(*temporary.delta_table));
    temporary.delta_entries = malloc(MCCFR_INITIAL_ENTRY_CAPACITY *
                                     sizeof(*temporary.delta_entries));
    temporary.sample_table = malloc(MCCFR_INITIAL_TABLE_CAPACITY *
                                    sizeof(*temporary.sample_table));
    temporary.sample_entries = malloc(MCCFR_INITIAL_ENTRY_CAPACITY *
                                      sizeof(*temporary.sample_entries));
    temporary.arena_capacity =
        2 * MCCFR_INITIAL_ENTRY_CAPACITY * max_legal_actions;
    temporary.arena =
        malloc(temporary.arena_capacity * sizeof(*temporary.arena));

    if (temporary.frames == NULL || temporary.delta_table == NULL ||
        temporary.delta_entries == NULL || temporary.sample_table == NULL ||
        temporary.sample_entries == NULL || temporary.arena == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.frame_capacity = MCCFR_INITIAL_FRAME_CAPACITY;
    temporary.delta_table_capacity = MCCFR_INITIAL_TABLE_CAPACITY;
    temporary.delta_entry_capacity = MCCFR_INITIAL_ENTRY_CAPACITY;
    temporary.sample_table_capacity = MCCFR_INITIAL_TABLE_CAPACITY;
    temporary.sample_entry_capacity = MCCFR_INITIAL_ENTRY_CAPACITY;
    initialize_table(temporary.delta_table, temporary.delta_table_capacity);
    initialize_table(temporary.sample_table, temporary.sample_table_capacity);
    temporary.rng = *rng;
    *workspace = temporary;
    return CFR_STATUS_SUCCESS;
}

static Status ensure_frame(MccfrWorkspace *workspace, size_t depth) {
    MccfrFrame *grown;
    size_t capacity;

    if (depth < workspace->frame_capacity)
        return CFR_STATUS_SUCCESS;
    if (workspace->frame_capacity > SIZE_MAX / 2)
        return CFR_STATUS_OUT_OF_MEMORY;
    capacity = workspace->frame_capacity * 2;
    if (capacity > SIZE_MAX / sizeof(*workspace->frames))
        return CFR_STATUS_OUT_OF_MEMORY;
    grown = realloc(workspace->frames, capacity * sizeof(*grown));
    if (grown == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    workspace->frames = grown;
    workspace->frame_capacity = capacity;
    return CFR_STATUS_SUCCESS;
}

static Status grow_index_table(size_t **table_pointer, size_t *capacity_pointer,
                               const InfoNode *const *nodes,
                               size_t entry_count) {
    size_t *grown;
    size_t capacity;

    if (*capacity_pointer > SIZE_MAX / 2)
        return CFR_STATUS_OUT_OF_MEMORY;
    capacity = *capacity_pointer * 2;
    if (capacity > SIZE_MAX / sizeof(*grown))
        return CFR_STATUS_OUT_OF_MEMORY;
    grown = malloc(capacity * sizeof(*grown));
    if (grown == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    initialize_table(grown, capacity);
    for (size_t entry = 0; entry < entry_count; entry += 1) {
        const size_t mask = capacity - 1;
        size_t cell = hash_node(nodes[entry]) & mask;

        while (grown[cell] != MCCFR_CELL_EMPTY)
            cell = (cell + 1) & mask;
        grown[cell] = entry;
    }
    free(*table_pointer);
    *table_pointer = grown;
    *capacity_pointer = capacity;
    return CFR_STATUS_SUCCESS;
}

static Status grow_delta_table(MccfrWorkspace *workspace) {
    const InfoNode **nodes;
    Status status;

    if (workspace->delta_entry_count > SIZE_MAX / sizeof(*nodes))
        return CFR_STATUS_OUT_OF_MEMORY;
    nodes = malloc(workspace->delta_entry_count * sizeof(*nodes));
    if (nodes == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    for (size_t index = 0; index < workspace->delta_entry_count; index += 1)
        nodes[index] = workspace->delta_entries[index].node;
    status = grow_index_table(&workspace->delta_table,
                              &workspace->delta_table_capacity, nodes,
                              workspace->delta_entry_count);
    free(nodes);
    return status;
}

static Status grow_sample_table(MccfrWorkspace *workspace) {
    const InfoNode **nodes;
    Status status;

    if (workspace->sample_entry_count > SIZE_MAX / sizeof(*nodes))
        return CFR_STATUS_OUT_OF_MEMORY;
    nodes = malloc(workspace->sample_entry_count * sizeof(*nodes));
    if (nodes == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    for (size_t index = 0; index < workspace->sample_entry_count; index += 1)
        nodes[index] = workspace->sample_entries[index].node;
    status = grow_index_table(&workspace->sample_table,
                              &workspace->sample_table_capacity, nodes,
                              workspace->sample_entry_count);
    free(nodes);
    return status;
}

static Status ensure_delta_entries(MccfrWorkspace *workspace) {
    DeltaEntry *grown;
    size_t capacity;

    if (workspace->delta_entry_count < workspace->delta_entry_capacity)
        return CFR_STATUS_SUCCESS;
    if (workspace->delta_entry_capacity > SIZE_MAX / 2)
        return CFR_STATUS_OUT_OF_MEMORY;
    capacity = workspace->delta_entry_capacity * 2;
    if (capacity > SIZE_MAX / sizeof(*grown))
        return CFR_STATUS_OUT_OF_MEMORY;
    grown = realloc(workspace->delta_entries, capacity * sizeof(*grown));
    if (grown == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    workspace->delta_entries = grown;
    workspace->delta_entry_capacity = capacity;
    return CFR_STATUS_SUCCESS;
}

static Status ensure_sample_entries(MccfrWorkspace *workspace) {
    SampleEntry *grown;
    size_t capacity;

    if (workspace->sample_entry_count < workspace->sample_entry_capacity)
        return CFR_STATUS_SUCCESS;
    if (workspace->sample_entry_capacity > SIZE_MAX / 2)
        return CFR_STATUS_OUT_OF_MEMORY;
    capacity = workspace->sample_entry_capacity * 2;
    if (capacity > SIZE_MAX / sizeof(*grown))
        return CFR_STATUS_OUT_OF_MEMORY;
    grown = realloc(workspace->sample_entries, capacity * sizeof(*grown));
    if (grown == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    workspace->sample_entries = grown;
    workspace->sample_entry_capacity = capacity;
    return CFR_STATUS_SUCCESS;
}

static Status ensure_arena(MccfrWorkspace *workspace, size_t action_count) {
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

static Status find_or_create_delta(MccfrWorkspace *workspace, InfoNode *node,
                                   size_t action_count, size_t *index_out) {
    size_t mask;
    size_t cell;
    Status status;

    if ((workspace->delta_table_used + 1) * 4 >
        workspace->delta_table_capacity * 3) {
        status = grow_delta_table(workspace);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    mask = workspace->delta_table_capacity - 1;
    cell = hash_node(node) & mask;
    while (workspace->delta_table[cell] != MCCFR_CELL_EMPTY) {
        const size_t candidate = workspace->delta_table[cell];

        if (workspace->delta_entries[candidate].node == node) {
            *index_out = candidate;
            return CFR_STATUS_SUCCESS;
        }
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
        (DeltaEntry){.node = node,
                     .action_count = action_count,
                     .arena_offset = workspace->arena_used};
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
    double sum = 0.0;
    size_t last_positive = SIZE_MAX;

    if (probabilities == NULL || count == 0 || rng == NULL ||
        index_out == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < count; index += 1) {
        if (!isfinite(probabilities[index]) || probabilities[index] < 0.0)
            return CFR_STATUS_INVALID_ARGUMENT;
        sum += probabilities[index];
        if (probabilities[index] > 0.0)
            last_positive = index;
    }
    if (!isfinite(sum) || sum <= 0.0 || last_positive == SIZE_MAX)
        return CFR_STATUS_INVALID_ARGUMENT;

    const double draw = rng_unit(rng) * sum;
    double cumulative = 0.0;
    for (size_t index = 0; index < count; index += 1) {
        cumulative += probabilities[index];
        if (draw < cumulative && probabilities[index] > 0.0) {
            *index_out = index;
            return CFR_STATUS_SUCCESS;
        }
    }
    *index_out = last_positive;
    return CFR_STATUS_SUCCESS;
}

static Status get_sampled_action(MccfrWorkspace *workspace,
                                 const InfoNode *node,
                                 const Action *actions,
                                 const Probability *strategy,
                                 size_t action_count, size_t *index_out) {
    size_t mask;
    size_t cell;
    Status status;

    if ((workspace->sample_table_used + 1) * 4 >
        workspace->sample_table_capacity * 3) {
        status = grow_sample_table(workspace);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    mask = workspace->sample_table_capacity - 1;
    cell = hash_node(node) & mask;
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

    status = ensure_sample_entries(workspace);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    size_t sampled;
    status = sample_index(strategy, action_count, &workspace->rng, &sampled);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    const size_t entry = workspace->sample_entry_count;
    workspace->sample_entries[entry] = (SampleEntry){
        .node = node, .action_index = sampled, .action_count = action_count};
    for (size_t action = 0; action < action_count; action += 1)
        workspace->sample_entries[entry].actions[action] = actions[action];
    workspace->sample_entry_count += 1;
    workspace->sample_table[cell] = entry;
    workspace->sample_table_used += 1;
    *index_out = sampled;
    return CFR_STATUS_SUCCESS;
}

static Status workspace_check_deltas(const MccfrWorkspace *workspace) {
    for (size_t index = 0; index < workspace->delta_entry_count; index += 1) {
        const DeltaEntry *entry = &workspace->delta_entries[index];
        const Utility *regret = workspace->arena + entry->arena_offset;
        const double *strategy = regret + entry->action_count;
        const Status status = cfr_info_node_check_deltas(
            entry->node, regret, strategy, entry->action_count);

        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    return CFR_STATUS_SUCCESS;
}

static Status workspace_apply_deltas(MccfrWorkspace *workspace) {
    for (size_t index = 0; index < workspace->delta_entry_count; index += 1) {
        DeltaEntry *entry = &workspace->delta_entries[index];
        Utility *regret = workspace->arena + entry->arena_offset;
        double *strategy = regret + entry->action_count;
        const Status status = cfr_info_node_apply_deltas(
            entry->node, regret, strategy, entry->action_count);

        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    return CFR_STATUS_SUCCESS;
}

static Status record_target_deltas(MccfrWorkspace *workspace, InfoNode *node,
                                   const MccfrFrame *frame,
                                   size_t action_count, Utility node_utility,
                                   long double own_reach,
                                   long double external_sample_reach) {
    size_t entry_index;
    Status status = find_or_create_delta(workspace, node, action_count,
                                         &entry_index);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (!(external_sample_reach > 0.0L) ||
        !isfinite(external_sample_reach) || !isfinite(own_reach)) {
        return CFR_STATUS_NUMERIC_ERROR;
    }

    DeltaEntry *entry = &workspace->delta_entries[entry_index];
    Utility *delta_regret = workspace->arena + entry->arena_offset;
    double *delta_strategy = delta_regret + action_count;
    const long double average_weight = own_reach / external_sample_reach;

    if (!isfinite(average_weight) || average_weight < 0.0L ||
        average_weight > DBL_MAX) {
        return CFR_STATUS_NUMERIC_ERROR;
    }
    for (size_t action = 0; action < action_count; action += 1) {
        const Utility regret_change =
            frame->utilities[action] - node_utility;
        const long double strategy_change =
            average_weight * (long double)frame->probabilities[action];

        if (!isfinite(regret_change) || !isfinite(strategy_change) ||
            strategy_change > DBL_MAX) {
            return CFR_STATUS_NUMERIC_ERROR;
        }
        delta_regret[action] += regret_change;
        delta_strategy[action] += (double)strategy_change;
        if (!isfinite(delta_regret[action]) ||
            !isfinite(delta_strategy[action])) {
            return CFR_STATUS_NUMERIC_ERROR;
        }
    }
    return CFR_STATUS_SUCCESS;
}

static Status traverse_target_node(
    const MccfrAdapter *adapter, GameState *state, InfoStore *store,
    Player target_player, size_t depth, long double own_reach,
    long double external_sample_reach, MccfrWorkspace *workspace,
    InfoNode *node, size_t action_count, Utility *utility_out) {
    Utility node_utility = 0.0;

    for (size_t action = 0; action < action_count; action += 1) {
        MccfrFrame *frame = &workspace->frames[depth];
        const long double child_own_reach =
            own_reach * (long double)frame->probabilities[action];

        if (!isfinite(child_own_reach))
            return CFR_STATUS_NUMERIC_ERROR;
        Status status = adapter->operations->apply_action(
            adapter->context, state, frame->actions[action]);
        if (status != CFR_STATUS_SUCCESS)
            return status;

        Utility branch_utility;
        const Status branch_status = traverse_branch(
            adapter, state, store, target_player, depth + 1, child_own_reach,
            external_sample_reach, workspace, &branch_utility);
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
        own_reach, external_sample_reach);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *utility_out = node_utility;
    return CFR_STATUS_SUCCESS;
}

static Status traverse_opponent_node(
    const MccfrAdapter *adapter, GameState *state, InfoStore *store,
    Player target_player, size_t depth, long double own_reach,
    long double external_sample_reach, MccfrWorkspace *workspace,
    const InfoNode *node, size_t action_count, Utility *utility_out) {
    MccfrFrame *frame = &workspace->frames[depth];
    size_t sampled_action;
    Status status = get_sampled_action(
        workspace, node, frame->actions, frame->probabilities, action_count,
        &sampled_action);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    const Probability probability = frame->probabilities[sampled_action];
    const long double child_sample_reach =
        external_sample_reach * (long double)probability;
    if (!(probability > 0.0) || !(child_sample_reach > 0.0L) ||
        !isfinite(child_sample_reach)) {
        return CFR_STATUS_NUMERIC_ERROR;
    }
    status = adapter->operations->apply_action(
        adapter->context, state, frame->actions[sampled_action]);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    Utility branch_utility;
    const Status branch_status = traverse_branch(
        adapter, state, store, target_player, depth + 1, own_reach,
        child_sample_reach, workspace, &branch_utility);
    status = adapter->operations->undo_action(adapter->context, state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (branch_status != CFR_STATUS_SUCCESS)
        return branch_status;
    *utility_out = branch_utility;
    return CFR_STATUS_SUCCESS;
}

static Status traverse_chance_node(
    const MccfrAdapter *adapter, GameState *state, InfoStore *store,
    Player target_player, size_t depth, long double own_reach,
    long double external_sample_reach, MccfrWorkspace *workspace,
    Utility *utility_out) {
    Status status = ensure_frame(workspace, depth);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    MccfrFrame *frame = &workspace->frames[depth];
    size_t action_count;
    const bool batched = adapter->operations->chance_outcomes != NULL;
    if (batched) {
        status = adapter->operations->chance_outcomes(
            adapter->context, state, frame->actions, frame->probabilities,
            CFR_TRAVERSAL_MAX_ACTIONS, &action_count);
    } else {
        status = adapter->operations->legal_actions(
            adapter->context, state, frame->actions,
            CFR_TRAVERSAL_MAX_ACTIONS, &action_count);
    }
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (action_count == 0 || action_count > adapter->max_legal_actions)
        return CFR_STATUS_INVALID_ARGUMENT;

    double probability_sum = 0.0;
    for (size_t action = 0; action < action_count; action += 1) {
        Probability probability;

        if (batched) {
            probability = frame->probabilities[action];
        } else {
            status = adapter->operations->chance_probability(
                adapter->context, state, frame->actions[action], &probability);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            frame->probabilities[action] = probability;
        }
        if (!isfinite(probability) || probability < 0.0)
            return CFR_STATUS_INVALID_ARGUMENT;
        probability_sum += probability;
    }
    if (!isfinite(probability_sum) || !sum_is_one(probability_sum))
        return CFR_STATUS_INVALID_ARGUMENT;

    size_t sampled_action;
    status = sample_index(frame->probabilities, action_count, &workspace->rng,
                          &sampled_action);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    const Probability probability = frame->probabilities[sampled_action];
    const long double child_sample_reach =
        external_sample_reach * (long double)probability;
    if (!(probability > 0.0) || !(child_sample_reach > 0.0L) ||
        !isfinite(child_sample_reach)) {
        return CFR_STATUS_NUMERIC_ERROR;
    }

    status = adapter->operations->apply_action(
        adapter->context, state, frame->actions[sampled_action]);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    Utility branch_utility;
    const Status branch_status = traverse_branch(
        adapter, state, store, target_player, depth + 1, own_reach,
        child_sample_reach, workspace, &branch_utility);
    status = adapter->operations->undo_action(adapter->context, state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (branch_status != CFR_STATUS_SUCCESS)
        return branch_status;
    *utility_out = branch_utility;
    return CFR_STATUS_SUCCESS;
}

static Status traverse_branch(const MccfrAdapter *adapter, GameState *state,
                              InfoStore *store, Player target_player,
                              size_t depth, long double own_reach,
                              long double external_sample_reach,
                              MccfrWorkspace *workspace,
                              Utility *utility_out) {
    if (workspace->visits != SIZE_MAX)
        workspace->visits += 1;

    bool terminal;
    Status status = adapter->operations->is_terminal(adapter->context, state,
                                                      &terminal);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (terminal) {
        Utility utility;
        status = adapter->operations->terminal_utility(
            adapter->context, state, target_player, &utility);
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
        return traverse_chance_node(
            adapter, state, store, target_player, depth, own_reach,
            external_sample_reach, workspace, utility_out);
    }
    if (actor.kind != CFR_ACTOR_PLAYER ||
        (actor.player != CFR_PLAYER_0 && actor.player != CFR_PLAYER_1)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    status = ensure_frame(workspace, depth);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    MccfrFrame *frame = &workspace->frames[depth];
    size_t action_count;
    status = adapter->operations->legal_actions(
        adapter->context, state, frame->actions, CFR_TRAVERSAL_MAX_ACTIONS,
        &action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (action_count == 0 || action_count > adapter->max_legal_actions)
        return CFR_STATUS_INVALID_ARGUMENT;

    InfoSetKey key;
    status = adapter->operations->information_set_key(adapter->context, state,
                                                      &key);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    InfoNode *node;
    status = cfr_info_store_get_or_create(store, key, action_count, &node);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    status = cfr_info_node_current_strategy(node, frame->probabilities,
                                            action_count);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    if (actor.player == target_player) {
        return traverse_target_node(
            adapter, state, store, target_player, depth, own_reach,
            external_sample_reach, workspace, node, action_count, utility_out);
    }
    return traverse_opponent_node(
        adapter, state, store, target_player, depth, own_reach,
        external_sample_reach, workspace, node, action_count, utility_out);
}

Status cfr_mccfr_external_traverse(const Game *game, GameState *state,
                                   InfoStore *store, Player target_player,
                                   MccfrRng *rng, Utility *utility_out) {
    TraversalStats discarded = {0};
    return cfr_mccfr_external_traverse_with_stats(
        game, state, store, target_player, rng, utility_out, &discarded);
}

Status cfr_mccfr_external_traverse_with_stats(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    MccfrRng *rng, Utility *utility_out, TraversalStats *stats_out) {
    if (game == NULL || state == NULL || store == NULL || rng == NULL ||
        utility_out == NULL || stats_out == NULL ||
        game->max_legal_actions == 0 ||
        game->max_legal_actions > CFR_TRAVERSAL_MAX_ACTIONS ||
        (target_player != CFR_PLAYER_0 && target_player != CFR_PLAYER_1)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    Status status = cfr_game_validate_state(game, state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    const GameOperations *operations = game->operations;
    if (game->trusted_operations != NULL)
        operations = game->trusted_operations;
    if (!operations_support_mccfr(operations))
        return CFR_STATUS_INVALID_ARGUMENT;

    MccfrWorkspace workspace = {0};
    status = workspace_init(&workspace, game->max_legal_actions, rng);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    const MccfrAdapter adapter = {.operations = operations,
                                   .context = game->context,
                                   .max_legal_actions =
                                       game->max_legal_actions};
    Utility temporary_utility;
    status = traverse_branch(&adapter, state, store, target_player, 0, 1.0L,
                             1.0L, &workspace, &temporary_utility);
    if (status == CFR_STATUS_SUCCESS)
        status = workspace_check_deltas(&workspace);
    if (status == CFR_STATUS_SUCCESS)
        status = workspace_apply_deltas(&workspace);
    if (status == CFR_STATUS_SUCCESS) {
        *rng = workspace.rng;
        *utility_out = temporary_utility;
        stats_out->visited_nodes = workspace.visits;
    }
    workspace_destroy(&workspace);
    return status;
}

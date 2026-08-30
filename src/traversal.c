#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/traversal.h"

#define CFR_CELL_EMPTY SIZE_MAX
#define INITIAL_FRAME_CAPACITY 32
#define INITIAL_TABLE_CAPACITY 64
#define INITIAL_ENTRY_CAPACITY 16
#define TRAVERSAL_REL_EPSILON 1e-8
#define TRAVERSAL_ABS_EPSILON 1e-12

typedef struct {
    Action actions[CFR_TRAVERSAL_MAX_ACTIONS];
    Probability probabilities[CFR_TRAVERSAL_MAX_ACTIONS];
    Utility utilities[CFR_TRAVERSAL_MAX_ACTIONS];
} Frame;

typedef struct {
    InfoNode *node;
    size_t action_count;
    size_t offset;
} Entry;

typedef struct {
    Frame *frames;
    size_t frame_capacity;
    size_t *table;
    size_t table_capacity;
    size_t used_table;
    Entry *entries;
    size_t used_entries;
    size_t entry_capacity;
    double *arena;
    size_t used_arena;
    size_t reserved_arena;
    size_t visits;
    double strategy_weight;
    bool regret_matching_plus;
} WorkSpace;

static Status cfr_traverse_chance(const Game *game, GameState *state,
                                  InfoStore *store, Player target_player,
                                  size_t depth, Probability reach_0,
                                  Probability reach_1, Probability reach_chance,
                                  WorkSpace *workspace, Utility *utility_out);

static bool sum_is_one(double sum) {
    if (fabs(sum - 1.0) <= TRAVERSAL_ABS_EPSILON)
        return true;
    double max = (sum > 1.0) ? sum : 1.0;
    return fabs(sum - 1.0) <= max * TRAVERSAL_REL_EPSILON;
}

static void workspace_destroy(WorkSpace *workspace) {
    if (workspace == NULL)
        return;

    free(workspace->frames);
    free(workspace->table);
    free(workspace->entries);
    free(workspace->arena);

    *workspace = (WorkSpace){0};
}

static Status workspace_init(WorkSpace *workspace, size_t max_legal_actions,
                             double strategy_weight,
                             bool regret_matching_plus) {
    if (workspace == NULL || max_legal_actions == 0 ||
        max_legal_actions > CFR_TRAVERSAL_MAX_ACTIONS ||
        !isfinite(strategy_weight) || strategy_weight <= 0.0)
        return CFR_STATUS_INVALID_ARGUMENT;

    WorkSpace temporary = {0};

    temporary.frames =
        malloc(INITIAL_FRAME_CAPACITY * sizeof(*temporary.frames));
    if (temporary.frames == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.table = malloc(INITIAL_TABLE_CAPACITY * sizeof(*temporary.table));
    if (temporary.table == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.entries =
        malloc(INITIAL_ENTRY_CAPACITY * sizeof(*temporary.entries));
    if (temporary.entries == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    /*
     * Each entry needs action_count regret deltas and action_count strategy
     * deltas.
     */
    temporary.reserved_arena = INITIAL_ENTRY_CAPACITY * 2 * max_legal_actions;

    temporary.arena =
        malloc(temporary.reserved_arena * sizeof(*temporary.arena));
    if (temporary.arena == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.frame_capacity = INITIAL_FRAME_CAPACITY;
    temporary.table_capacity = INITIAL_TABLE_CAPACITY;
    temporary.entry_capacity = INITIAL_ENTRY_CAPACITY;

    temporary.used_table = 0;
    temporary.used_entries = 0;
    temporary.used_arena = 0;

    for (size_t i = 0; i < temporary.table_capacity; i++)
        temporary.table[i] = CFR_CELL_EMPTY;

    temporary.visits = 0;
    temporary.strategy_weight = strategy_weight;
    temporary.regret_matching_plus = regret_matching_plus;

    *workspace = temporary;
    return CFR_STATUS_SUCCESS;
}

static Status workspace_check_deltas(const WorkSpace *workspace) {
    if (workspace == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t i = 0; i < workspace->used_entries; i++) {
        const Entry *entry = &workspace->entries[i];

        if (entry->offset > workspace->used_arena ||
            entry->action_count > (workspace->used_arena - entry->offset) / 2)
            return CFR_STATUS_INVALID_ARGUMENT;

        const Utility *delta_regret = workspace->arena + entry->offset;

        const double *delta_strategy = delta_regret + entry->action_count;

        Status status = cfr_info_node_check_deltas(
            entry->node, delta_regret, delta_strategy, entry->action_count);

        if (status != CFR_STATUS_SUCCESS)
            return status;
    }

    return CFR_STATUS_SUCCESS;
}

static Status workspace_apply_deltas(WorkSpace *workspace) {
    if (workspace == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t i = 0; i < workspace->used_entries; i++) {
        Entry *entry = &workspace->entries[i];

        Utility *delta_regret = workspace->arena + entry->offset;

        double *delta_strategy = delta_regret + entry->action_count;

        /* All entries have already been validated by workspace_check_deltas. */
        Status status = cfr_info_node_apply_deltas(
            entry->node, delta_regret, delta_strategy, entry->action_count);

        if (status != CFR_STATUS_SUCCESS)
            return status;

        if (workspace->regret_matching_plus) {
            for (size_t action = 0; action < entry->action_count; action++) {
                if (entry->node->regret_sums[action] < 0.0)
                    entry->node->regret_sums[action] = 0.0;
            }
        }
    }

    return CFR_STATUS_SUCCESS;
}

static size_t hash_node(const InfoNode *node) {
    /* Fibonacci hashing. Discard the pointer's low 4 bits because malloc
       alignment makes them nearly constant. */
    uintptr_t value = (uintptr_t)node >> 4;
    return (size_t)(value * 11400714819323198485ULL);
}

static Status grow_table(WorkSpace *ws) {
    size_t new_capacity = ws->table_capacity * 2;
    size_t *new_table = malloc(new_capacity * sizeof(size_t));
    if (new_table == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    for (size_t i = 0; i < new_capacity; i++)
        new_table[i] = CFR_CELL_EMPTY;
    /* Reinsert all entries with the new mask. */
    size_t mask = new_capacity - 1;
    for (size_t e = 0; e < ws->used_entries; e++) {
        size_t cell = hash_node(ws->entries[e].node) & mask;
        while (new_table[cell] != CFR_CELL_EMPTY)
            cell = (cell + 1) & mask;
        new_table[cell] = e;
    }
    free(ws->table);
    ws->table = new_table;
    ws->table_capacity = new_capacity;
    return CFR_STATUS_SUCCESS;
}

static Status find_or_create_entry(WorkSpace *ws, InfoNode *node,
                                   size_t action_count, size_t *index_out) {
    /* Phase 1: grow BEFORE searching if insertion would exceed 3/4 load. */
    if ((ws->used_table + 1) * 4 > ws->table_capacity * 3) {
        Status status = grow_table(ws);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    /* Phase 2: probe linearly from the cell selected by the hash. */
    size_t mask = ws->table_capacity - 1; /* power-of-two capacity */
    size_t cell = hash_node(node) & mask;
    while (ws->table[cell] != CFR_CELL_EMPTY) {
        size_t candidate = ws->table[cell];
        if (ws->entries[candidate].node == node) {
            *index_out = candidate; /* it exists, so reuse it */
            return CFR_STATUS_SUCCESS;
        }
        cell = (cell + 1) & mask; /* collision: try the next cell */
    }
    /* cell is now an empty cell in which to record the new entry. */
    /* Phase 3a: reserve a slot in the entry array. */
    if (ws->used_entries == ws->entry_capacity) {
        size_t new_capacity = ws->entry_capacity * 2;
        Entry *grown = realloc(ws->entries, new_capacity * sizeof(Entry));
        if (grown == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
        ws->entries = grown;
        ws->entry_capacity = new_capacity;
    }
    /* Phase 3b: reserve arena space for 2 * action_count doubles. */
    if (ws->used_arena + 2 * action_count > ws->reserved_arena) {
        size_t new_reserved = ws->reserved_arena * 2;
        while (ws->used_arena + 2 * action_count > new_reserved)
            new_reserved *= 2;
        double *grown = realloc(ws->arena, new_reserved * sizeof(double));
        if (grown == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
        ws->arena = grown;
        ws->reserved_arena = new_reserved;
    }
    /* Phase 3c: initialize the entry with zero deltas. */
    size_t index = ws->used_entries;
    ws->entries[index].node = node;
    ws->entries[index].action_count = action_count;
    ws->entries[index].offset = ws->used_arena;
    for (size_t i = 0; i < 2 * action_count; i++)
        ws->arena[ws->used_arena + i] = 0.0;
    ws->used_arena += 2 * action_count;
    ws->used_entries += 1;
    ws->table[cell] = index;
    ws->used_table += 1;
    *index_out = index;
    return CFR_STATUS_SUCCESS;
}

static Status cfr_traverse_branch(const Game *game, GameState *state,
                                  InfoStore *store, Player target_player,
                                  size_t depth, Probability reach_0,
                                  Probability reach_1, Probability reach_chance,
                                  WorkSpace *workspace, Utility *utility_out) {
    if (game == NULL || state == NULL || store == NULL || utility_out == NULL ||
        workspace == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!(workspace->visits == SIZE_MAX))
        workspace->visits += 1;

    Status status;

    /* Check whether the state is terminal. */
    bool is_terminal;
    status = cfr_game_is_terminal(game, state, &is_terminal);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (is_terminal) {
        Utility terminal_utility;
        status = cfr_game_terminal_utility(game, state, target_player,
                                           &terminal_utility);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (!isfinite(terminal_utility))
            return CFR_STATUS_NUMERIC_ERROR;
        *utility_out = terminal_utility;
        return CFR_STATUS_SUCCESS;
    }

    /* Get the current actor. */
    Actor current_actor;
    status = cfr_game_current_actor(game, state, &current_actor);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (current_actor.kind == CFR_ACTOR_CHANCE)
        return cfr_traverse_chance(game, state, store, target_player, depth,
                                   reach_0, reach_1, reach_chance, workspace,
                                   utility_out);
    if (current_actor.kind != CFR_ACTOR_PLAYER ||
        (current_actor.player != CFR_PLAYER_0 &&
         current_actor.player != CFR_PLAYER_1))
        return CFR_STATUS_INVALID_ARGUMENT;

    /* Grow the frame array when it has insufficient capacity. */
    if (depth == workspace->frame_capacity) {
        size_t new_capacity = workspace->frame_capacity * 2;
        Frame *new_frames =
            realloc(workspace->frames, new_capacity * sizeof(Frame));
        if (new_frames == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
        workspace->frames = new_frames;
        workspace->frame_capacity = new_capacity;
    }

    /* Get the legal actions. */
    Frame *frame = &(workspace->frames[depth]);
    size_t required_amount;
    status =
        cfr_game_legal_actions(game, state, frame->actions,
                               CFR_TRAVERSAL_MAX_ACTIONS, &required_amount);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (required_amount == 0 || required_amount > game->max_legal_actions)
        return CFR_STATUS_INVALID_ARGUMENT;

    /* Get the information-set key. */
    InfoSetKey key;
    status = cfr_game_information_set_key(game, state, &key);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    /* Get or create the information node. */
    InfoNode *node;
    status = cfr_info_store_get_or_create(store, key, required_amount, &node);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    /* Compute the current strategy. */
    status = cfr_info_node_current_strategy(node, frame->probabilities,
                                            required_amount);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    /* Initialize the node utility. */
    Utility node_utility = 0.0;

    for (size_t i = 0; i < required_amount; i++) {
        /* Update the current actor's reach. */
        Probability reach_copy_0 = reach_0;
        Probability reach_copy_1 = reach_1;
        Probability reach_copy_chance = reach_chance;
        switch (current_actor.player) {
        case CFR_PLAYER_0:
            reach_copy_0 *= frame->probabilities[i];
            if (!isfinite(reach_copy_0))
                return CFR_STATUS_NUMERIC_ERROR;
            break;
        case CFR_PLAYER_1:
            reach_copy_1 *= frame->probabilities[i];

            if (!isfinite(reach_copy_1))
                return CFR_STATUS_NUMERIC_ERROR;
            break;
        default:
            return CFR_STATUS_INVALID_ARGUMENT;
        }

        /* Apply the action. */
        status = cfr_game_apply_action(game, state, frame->actions[i]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        Utility branch_utility;

        /* Traverse the child branch. */
        Status status_new_branch = cfr_traverse_branch(
            game, state, store, target_player, depth + 1, reach_copy_0,
            reach_copy_1, reach_copy_chance, workspace, &branch_utility);

        /* Undo the applied action. */
        status = cfr_game_undo_action(game, state);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        frame = &workspace->frames[depth];
        if (status_new_branch != CFR_STATUS_SUCCESS)
            return status_new_branch;

        /* Store the child branch utility. */
        frame->utilities[i] = branch_utility;
        Utility candidate =
            node_utility + branch_utility * frame->probabilities[i];
        if (!isfinite(candidate))
            return CFR_STATUS_NUMERIC_ERROR;
        node_utility = candidate;
    }

    if (current_actor.player == target_player) {
        /* Get the node's pending deltas. */
        size_t index;
        status = find_or_create_entry(workspace, node, required_amount, &index);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        double *delta_regret =
            workspace->arena + workspace->entries[index].offset;
        double *delta_strategy = delta_regret + required_amount;

        /* Select the acting player's and opponent's reaches. */
        Probability own_reach;
        Probability rival_reach;
        switch (current_actor.player) {
        case CFR_PLAYER_0:
            own_reach = reach_0;
            rival_reach = reach_1;
            break;
        case CFR_PLAYER_1:
            own_reach = reach_1;
            rival_reach = reach_0;
            break;
        default:
            return CFR_STATUS_INVALID_ARGUMENT;
        }

        /* Accumulate the pending deltas. */
        for (size_t i = 0; i < required_amount; i++) {
            Utility change = rival_reach * reach_chance *
                             (frame->utilities[i] - node_utility);
            if (!isfinite(change))
                return CFR_STATUS_NUMERIC_ERROR;
            delta_regret[i] += change;
            double strategy_change = own_reach * frame->probabilities[i] *
                                     workspace->strategy_weight;
            if (!isfinite(strategy_change))
                return CFR_STATUS_NUMERIC_ERROR;
            delta_strategy[i] += strategy_change;
            if (!isfinite(delta_strategy[i]))
                return CFR_STATUS_NUMERIC_ERROR;
        }
    }
    *utility_out = node_utility;
    return CFR_STATUS_SUCCESS;
}

static Status cfr_traverse_chance(const Game *game, GameState *state,
                                  InfoStore *store, Player target_player,
                                  size_t depth, Probability reach_0,
                                  Probability reach_1, Probability reach_chance,
                                  WorkSpace *workspace, Utility *utility_out) {
    if (game == NULL || state == NULL || store == NULL || utility_out == NULL ||
        workspace == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    /* Grow the frame array when it has insufficient capacity. */
    if (depth == workspace->frame_capacity) {
        size_t new_capacity = workspace->frame_capacity * 2;
        Frame *new_frames =
            realloc(workspace->frames, new_capacity * sizeof(Frame));
        if (new_frames == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
        workspace->frames = new_frames;
        workspace->frame_capacity = new_capacity;
    }

    /* Get the legal chance outcomes. */
    Frame *frame = &(workspace->frames[depth]);
    size_t required_amount;
    Status status =
        cfr_game_legal_actions(game, state, frame->actions,
                               CFR_TRAVERSAL_MAX_ACTIONS, &required_amount);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (required_amount == 0 || required_amount > game->max_legal_actions)
        return CFR_STATUS_INVALID_ARGUMENT;

    /* Query and validate the complete distribution before applying the first
     * action: an invalid distribution does not traverse any branch. */
    Probability probability_sum = 0.0;
    for (size_t i = 0; i < required_amount; i++) {
        Probability probability;
        status = cfr_game_chance_probability(game, state, frame->actions[i],
                                             &probability);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (!isfinite(probability) || probability < 0.0)
            return CFR_STATUS_INVALID_ARGUMENT;
        frame->probabilities[i] = probability;
        probability_sum += probability;
    }
    if (!isfinite(probability_sum) || !sum_is_one(probability_sum))
        return CFR_STATUS_INVALID_ARGUMENT;

    /* Expected value weighted by the probability of each outcome. */
    Utility expected_utility = 0.0;

    for (size_t i = 0; i < required_amount; i++) {
        Probability child_chance = reach_chance * frame->probabilities[i];
        if (!isfinite(child_chance))
            return CFR_STATUS_NUMERIC_ERROR;

        /* Apply the chance outcome. */
        status = cfr_game_apply_action(game, state, frame->actions[i]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        Utility branch_utility;

        /* Traverse the child branch with the updated chance reach. */
        Status status_new_branch = cfr_traverse_branch(
            game, state, store, target_player, depth + 1, reach_0, reach_1,
            child_chance, workspace, &branch_utility);

        /* Always undo the applied action. */
        status = cfr_game_undo_action(game, state);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        frame = &workspace->frames[depth];
        if (status_new_branch != CFR_STATUS_SUCCESS)
            return status_new_branch;

        /* Store the child branch utility. */
        frame->utilities[i] = branch_utility;
        Utility candidate =
            expected_utility + frame->probabilities[i] * branch_utility;
        if (!isfinite(candidate))
            return CFR_STATUS_NUMERIC_ERROR;
        expected_utility = candidate;
    }

    *utility_out = expected_utility;
    return CFR_STATUS_SUCCESS;
}

Status cfr_traverse(const Game *game, GameState *state, InfoStore *store,
                    Player target_player, Utility *utility_out) {
    TraversalStats discard = {0};
    Status status = cfr_traverse_with_stats(game, state, store, target_player,
                                            utility_out, &discard);
    return status;
}

Status cfr_traverse_plus(const Game *game, GameState *state, InfoStore *store,
                         Player target_player, size_t iteration,
                         Utility *utility_out) {
    TraversalStats discard = {0};
    return cfr_traverse_plus_with_stats(game, state, store, target_player,
                                        iteration, utility_out, &discard);
}

static Status traverse_with_stats(const Game *game, GameState *state,
                                  InfoStore *store, Player target_player,
                                  double strategy_weight,
                                  bool regret_matching_plus,
                                  Utility *utility_out,
                                  TraversalStats *stats_out) {
    Game trusted_game;
    const Game *traversal_game = game;

    if (game == NULL || state == NULL || store == NULL || utility_out == NULL ||
        stats_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (game->max_legal_actions == 0 ||
        (game->max_legal_actions > CFR_TRAVERSAL_MAX_ACTIONS))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (target_player != CFR_PLAYER_0 && target_player != CFR_PLAYER_1)
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status = cfr_game_validate_state(game, state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (game->trusted_operations != NULL) {
        trusted_game = *game;
        trusted_game.operations = game->trusted_operations;
        trusted_game.trusted_operations = NULL;
        traversal_game = &trusted_game;
    }
    WorkSpace ws = {0};
    status = workspace_init(&ws, traversal_game->max_legal_actions,
                            strategy_weight,
                            regret_matching_plus);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    Utility temp_utility = 0.0;
    status = cfr_traverse_branch(traversal_game, state, store, target_player, 0,
                                 1.0, 1.0, 1.0, &ws, &temp_utility);
    if (status == CFR_STATUS_SUCCESS)
        status = workspace_check_deltas(&ws);
    if (status == CFR_STATUS_SUCCESS)
        status = workspace_apply_deltas(&ws);
    if (status == CFR_STATUS_SUCCESS) {
        *utility_out = temp_utility;
        stats_out->visited_nodes = ws.visits;
    }
    workspace_destroy(&ws);
    return status;
}

Status cfr_traverse_with_stats(const Game *game, GameState *state,
                               InfoStore *store, Player target_player,
                               Utility *utility_out,
                               TraversalStats *stats_out) {
    return traverse_with_stats(game, state, store, target_player, 1.0, false,
                               utility_out, stats_out);
}

Status cfr_traverse_plus_with_stats(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    size_t iteration, Utility *utility_out, TraversalStats *stats_out) {
    if (iteration == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    return traverse_with_stats(game, state, store, target_player,
                               (double)iteration, true, utility_out, stats_out);
}

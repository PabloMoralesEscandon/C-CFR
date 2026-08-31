#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/evaluation.h"

static const double EVALUATION_ABS_EPSILON = 1e-12;
static const double EVALUATION_REL_EPSILON = 1e-8;
#define EVALUATION_INDEX_EMPTY SIZE_MAX
#define INITIAL_EDGE_CAPACITY ((size_t)8)
#define INITIAL_FRAME_CAPACITY ((size_t)8)
#define INITIAL_GROUP_CAPACITY ((size_t)8)
#define INITIAL_GROUP_TABLE_CAPACITY ((size_t)8)
#define INITIAL_NODE_CAPACITY ((size_t)8)

typedef enum {
    EVALUATION_NODE_TERMINAL,
    EVALUATION_NODE_CHANCE,
    EVALUATION_NODE_PLAYER
} EvaluationNodeKind;

typedef enum {
    GROUP_LOCATE_INVALID_ARGUMENT,
    GROUP_LOCATE_FOUND,
    GROUP_LOCATE_EMPTY_SLOT,
    GROUP_LOCATE_FULL
} GroupLocateResult;

typedef struct {
    Action action;
    Probability profile_probability;
    size_t child_index;
} EvaluationEdge;

typedef struct {
    EvaluationNodeKind kind;
    Player player;
    const InfoNode *policy_node;
    size_t edge_offset;
    size_t action_count;
    Utility terminal_utility_player_0;
    Utility terminal_utility_player_1;
    Probability counterfactual_reach_0;
    Probability counterfactual_reach_1;
    size_t next_same_information;
    bool cache_ready_player_0;
    Utility cache_player_0;
    bool cache_ready_player_1;
    Utility cache_player_1;
} EvaluationNode;

typedef struct {
    const InfoNode *policy_node;
    Player player;
    size_t first_node_index;
    size_t action_count;
    size_t selected_action_index;
    bool solving;
    bool solved;
} InformationGroup;

typedef struct {
    Action *actions;
    Probability *probabilities;
} EvaluationFrame;

typedef struct {
    EvaluationNode *nodes;
    size_t node_count;
    size_t node_capacity;

    EvaluationEdge *edges;
    size_t edge_count;
    size_t edge_capacity;

    InformationGroup *groups;
    size_t group_count;
    size_t group_capacity;

    size_t *group_table;
    size_t group_table_used;
    size_t group_table_capacity;

    EvaluationFrame *frames;
    size_t frame_count;
    size_t frame_capacity;

    size_t max_legal_actions;
    InfoStore unvisited_store;
    bool unvisited_store_initialized;
    bool use_uniform_for_unvisited;
} EvaluationWorkspace;

static Status build(const Game *game, GameState *state, const InfoStore *store,
                    size_t depth, EvaluationWorkspace *workspace,
                    Probability counterfactual_reach_0,
                    Probability counterfactual_reach_1, size_t *index_out);

static Status best_response_value(EvaluationWorkspace *workspace,
                                  size_t node_index, Player responder,
                                  Utility *value_out);

static void workspace_destroy(EvaluationWorkspace *workspace) {
    if (workspace == NULL)
        return;

    if (workspace->frames != NULL) {
        for (size_t i = 0; i < workspace->frame_count; i++) {
            free(workspace->frames[i].actions);
            free(workspace->frames[i].probabilities);
        }
    }

    free(workspace->frames);
    free(workspace->group_table);
    free(workspace->groups);
    free(workspace->edges);
    free(workspace->nodes);
    if (workspace->unvisited_store_initialized)
        (void)cfr_info_store_destroy(&workspace->unvisited_store);

    *workspace = (EvaluationWorkspace){0};
}

static Status workspace_init(EvaluationWorkspace *workspace,
                             size_t max_legal_actions,
                             bool use_uniform_for_unvisited) {
    if (workspace == NULL || max_legal_actions == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (max_legal_actions > SIZE_MAX / sizeof(Action) ||
        max_legal_actions > SIZE_MAX / sizeof(Probability))
        return CFR_STATUS_OUT_OF_MEMORY;

    EvaluationWorkspace temporary = {0};

    temporary.nodes =
        malloc(INITIAL_NODE_CAPACITY * sizeof(*temporary.nodes));
    if (temporary.nodes == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.edges =
        malloc(INITIAL_EDGE_CAPACITY * sizeof(*temporary.edges));
    if (temporary.edges == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.groups =
        malloc(INITIAL_GROUP_CAPACITY * sizeof(*temporary.groups));
    if (temporary.groups == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.group_table =
        malloc(INITIAL_GROUP_TABLE_CAPACITY * sizeof(*temporary.group_table));
    if (temporary.group_table == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.frames =
        malloc(INITIAL_FRAME_CAPACITY * sizeof(*temporary.frames));
    if (temporary.frames == NULL) {
        workspace_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < INITIAL_GROUP_TABLE_CAPACITY; i++)
        temporary.group_table[i] = EVALUATION_INDEX_EMPTY;

    for (size_t i = 0; i < INITIAL_FRAME_CAPACITY; i++)
        temporary.frames[i] = (EvaluationFrame){0};

    temporary.node_capacity = INITIAL_NODE_CAPACITY;
    temporary.edge_capacity = INITIAL_EDGE_CAPACITY;
    temporary.group_capacity = INITIAL_GROUP_CAPACITY;
    temporary.group_table_capacity = INITIAL_GROUP_TABLE_CAPACITY;
    temporary.frame_capacity = INITIAL_FRAME_CAPACITY;
    temporary.max_legal_actions = max_legal_actions;
    temporary.use_uniform_for_unvisited = use_uniform_for_unvisited;
    if (use_uniform_for_unvisited) {
        const Status status =
            cfr_info_store_init(&temporary.unvisited_store);
        if (status != CFR_STATUS_SUCCESS) {
            workspace_destroy(&temporary);
            return status;
        }
        temporary.unvisited_store_initialized = true;
    }

    *workspace = temporary;
    return CFR_STATUS_SUCCESS;
}

static bool sum_is_one(double sum) {
    double difference = fabs(sum - 1.0);
    if (difference <= EVALUATION_ABS_EPSILON)
        return true;

    double scale = sum > 1.0 ? sum : 1.0;
    return difference <= scale * EVALUATION_REL_EPSILON;
}

static bool utilities_are_zero_sum(Utility utility_0, Utility utility_1) {
    double difference = fabs(utility_0 + utility_1);
    if (difference <= EVALUATION_ABS_EPSILON)
        return true;

    double magnitude_0 = fabs(utility_0);
    double magnitude_1 = fabs(utility_1);
    double scale = magnitude_0 > magnitude_1 ? magnitude_0 : magnitude_1;
    return difference <= scale * EVALUATION_REL_EPSILON;
}

static bool negative_is_rounding_noise(Utility improvement,
                                       Utility best_response,
                                       Utility profile_value) {
    double difference = fabs(improvement);
    if (difference <= EVALUATION_ABS_EPSILON)
        return true;

    double best_response_magnitude = fabs(best_response);
    double profile_magnitude = fabs(profile_value);
    double scale = best_response_magnitude > profile_magnitude
                       ? best_response_magnitude
                       : profile_magnitude;
    return difference <= scale * EVALUATION_REL_EPSILON;
}

static Status ensure_frame(EvaluationWorkspace *workspace, size_t depth) {
    if (workspace == NULL || workspace->max_legal_actions == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (workspace->max_legal_actions > SIZE_MAX / sizeof(Action) ||
        workspace->max_legal_actions > SIZE_MAX / sizeof(Probability))
        return CFR_STATUS_OUT_OF_MEMORY;

    if (depth < workspace->frame_count)
        return CFR_STATUS_SUCCESS;

    if (depth == SIZE_MAX)
        return CFR_STATUS_OUT_OF_MEMORY;

    size_t required_count = depth + 1;

    if (required_count > workspace->frame_capacity) {
        size_t old_capacity = workspace->frame_capacity;
        size_t new_capacity =
            old_capacity == 0 ? INITIAL_FRAME_CAPACITY : old_capacity;

        while (new_capacity < required_count) {
            if (new_capacity > SIZE_MAX / 2)
                return CFR_STATUS_OUT_OF_MEMORY;

            new_capacity *= 2;
        }

        if (new_capacity > SIZE_MAX / sizeof(EvaluationFrame))
            return CFR_STATUS_OUT_OF_MEMORY;

        EvaluationFrame *grown = realloc(
            workspace->frames, new_capacity * sizeof(EvaluationFrame));

        if (grown == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;

        for (size_t i = old_capacity; i < new_capacity; i++)
            grown[i] = (EvaluationFrame){0};

        workspace->frames = grown;
        workspace->frame_capacity = new_capacity;
    }

    while (workspace->frame_count < required_count) {
        Action *actions =
            malloc(workspace->max_legal_actions * sizeof(Action));

        if (actions == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;

        Probability *probabilities =
            malloc(workspace->max_legal_actions * sizeof(Probability));

        if (probabilities == NULL) {
            free(actions);
            return CFR_STATUS_OUT_OF_MEMORY;
        }

        size_t index = workspace->frame_count;
        workspace->frames[index].actions = actions;
        workspace->frames[index].probabilities = probabilities;
        workspace->frame_count++;
    }

    return CFR_STATUS_SUCCESS;
}

static Status workspace_append_node(EvaluationWorkspace *workspace,
                                    const EvaluationNode *node,
                                    size_t *index_out) {
    if (workspace == NULL || node == NULL || index_out == NULL ||
        workspace->node_count > workspace->node_capacity ||
        (workspace->node_capacity > 0 && workspace->nodes == NULL))
        return CFR_STATUS_INVALID_ARGUMENT;

    EvaluationNode node_copy = *node;

    if (workspace->node_count == workspace->node_capacity) {
        size_t new_capacity = workspace->node_capacity;
        if (new_capacity == 0) {
            new_capacity = INITIAL_NODE_CAPACITY;
        } else {
            if (new_capacity > SIZE_MAX / 2)
                return CFR_STATUS_OUT_OF_MEMORY;
            new_capacity *= 2;
        }

        if (new_capacity > SIZE_MAX / sizeof(*workspace->nodes))
            return CFR_STATUS_OUT_OF_MEMORY;

        EvaluationNode *grown = realloc(
            workspace->nodes, new_capacity * sizeof(*workspace->nodes));
        if (grown == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;

        workspace->nodes = grown;
        workspace->node_capacity = new_capacity;
    }

    size_t index = workspace->node_count;
    workspace->nodes[index] = node_copy;
    workspace->node_count++;
    *index_out = index;
    return CFR_STATUS_SUCCESS;
}

static Status workspace_reserve_edges(EvaluationWorkspace *workspace,
                                      size_t amount, size_t *offset_out) {
    if (workspace == NULL || amount == 0 || offset_out == NULL ||
        workspace->edge_count > workspace->edge_capacity ||
        (workspace->edge_capacity > 0 && workspace->edges == NULL))
        return CFR_STATUS_INVALID_ARGUMENT;

    if (amount > SIZE_MAX - workspace->edge_count)
        return CFR_STATUS_OUT_OF_MEMORY;

    size_t required_count = workspace->edge_count + amount;

    if (required_count > workspace->edge_capacity) {
        size_t new_capacity = workspace->edge_capacity;
        if (new_capacity == 0)
            new_capacity = INITIAL_EDGE_CAPACITY;

        while (new_capacity < required_count) {
            if (new_capacity > SIZE_MAX / 2)
                return CFR_STATUS_OUT_OF_MEMORY;
            new_capacity *= 2;
        }

        if (new_capacity > SIZE_MAX / sizeof(*workspace->edges))
            return CFR_STATUS_OUT_OF_MEMORY;

        EvaluationEdge *grown = realloc(
            workspace->edges, new_capacity * sizeof(*workspace->edges));
        if (grown == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;

        workspace->edges = grown;
        workspace->edge_capacity = new_capacity;
    }

    size_t offset = workspace->edge_count;
    for (size_t i = 0; i < amount; i++) {
        workspace->edges[offset + i] = (EvaluationEdge){0};
        workspace->edges[offset + i].child_index = EVALUATION_INDEX_EMPTY;
    }

    workspace->edge_count = required_count;
    *offset_out = offset;
    return CFR_STATUS_SUCCESS;
}

static size_t hash_info_node(const InfoNode *node) {
    uint64_t value = (uint64_t)(uintptr_t)node;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return (size_t)value;
}

static GroupLocateResult
group_table_locate(const EvaluationWorkspace *workspace,
                   const InfoNode *policy_node, size_t *cell_out,
                   size_t *group_index_out) {
    if (workspace == NULL || policy_node == NULL || cell_out == NULL ||
        group_index_out == NULL || workspace->group_table == NULL ||
        workspace->group_table_capacity == 0 ||
        (workspace->group_table_capacity &
         (workspace->group_table_capacity - 1)) != 0 ||
        workspace->group_table_used > workspace->group_table_capacity ||
        workspace->group_table_used != workspace->group_count ||
        workspace->group_count > workspace->group_capacity ||
        (workspace->group_count > 0 && workspace->groups == NULL))
        return GROUP_LOCATE_INVALID_ARGUMENT;

    size_t mask = workspace->group_table_capacity - 1;
    size_t initial_cell = hash_info_node(policy_node) & mask;

    for (size_t probe = 0; probe < workspace->group_table_capacity; probe++) {
        size_t cell = (initial_cell + probe) & mask;
        size_t group_index = workspace->group_table[cell];

        if (group_index == EVALUATION_INDEX_EMPTY) {
            *cell_out = cell;
            return GROUP_LOCATE_EMPTY_SLOT;
        }

        if (group_index >= workspace->group_count)
            return GROUP_LOCATE_INVALID_ARGUMENT;

        if (workspace->groups[group_index].policy_node == policy_node) {
            *cell_out = cell;
            *group_index_out = group_index;
            return GROUP_LOCATE_FOUND;
        }
    }

    return GROUP_LOCATE_FULL;
}

static Status workspace_grow_group_table(EvaluationWorkspace *workspace) {
    if (workspace == NULL || workspace->group_table == NULL ||
        workspace->group_table_capacity == 0 ||
        (workspace->group_table_capacity &
         (workspace->group_table_capacity - 1)) != 0 ||
        workspace->group_table_used != workspace->group_count ||
        workspace->group_count > workspace->group_capacity ||
        (workspace->group_count > 0 && workspace->groups == NULL))
        return CFR_STATUS_INVALID_ARGUMENT;

    if (workspace->group_table_capacity > SIZE_MAX / 2)
        return CFR_STATUS_OUT_OF_MEMORY;

    size_t new_capacity = workspace->group_table_capacity * 2;
    if (new_capacity > SIZE_MAX / sizeof(*workspace->group_table))
        return CFR_STATUS_OUT_OF_MEMORY;

    size_t *new_table = malloc(new_capacity * sizeof(*new_table));
    if (new_table == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;

    for (size_t i = 0; i < new_capacity; i++)
        new_table[i] = EVALUATION_INDEX_EMPTY;

    size_t mask = new_capacity - 1;
    for (size_t group_index = 0; group_index < workspace->group_count;
         group_index++) {
        const InfoNode *policy_node =
            workspace->groups[group_index].policy_node;
        if (policy_node == NULL) {
            free(new_table);
            return CFR_STATUS_INVALID_ARGUMENT;
        }

        size_t initial_cell = hash_info_node(policy_node) & mask;
        bool inserted = false;

        for (size_t probe = 0; probe < new_capacity; probe++) {
            size_t cell = (initial_cell + probe) & mask;
            size_t candidate = new_table[cell];

            if (candidate == EVALUATION_INDEX_EMPTY) {
                new_table[cell] = group_index;
                inserted = true;
                break;
            }

            if (workspace->groups[candidate].policy_node == policy_node) {
                free(new_table);
                return CFR_STATUS_INVALID_ARGUMENT;
            }
        }

        if (!inserted) {
            free(new_table);
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }

    free(workspace->group_table);
    workspace->group_table = new_table;
    workspace->group_table_capacity = new_capacity;
    return CFR_STATUS_SUCCESS;
}

static Status validate_group_actions(const EvaluationWorkspace *workspace,
                                     size_t first_node_index,
                                     size_t new_node_index) {
    if (workspace == NULL || workspace->nodes == NULL ||
        workspace->edges == NULL || first_node_index >= workspace->node_count ||
        new_node_index >= workspace->node_count)
        return CFR_STATUS_INVALID_ARGUMENT;

    const EvaluationNode *first = &workspace->nodes[first_node_index];
    const EvaluationNode *current = &workspace->nodes[new_node_index];

    if (first->kind != EVALUATION_NODE_PLAYER ||
        current->kind != EVALUATION_NODE_PLAYER || first->policy_node == NULL ||
        first->policy_node != current->policy_node ||
        first->player != current->player || first->action_count == 0 ||
        first->action_count != current->action_count ||
        first->edge_offset > workspace->edge_count ||
        first->action_count > workspace->edge_count - first->edge_offset ||
        current->edge_offset > workspace->edge_count ||
        current->action_count > workspace->edge_count - current->edge_offset)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t i = 0; i < first->action_count; i++) {
        const EvaluationEdge *first_edge =
            &workspace->edges[first->edge_offset + i];
        const EvaluationEdge *current_edge =
            &workspace->edges[current->edge_offset + i];

        if (first_edge->action != current_edge->action)
            return CFR_STATUS_INVALID_ARGUMENT;
    }

    return CFR_STATUS_SUCCESS;
}

static Status workspace_find_or_create_group(EvaluationWorkspace *workspace,
                                             size_t node_index,
                                             size_t *group_index_out) {
    if (workspace == NULL || group_index_out == NULL ||
        workspace->nodes == NULL || node_index >= workspace->node_count ||
        workspace->groups == NULL || workspace->group_table == NULL ||
        workspace->group_table_capacity == 0 ||
        workspace->group_count > workspace->group_capacity ||
        workspace->group_table_used != workspace->group_count)
        return CFR_STATUS_INVALID_ARGUMENT;

    EvaluationNode *node = &workspace->nodes[node_index];
    if (node->kind != EVALUATION_NODE_PLAYER || node->policy_node == NULL ||
        (node->player != CFR_PLAYER_0 && node->player != CFR_PLAYER_1) ||
        node->action_count == 0 || node->edge_offset > workspace->edge_count ||
        node->action_count > workspace->edge_count - node->edge_offset ||
        node->next_same_information != EVALUATION_INDEX_EMPTY)
        return CFR_STATUS_INVALID_ARGUMENT;

    size_t cell;
    size_t group_index;
    GroupLocateResult locate_result =
        group_table_locate(workspace, node->policy_node, &cell, &group_index);

    if (locate_result == GROUP_LOCATE_INVALID_ARGUMENT)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (locate_result == GROUP_LOCATE_FOUND) {
        if (group_index >= workspace->group_count ||
            workspace->groups[group_index].policy_node != node->policy_node ||
            workspace->groups[group_index].player != node->player ||
            workspace->groups[group_index].action_count != node->action_count ||
            workspace->groups[group_index].first_node_index == node_index ||
            workspace->groups[group_index].selected_action_index !=
                EVALUATION_INDEX_EMPTY ||
            workspace->groups[group_index].solving ||
            workspace->groups[group_index].solved)
            return CFR_STATUS_INVALID_ARGUMENT;

        Status status = validate_group_actions(
            workspace, workspace->groups[group_index].first_node_index,
            node_index);
        if (status != CFR_STATUS_SUCCESS)
            return status;

        node = &workspace->nodes[node_index];
        node->next_same_information =
            workspace->groups[group_index].first_node_index;
        workspace->groups[group_index].first_node_index = node_index;
        *group_index_out = group_index;
        return CFR_STATUS_SUCCESS;
    }

    size_t load_limit =
        workspace->group_table_capacity - workspace->group_table_capacity / 4;
    if (locate_result == GROUP_LOCATE_FULL ||
        workspace->group_table_used >= load_limit) {
        Status status = workspace_grow_group_table(workspace);
        if (status != CFR_STATUS_SUCCESS)
            return status;

        locate_result = group_table_locate(workspace, node->policy_node, &cell,
                                           &group_index);
        if (locate_result != GROUP_LOCATE_EMPTY_SLOT)
            return CFR_STATUS_INVALID_ARGUMENT;
    } else if (locate_result != GROUP_LOCATE_EMPTY_SLOT) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    if (workspace->group_count == workspace->group_capacity) {
        size_t new_capacity = workspace->group_capacity;
        if (new_capacity == 0) {
            new_capacity = INITIAL_GROUP_CAPACITY;
        } else {
            if (new_capacity > SIZE_MAX / 2)
                return CFR_STATUS_OUT_OF_MEMORY;
            new_capacity *= 2;
        }

        if (new_capacity > SIZE_MAX / sizeof(*workspace->groups))
            return CFR_STATUS_OUT_OF_MEMORY;

        InformationGroup *grown = realloc(
            workspace->groups, new_capacity * sizeof(*workspace->groups));
        if (grown == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;

        workspace->groups = grown;
        workspace->group_capacity = new_capacity;
    }

    group_index = workspace->group_count;
    workspace->groups[group_index] = (InformationGroup){
        .policy_node = node->policy_node,
        .player = node->player,
        .first_node_index = node_index,
        .action_count = node->action_count,
        .selected_action_index = EVALUATION_INDEX_EMPTY,
        .solving = false,
        .solved = false,
    };
    workspace->group_table[cell] = group_index;
    workspace->group_count++;
    workspace->group_table_used++;
    *group_index_out = group_index;
    return CFR_STATUS_SUCCESS;
}

static Status workspace_build_snapshot(const Game *game, GameState *state,
                                       const InfoStore *store,
                                       bool use_uniform_for_unvisited,
                                       EvaluationWorkspace *workspace_out,
                                       size_t *root_index_out) {
    if (game == NULL || state == NULL || store == NULL ||
        workspace_out == NULL || root_index_out == NULL ||
        game->max_legal_actions == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    EvaluationWorkspace temporary = {0};
    Status status = cfr_game_validate_state(game, state);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    Game trusted_game;
    const Game *evaluation_game = game;
    if (game->trusted_operations != NULL) {
        trusted_game = *game;
        trusted_game.operations = game->trusted_operations;
        trusted_game.trusted_operations = NULL;
        evaluation_game = &trusted_game;
    }
    status = workspace_init(&temporary, game->max_legal_actions,
                            use_uniform_for_unvisited);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    size_t root_index;
    status = build(evaluation_game, state, store, 0, &temporary, 1.0, 1.0,
                   &root_index);
    if (status != CFR_STATUS_SUCCESS) {
        workspace_destroy(&temporary);
        return status;
    }
    if (root_index >= temporary.node_count) {
        workspace_destroy(&temporary);
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    *workspace_out = temporary;
    *root_index_out = root_index;
    return CFR_STATUS_SUCCESS;
}

static Status build(const Game *game, GameState *state, const InfoStore *store,
                    size_t depth, EvaluationWorkspace *workspace,
                    Probability counterfactual_reach_0,
                    Probability counterfactual_reach_1, size_t *index_out) {
    if (game == NULL || state == NULL || store == NULL || workspace == NULL ||
        index_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!(isfinite(counterfactual_reach_0) && 0.0 <= counterfactual_reach_0 &&
          1.0 >= counterfactual_reach_0))
        return CFR_STATUS_INVALID_ARGUMENT;
    if (!(isfinite(counterfactual_reach_1) && 0.0 <= counterfactual_reach_1 &&
          1.0 >= counterfactual_reach_1))
        return CFR_STATUS_INVALID_ARGUMENT;
    EvaluationNode new_node = {0};
    new_node.counterfactual_reach_0 = counterfactual_reach_0;
    new_node.counterfactual_reach_1 = counterfactual_reach_1;
    Status status;
    bool is_terminal;
    status = cfr_game_is_terminal(game, state, &is_terminal);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (is_terminal) {
        Utility utility_player_0;
        status = cfr_game_terminal_utility(game, state, CFR_PLAYER_0,
                                           &utility_player_0);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        Utility utility_player_1;
        status = cfr_game_terminal_utility(game, state, CFR_PLAYER_1,
                                           &utility_player_1);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (!isfinite(utility_player_0) || !isfinite(utility_player_1))
            return CFR_STATUS_NUMERIC_ERROR;

        Utility utility_sum = utility_player_0 + utility_player_1;
        if (!isfinite(utility_sum))
            return CFR_STATUS_NUMERIC_ERROR;
        if (!utilities_are_zero_sum(utility_player_0, utility_player_1))
            return CFR_STATUS_INVALID_ARGUMENT;
        new_node.kind = EVALUATION_NODE_TERMINAL;
        new_node.policy_node = NULL;
        new_node.action_count = 0;
        new_node.next_same_information = EVALUATION_INDEX_EMPTY;
        new_node.terminal_utility_player_0 = utility_player_0;
        new_node.terminal_utility_player_1 = utility_player_1;
        new_node.cache_ready_player_0 = false;
        new_node.cache_ready_player_1 = false;
        return workspace_append_node(workspace, &new_node, index_out);
    }
    status = ensure_frame(workspace, depth);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    EvaluationFrame *frame = &workspace->frames[depth];
    Actor current_actor;
    status = cfr_game_current_actor(game, state, &current_actor);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    const bool batched_chance =
        current_actor.kind == CFR_ACTOR_CHANCE &&
        game->operations->chance_outcomes != NULL;
    size_t required_amount;
    if (batched_chance) {
        status = cfr_game_chance_outcomes(
            game, state, frame->actions, frame->probabilities,
            game->max_legal_actions, &required_amount);
    } else {
        status = cfr_game_legal_actions(game, state, frame->actions,
                                        game->max_legal_actions,
                                        &required_amount);
    }
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (required_amount == 0 || required_amount > workspace->max_legal_actions)
        return CFR_STATUS_INVALID_ARGUMENT;

    if (current_actor.kind == CFR_ACTOR_CHANCE) {
        for (size_t i = 0; i < required_amount; i++) {
            if (!batched_chance) {
                status = cfr_game_chance_probability(
                    game, state, workspace->frames[depth].actions[i],
                    &(frame->probabilities[i]));
                if (status != CFR_STATUS_SUCCESS)
                    return status;
            }
            if (!isfinite(frame->probabilities[i]) ||
                (workspace->frames[depth].probabilities[i] > 1.0 ||
                 (workspace->frames[depth].probabilities[i] < 0.0)))
                return CFR_STATUS_INVALID_ARGUMENT;
        }
        Probability probability_sum = 0.0;
        for (size_t i = 0; i < required_amount; i++) {
            Probability candidate = probability_sum + frame->probabilities[i];
            if (!isfinite(candidate))
                return CFR_STATUS_NUMERIC_ERROR;
            probability_sum = candidate;
        }
        if (!sum_is_one(probability_sum))
            return CFR_STATUS_INVALID_ARGUMENT;
        new_node.kind = EVALUATION_NODE_CHANCE;
        new_node.policy_node = NULL;
        new_node.action_count = required_amount;
        new_node.next_same_information = EVALUATION_INDEX_EMPTY;
    } else if (current_actor.kind == CFR_ACTOR_PLAYER) {
        if (current_actor.player != CFR_PLAYER_0 &&
            current_actor.player != CFR_PLAYER_1)
            return CFR_STATUS_INVALID_ARGUMENT;
        InfoSetKey key;
        status = cfr_game_information_set_key(game, state, &key);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        const InfoNode *node;
        status = cfr_info_store_find_const(store, key, &node);
        if (status == CFR_STATUS_NOT_FOUND &&
            workspace->use_uniform_for_unvisited) {
            InfoNode *temporary_node;

            status = cfr_info_store_get_or_create(
                &workspace->unvisited_store, key, required_amount,
                &temporary_node);
            if (status == CFR_STATUS_SUCCESS)
                node = temporary_node;
        }
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (node->action_count != required_amount)
            return CFR_STATUS_INVALID_ARGUMENT;
        status = cfr_info_node_average_strategy(node, frame->probabilities,
                                                required_amount);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        new_node.player = current_actor.player;
        new_node.kind = EVALUATION_NODE_PLAYER;
        new_node.policy_node = node;
        new_node.action_count = required_amount;
        new_node.next_same_information = EVALUATION_INDEX_EMPTY;
    } else
        return CFR_STATUS_INVALID_ARGUMENT;
    size_t edge_offset;
    status = workspace_reserve_edges(workspace, required_amount, &edge_offset);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    new_node.edge_offset = edge_offset;

    for (size_t i = 0; i < required_amount; i++) {
        EvaluationEdge *edge = &workspace->edges[edge_offset + i];
        edge->action = frame->actions[i];
        edge->profile_probability = frame->probabilities[i];
    }

    size_t parent_index;
    status = workspace_append_node(workspace, &new_node, &parent_index);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    if (new_node.kind == EVALUATION_NODE_PLAYER) {
        size_t group_index;
        status = workspace_find_or_create_group(workspace, parent_index,
                                                &group_index);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    for (size_t i = 0; i < required_amount; i++) {
        Action action = workspace->edges[edge_offset + i].action;
        Probability probability =
            workspace->edges[edge_offset + i].profile_probability;
        Probability counterfactual_reach_0_copy = counterfactual_reach_0;
        Probability counterfactual_reach_1_copy = counterfactual_reach_1;
        if (current_actor.kind == CFR_ACTOR_CHANCE) {
            counterfactual_reach_0_copy *= probability;
            counterfactual_reach_1_copy *= probability;
        } else if (current_actor.player == CFR_PLAYER_0)
            counterfactual_reach_1_copy *= probability;
        else if (current_actor.player == CFR_PLAYER_1)
            counterfactual_reach_0_copy *= probability;
        if (!isfinite(counterfactual_reach_0_copy) ||
            !isfinite(counterfactual_reach_1_copy))
            return CFR_STATUS_NUMERIC_ERROR;
        status = cfr_game_apply_action(game, state, action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        size_t child_index;
        Status child_status = build(game, state, store, depth + 1, workspace,
                                    counterfactual_reach_0_copy,
                                    counterfactual_reach_1_copy, &child_index);
        status = cfr_game_undo_action(game, state);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (child_status != CFR_STATUS_SUCCESS)
            return child_status;
        workspace->edges[edge_offset + i].child_index = child_index;
    }
    *index_out = parent_index;
    return CFR_STATUS_SUCCESS;
}

static Status profile_value(const EvaluationWorkspace *workspace,
                            size_t node_index, Player player,
                            Utility *value_out) {
    if (workspace == NULL || workspace->nodes == NULL || value_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (node_index >= workspace->node_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (player != CFR_PLAYER_0 && player != CFR_PLAYER_1)
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status;
    if (workspace->nodes[node_index].kind == EVALUATION_NODE_TERMINAL) {
        if (player == CFR_PLAYER_0)
            *value_out = workspace->nodes[node_index].terminal_utility_player_0;
        else if (player == CFR_PLAYER_1)
            *value_out = workspace->nodes[node_index].terminal_utility_player_1;
        return CFR_STATUS_SUCCESS;
    } else if (workspace->nodes[node_index].kind == EVALUATION_NODE_PLAYER ||
               workspace->nodes[node_index].kind == EVALUATION_NODE_CHANCE) {
        const EvaluationNode *node = &workspace->nodes[node_index];
        if (workspace->edges == NULL || node->action_count == 0 ||
            node->edge_offset > workspace->edge_count ||
            node->action_count > workspace->edge_count - node->edge_offset)
            return CFR_STATUS_INVALID_ARGUMENT;
        Utility accumulated_utility = 0.0;
        for (size_t i = 0; i < node->action_count; i++) {
            const EvaluationEdge *edge =
                &workspace->edges[node->edge_offset + i];
            if (!isfinite(edge->profile_probability) ||
                edge->profile_probability < 0.0 ||
                edge->profile_probability > 1.0 ||
                edge->child_index >= workspace->node_count)
                return CFR_STATUS_INVALID_ARGUMENT;

            Utility child_utility;
            status = profile_value(workspace, edge->child_index, player,
                                   &child_utility);
            if (status != CFR_STATUS_SUCCESS)
                return status;

            Utility contribution = child_utility * edge->profile_probability;
            Utility candidate = accumulated_utility + contribution;
            if (!isfinite(contribution) || !isfinite(candidate))
                return CFR_STATUS_NUMERIC_ERROR;
            accumulated_utility = candidate;
        }
        *value_out = accumulated_utility;
    } else
        return CFR_STATUS_INVALID_ARGUMENT;
    return CFR_STATUS_SUCCESS;
}

static Status solve_group(EvaluationWorkspace *workspace, size_t group_index,
                          Player responder) {
    if (workspace == NULL || workspace->groups == NULL ||
        workspace->nodes == NULL || workspace->edges == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (responder != CFR_PLAYER_0 && responder != CFR_PLAYER_1)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (group_index >= workspace->group_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    InformationGroup *group = &(workspace->groups[group_index]);
    if (group->policy_node == NULL || group->player != responder ||
        group->action_count == 0 ||
        group->first_node_index >= workspace->node_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (group->solved) {
        if (group->solving ||
            group->selected_action_index >= group->action_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        return CFR_STATUS_SUCCESS;
    }
    if (group->solving)
        return CFR_STATUS_INVALID_ARGUMENT;
    group->solving = true;
    bool best_is_set = false;
    Utility current_max;
    size_t best_action_index = EVALUATION_INDEX_EMPTY;
    for (size_t action_index = 0; action_index < group->action_count;
         action_index++) {
        size_t current_index = group->first_node_index;
        Utility action_total = 0.0;
        while (current_index != EVALUATION_INDEX_EMPTY) {
            if (current_index >= workspace->node_count) {
                group->solving = false;
                return CFR_STATUS_INVALID_ARGUMENT;
            }

            const EvaluationNode *node = &workspace->nodes[current_index];
            if (node->kind != EVALUATION_NODE_PLAYER ||
                node->policy_node != group->policy_node ||
                node->player != responder ||
                node->action_count != group->action_count ||
                node->edge_offset > workspace->edge_count ||
                node->action_count >
                    workspace->edge_count - node->edge_offset) {
                group->solving = false;
                return CFR_STATUS_INVALID_ARGUMENT;
            }

            size_t edge_index = node->edge_offset + action_index;
            size_t child_index = workspace->edges[edge_index].child_index;

            if (child_index >= workspace->node_count) {
                group->solving = false;
                return CFR_STATUS_INVALID_ARGUMENT;
            }

            Probability counterfactual_reach;
            if (responder == CFR_PLAYER_0)
                counterfactual_reach = node->counterfactual_reach_0;
            else
                counterfactual_reach = node->counterfactual_reach_1;

            if (!isfinite(counterfactual_reach) || counterfactual_reach < 0.0 ||
                counterfactual_reach > 1.0) {
                group->solving = false;
                return CFR_STATUS_INVALID_ARGUMENT;
            }

            size_t next_index = node->next_same_information;

            Utility edge_value;
            Status status = best_response_value(workspace, child_index,
                                                responder, &edge_value);
            if (status != CFR_STATUS_SUCCESS) {
                group->solving = false;
                return status;
            }

            Utility contribution = counterfactual_reach * edge_value;
            Utility candidate = action_total + contribution;

            if (!isfinite(contribution) || !isfinite(candidate)) {
                group->solving = false;
                return CFR_STATUS_NUMERIC_ERROR;
            }

            action_total = candidate;
            current_index = next_index;
        }

        if (!best_is_set || action_total > current_max) {
            best_is_set = true;
            current_max = action_total;
            best_action_index = action_index;
        }
    }
    if (!best_is_set || best_action_index == EVALUATION_INDEX_EMPTY) {
        group->solving = false;
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    group->selected_action_index = best_action_index;
    group->solving = false;
    group->solved = true;

    return CFR_STATUS_SUCCESS;
}

static Status best_response_value(EvaluationWorkspace *workspace,
                                  size_t node_index, Player responder,
                                  Utility *value_out) {
    if (workspace == NULL || workspace->nodes == NULL ||
        node_index >= workspace->node_count || value_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (responder != CFR_PLAYER_0 && responder != CFR_PLAYER_1)
        return CFR_STATUS_INVALID_ARGUMENT;

    EvaluationNode *node = &workspace->nodes[node_index];
    if (responder == CFR_PLAYER_0 && node->cache_ready_player_0) {
        if (!isfinite(node->cache_player_0))
            return CFR_STATUS_NUMERIC_ERROR;
        *value_out = node->cache_player_0;
        return CFR_STATUS_SUCCESS;
    }
    if (responder == CFR_PLAYER_1 && node->cache_ready_player_1) {
        if (!isfinite(node->cache_player_1))
            return CFR_STATUS_NUMERIC_ERROR;
        *value_out = node->cache_player_1;
        return CFR_STATUS_SUCCESS;
    }

    Utility result;
    Status status;

    if (node->kind == EVALUATION_NODE_TERMINAL) {
        if (node->action_count != 0)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (responder == CFR_PLAYER_0)
            result = node->terminal_utility_player_0;
        else
            result = node->terminal_utility_player_1;
    } else if (node->kind == EVALUATION_NODE_CHANCE ||
               (node->kind == EVALUATION_NODE_PLAYER &&
                node->player != responder)) {
        if (node->kind == EVALUATION_NODE_PLAYER &&
            (node->policy_node == NULL ||
             (node->player != CFR_PLAYER_0 && node->player != CFR_PLAYER_1)))
            return CFR_STATUS_INVALID_ARGUMENT;
        if (workspace->edges == NULL || node->action_count == 0 ||
            node->edge_offset > workspace->edge_count ||
            node->action_count > workspace->edge_count - node->edge_offset)
            return CFR_STATUS_INVALID_ARGUMENT;

        result = 0.0;
        for (size_t action_index = 0; action_index < node->action_count;
             action_index++) {
            const EvaluationEdge *edge =
                &workspace->edges[node->edge_offset + action_index];
            if (!isfinite(edge->profile_probability) ||
                edge->profile_probability < 0.0 ||
                edge->profile_probability > 1.0 ||
                edge->child_index >= workspace->node_count)
                return CFR_STATUS_INVALID_ARGUMENT;

            Utility child_value;
            status = best_response_value(workspace, edge->child_index,
                                         responder, &child_value);
            if (status != CFR_STATUS_SUCCESS)
                return status;

            Utility contribution = edge->profile_probability * child_value;
            Utility candidate = result + contribution;
            if (!isfinite(contribution) || !isfinite(candidate))
                return CFR_STATUS_NUMERIC_ERROR;
            result = candidate;
        }
    } else if (node->kind == EVALUATION_NODE_PLAYER &&
               node->player == responder) {
        if (node->policy_node == NULL || workspace->edges == NULL ||
            node->action_count == 0 ||
            node->edge_offset > workspace->edge_count ||
            node->action_count > workspace->edge_count - node->edge_offset)
            return CFR_STATUS_INVALID_ARGUMENT;

        size_t cell;
        size_t group_index = EVALUATION_INDEX_EMPTY;
        GroupLocateResult locate_result = group_table_locate(
            workspace, node->policy_node, &cell, &group_index);
        if (locate_result != GROUP_LOCATE_FOUND)
            return CFR_STATUS_INVALID_ARGUMENT;

        status = solve_group(workspace, group_index, responder);
        if (status != CFR_STATUS_SUCCESS)
            return status;

        node = &workspace->nodes[node_index];
        if (group_index >= workspace->group_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        const InformationGroup *group = &workspace->groups[group_index];
        if (group->policy_node != node->policy_node ||
            group->player != responder ||
            group->action_count != node->action_count || !group->solved ||
            group->solving ||
            group->selected_action_index >= node->action_count)
            return CFR_STATUS_INVALID_ARGUMENT;

        const EvaluationEdge *selected_edge =
            &workspace->edges[node->edge_offset + group->selected_action_index];
        if (selected_edge->child_index >= workspace->node_count)
            return CFR_STATUS_INVALID_ARGUMENT;

        status = best_response_value(workspace, selected_edge->child_index,
                                     responder, &result);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    } else
        return CFR_STATUS_INVALID_ARGUMENT;

    if (!isfinite(result))
        return CFR_STATUS_NUMERIC_ERROR;

    node = &workspace->nodes[node_index];
    if (responder == CFR_PLAYER_0) {
        node->cache_player_0 = result;
        node->cache_ready_player_0 = true;
    } else {
        node->cache_player_1 = result;
        node->cache_ready_player_1 = true;
    }
    *value_out = result;
    return CFR_STATUS_SUCCESS;
}

static Status calculate_metrics(EvaluationWorkspace *workspace,
                                size_t root_node_index,
                                EvaluationMetrics *out) {
    if (workspace == NULL || out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status;
    Utility profile_player_0;
    status = profile_value(workspace, root_node_index, CFR_PLAYER_0,
                           &profile_player_0);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    Utility profile_player_1;
    status = profile_value(workspace, root_node_index, CFR_PLAYER_1,
                           &profile_player_1);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    Utility best_response_player_0;
    status = best_response_value(workspace, root_node_index, CFR_PLAYER_0,
                                 &best_response_player_0);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    Utility best_response_player_1;
    status = best_response_value(workspace, root_node_index, CFR_PLAYER_1,
                                 &best_response_player_1);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    Utility profile_sum = profile_player_0 + profile_player_1;
    if (!isfinite(profile_sum))
        return CFR_STATUS_NUMERIC_ERROR;
    if (!utilities_are_zero_sum(profile_player_0, profile_player_1))
        return CFR_STATUS_INVALID_ARGUMENT;

    Utility improvement_player_0 = best_response_player_0 - profile_player_0;
    Utility improvement_player_1 = best_response_player_1 - profile_player_1;

    if (!isfinite(improvement_player_0) || !isfinite(improvement_player_1))
        return CFR_STATUS_NUMERIC_ERROR;

    if (improvement_player_0 < 0.0) {
        if (!negative_is_rounding_noise(
                improvement_player_0, best_response_player_0, profile_player_0))
            return CFR_STATUS_NUMERIC_ERROR;
        improvement_player_0 = 0.0;
    }
    if (improvement_player_1 < 0.0) {
        if (!negative_is_rounding_noise(
                improvement_player_1, best_response_player_1, profile_player_1))
            return CFR_STATUS_NUMERIC_ERROR;
        improvement_player_1 = 0.0;
    }

    Utility nash_conv = improvement_player_0 + improvement_player_1;
    if (!isfinite(nash_conv))
        return CFR_STATUS_NUMERIC_ERROR;

    Utility exploitability = nash_conv / 2.0;
    if (!isfinite(exploitability))
        return CFR_STATUS_NUMERIC_ERROR;

    out->best_response_value_player_0 = best_response_player_0;
    out->best_response_value_player_1 = best_response_player_1;
    out->improvement_player_0 = improvement_player_0;
    out->improvement_player_1 = improvement_player_1;
    out->profile_value_player_0 = profile_player_0;
    out->profile_value_player_1 = profile_player_1;
    out->nash_conv = nash_conv;
    out->exploitability = exploitability;
    return CFR_STATUS_SUCCESS;
}

Status cfr_evaluation_average_strategy(const InfoStore *store, InfoSetKey key,
                                       Probability *strategy_out,
                                       size_t capacity,
                                       size_t *required_count) {
    if (store == NULL || strategy_out == NULL || required_count == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status;
    const InfoNode *node;
    status = cfr_info_store_find_const(store, key, &node);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    size_t required_count_temp = node->action_count;
    if (capacity < required_count_temp) {
        *required_count = required_count_temp;
        return CFR_STATUS_BUFFER_TOO_SMALL;
    }
    status = cfr_info_node_average_strategy(node, strategy_out, capacity);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *required_count = required_count_temp;
    return CFR_STATUS_SUCCESS;
}

Status cfr_evaluation_profile_value(const Game *game, GameState *state,
                                    const InfoStore *store, Player player,
                                    Utility *utility_out) {
    if (game == NULL || state == NULL || store == NULL || utility_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (player != CFR_PLAYER_0 && player != CFR_PLAYER_1)
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status;
    EvaluationWorkspace workspace = {0};
    size_t root_index;
    status =
        workspace_build_snapshot(game, state, store, false, &workspace,
                                 &root_index);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    Utility profile_value_temp;
    status = profile_value(&workspace, root_index, player, &profile_value_temp);
    workspace_destroy(&workspace);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *utility_out = profile_value_temp;
    return CFR_STATUS_SUCCESS;
}

Status cfr_evaluation_best_response_value(const Game *game, GameState *state,
                                          const InfoStore *store, Player player,
                                          Utility *utility_out) {
    if (game == NULL || state == NULL || store == NULL || utility_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (player != CFR_PLAYER_0 && player != CFR_PLAYER_1)
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status;
    EvaluationWorkspace workspace = {0};
    size_t root_index;
    status =
        workspace_build_snapshot(game, state, store, false, &workspace,
                                 &root_index);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    Utility best_response_temp;
    status = best_response_value(&workspace, root_index, player,
                                 &best_response_temp);
    workspace_destroy(&workspace);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *utility_out = best_response_temp;
    return CFR_STATUS_SUCCESS;
}

Status cfr_evaluation_metrics(const Game *game, GameState *state,
                              const InfoStore *store,
                              EvaluationMetrics *eval_out) {
    if (game == NULL || state == NULL || store == NULL || eval_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status;
    EvaluationWorkspace workspace = {0};
    size_t root_index;
    status =
        workspace_build_snapshot(game, state, store, false, &workspace,
                                 &root_index);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    EvaluationMetrics eval_temp = {0};
    status = calculate_metrics(&workspace, root_index, &eval_temp);
    workspace_destroy(&workspace);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *eval_out = eval_temp;
    return CFR_STATUS_SUCCESS;
}

Status cfr_evaluation_metrics_with_unvisited_uniform(
    const Game *game, GameState *state, const InfoStore *store,
    EvaluationMetrics *eval_out) {
    if (game == NULL || state == NULL || store == NULL || eval_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    EvaluationWorkspace workspace = {0};
    size_t root_index;
    Status status = workspace_build_snapshot(game, state, store, true,
                                             &workspace, &root_index);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    EvaluationMetrics temporary = {0};
    status = calculate_metrics(&workspace, root_index, &temporary);
    workspace_destroy(&workspace);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    *eval_out = temporary;
    return CFR_STATUS_SUCCESS;
}

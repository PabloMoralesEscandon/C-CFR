#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfr/blackjack.h"
#include "cfr/checkpoint.h"
#include "cfr/evaluation.h"
#include "cfr/game.h"
#include "cfr/info_store.h"

#define BLACKJACK_INFOSET_COUNT (10 * 2 * 22)
#define EMPTY_INDEX SIZE_MAX
#define INITIAL_EDGE_CAPACITY 2048
#define INITIAL_NODE_CAPACITY 1024
#define INITIAL_TABLE_CAPACITY 2048

typedef enum {
    COMPACT_TERMINAL,
    COMPACT_CHANCE,
    COMPACT_PLAYER
} CompactNodeKind;

typedef struct {
    uint8_t phase;
    uint8_t player_total;
    uint8_t player_card_count;
    uint8_t player_ace_count;
    uint8_t player_is_soft;
    uint8_t dealer_total;
    uint8_t dealer_card_count;
    uint8_t dealer_ace_count;
    uint8_t dealer_is_soft;
    uint8_t dealer_up_card;
    uint8_t cards_remaining;
    uint8_t remaining_cards[CFR_BLACKJACK_NUMBER_OF_CARD_RANKS];
    uint8_t depth;
} CompactState;

typedef struct {
    size_t child;
    Probability probability;
    Action action;
} CompactEdge;

typedef struct {
    CompactState state;
    CompactNodeKind kind;
    InfoSetKey information_key;
    Utility terminal_utility;
    size_t edge_offset;
    size_t edge_count;
    size_t next_in_group;
    size_t next_at_depth;
    double counterfactual_reach;
    double own_reach;
    size_t raw_occurrences;
    Utility profile_value;
    Utility best_response_value;
    Utility iteration_value;
    bool profile_ready;
    bool best_response_ready;
    bool iteration_ready;
} CompactNode;

typedef struct {
    CompactNode *nodes;
    size_t node_count;
    size_t node_capacity;
    CompactEdge *edges;
    size_t edge_count;
    size_t edge_capacity;
    size_t *table;
    size_t table_used;
    size_t table_capacity;
    size_t group_heads[BLACKJACK_INFOSET_COUNT];
    size_t depth_heads[CFR_BLACKJACK_UNDO_HISTORY_CAPACITY + 1];
    size_t maximum_depth;
    size_t raw_visit_count;
    const Game *game;
    const GameOperations *operations;
} CompactGraph;

typedef struct {
    bool present;
    bool selected;
    size_t selected_action;
    InfoNode *node;
    Probability current[2];
    Probability learned[2];
    Utility action_total[2];
    double counterfactual_reach;
} CompactPolicy;

static void graph_destroy(CompactGraph *graph) {
    if (graph == NULL)
        return;
    free(graph->table);
    free(graph->edges);
    free(graph->nodes);
    *graph = (CompactGraph){0};
}

static bool checked_double(size_t value, size_t *result) {
    if (result == NULL || value > SIZE_MAX / 2)
        return false;
    *result = value * 2;
    return true;
}

static Status graph_init(CompactGraph *graph, const Game *game) {
    if (graph == NULL || game == NULL || game->operations == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    CompactGraph temporary = {0};
    temporary.nodes =
        malloc(INITIAL_NODE_CAPACITY * sizeof(*temporary.nodes));
    temporary.edges =
        malloc(INITIAL_EDGE_CAPACITY * sizeof(*temporary.edges));
    temporary.table =
        malloc(INITIAL_TABLE_CAPACITY * sizeof(*temporary.table));
    if (temporary.nodes == NULL || temporary.edges == NULL ||
        temporary.table == NULL) {
        graph_destroy(&temporary);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    temporary.node_capacity = INITIAL_NODE_CAPACITY;
    temporary.edge_capacity = INITIAL_EDGE_CAPACITY;
    temporary.table_capacity = INITIAL_TABLE_CAPACITY;
    temporary.game = game;
    temporary.operations = game->trusted_operations != NULL
                               ? game->trusted_operations
                               : game->operations;
    for (size_t index = 0; index < temporary.table_capacity; index++)
        temporary.table[index] = EMPTY_INDEX;
    for (size_t index = 0; index < BLACKJACK_INFOSET_COUNT; index++)
        temporary.group_heads[index] = EMPTY_INDEX;
    for (size_t index = 0;
         index <= CFR_BLACKJACK_UNDO_HISTORY_CAPACITY; index++)
        temporary.depth_heads[index] = EMPTY_INDEX;

    *graph = temporary;
    return CFR_STATUS_SUCCESS;
}

static Status compact_state_from_blackjack(const BlackjackState *state,
                                           CompactState *compact) {
    if (state == NULL || compact == NULL || state->phase < 0 ||
        state->phase > CFR_BLACKJACK_PHASE_TERMINAL ||
        state->player_hand.total < 0 || state->player_hand.total > UINT8_MAX ||
        state->dealer_hand.total < 0 || state->dealer_hand.total > UINT8_MAX ||
        state->player_hand.card_count > UINT8_MAX ||
        state->player_hand.ace_count > UINT8_MAX ||
        state->dealer_hand.card_count > UINT8_MAX ||
        state->dealer_hand.ace_count > UINT8_MAX ||
        state->dealer_up_card < 0 || state->dealer_up_card > UINT8_MAX ||
        state->cards_remaining > UINT8_MAX ||
        state->undo_count > CFR_BLACKJACK_UNDO_HISTORY_CAPACITY ||
        state->undo_count > UINT8_MAX)
        return CFR_STATUS_INVALID_ARGUMENT;

    CompactState result = {0};
    result.phase = (uint8_t)state->phase;
    result.player_total = (uint8_t)state->player_hand.total;
    result.player_card_count = (uint8_t)state->player_hand.card_count;
    result.player_ace_count = (uint8_t)state->player_hand.ace_count;
    result.player_is_soft = state->player_hand.is_soft ? 1 : 0;
    result.dealer_total = (uint8_t)state->dealer_hand.total;
    result.dealer_card_count = (uint8_t)state->dealer_hand.card_count;
    result.dealer_ace_count = (uint8_t)state->dealer_hand.ace_count;
    result.dealer_is_soft = state->dealer_hand.is_soft ? 1 : 0;
    result.dealer_up_card = (uint8_t)state->dealer_up_card;
    result.cards_remaining = (uint8_t)state->cards_remaining;
    result.depth = (uint8_t)state->undo_count;
    for (size_t index = 0; index < CFR_BLACKJACK_NUMBER_OF_CARD_RANKS;
         index++) {
        if (state->remaining_cards[index] > UINT8_MAX)
            return CFR_STATUS_INVALID_ARGUMENT;
        result.remaining_cards[index] =
            (uint8_t)state->remaining_cards[index];
    }
    *compact = result;
    return CFR_STATUS_SUCCESS;
}

static size_t compact_hash(const CompactState *state) {
    const unsigned char *bytes = (const unsigned char *)state;
    uint64_t hash = UINT64_C(1469598103934665603);

    for (size_t index = 0; index < sizeof(*state); index++) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 32;
    return (size_t)hash;
}

static Status graph_grow_table(CompactGraph *graph) {
    size_t new_capacity;
    if (graph == NULL || graph->table == NULL ||
        !checked_double(graph->table_capacity, &new_capacity) ||
        new_capacity > SIZE_MAX / sizeof(*graph->table))
        return CFR_STATUS_OUT_OF_MEMORY;

    size_t *grown = malloc(new_capacity * sizeof(*grown));
    if (grown == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    for (size_t index = 0; index < new_capacity; index++)
        grown[index] = EMPTY_INDEX;

    const size_t mask = new_capacity - 1;
    for (size_t index = 0; index < graph->node_count; index++) {
        size_t cell = compact_hash(&graph->nodes[index].state) & mask;
        while (grown[cell] != EMPTY_INDEX)
            cell = (cell + 1) & mask;
        grown[cell] = index;
    }

    free(graph->table);
    graph->table = grown;
    graph->table_capacity = new_capacity;
    return CFR_STATUS_SUCCESS;
}

static Status graph_find(const CompactGraph *graph, const CompactState *state,
                         size_t *cell_out, size_t *index_out, bool *found_out) {
    if (graph == NULL || state == NULL || cell_out == NULL ||
        index_out == NULL || found_out == NULL || graph->table == NULL ||
        graph->table_capacity == 0)
        return CFR_STATUS_INVALID_ARGUMENT;

    const size_t mask = graph->table_capacity - 1;
    size_t cell = compact_hash(state) & mask;
    for (size_t probe = 0; probe < graph->table_capacity; probe++) {
        const size_t index = graph->table[cell];
        if (index == EMPTY_INDEX) {
            *cell_out = cell;
            *found_out = false;
            return CFR_STATUS_SUCCESS;
        }
        if (index >= graph->node_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        if (memcmp(&graph->nodes[index].state, state, sizeof(*state)) == 0) {
            *cell_out = cell;
            *index_out = index;
            *found_out = true;
            return CFR_STATUS_SUCCESS;
        }
        cell = (cell + 1) & mask;
    }
    return CFR_STATUS_OUT_OF_MEMORY;
}

static Status graph_append_node(CompactGraph *graph,
                                const CompactState *state,
                                size_t *index_out) {
    if (graph == NULL || state == NULL || index_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    if ((graph->table_used + 1) * 4 > graph->table_capacity * 3) {
        Status status = graph_grow_table(graph);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }

    size_t cell = 0;
    size_t existing = 0;
    bool found = false;
    Status status = graph_find(graph, state, &cell, &existing, &found);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (found) {
        *index_out = existing;
        return CFR_STATUS_SUCCESS;
    }

    if (graph->node_count == graph->node_capacity) {
        size_t new_capacity;
        if (!checked_double(graph->node_capacity, &new_capacity) ||
            new_capacity > SIZE_MAX / sizeof(*graph->nodes))
            return CFR_STATUS_OUT_OF_MEMORY;
        CompactNode *grown =
            realloc(graph->nodes, new_capacity * sizeof(*grown));
        if (grown == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
        graph->nodes = grown;
        graph->node_capacity = new_capacity;
    }

    const size_t index = graph->node_count;
    graph->nodes[index] = (CompactNode){
        .state = *state,
        .information_key = -1,
        .next_in_group = EMPTY_INDEX,
        .next_at_depth = graph->depth_heads[state->depth],
    };
    graph->depth_heads[state->depth] = index;
    if (state->depth > graph->maximum_depth)
        graph->maximum_depth = state->depth;
    graph->node_count++;
    graph->table[cell] = index;
    graph->table_used++;
    *index_out = index;

    if (graph->node_count % 1000000 == 0) {
        (void)fprintf(stderr, "graph nodes=%zu edges=%zu\n",
                      graph->node_count, graph->edge_count);
        (void)fflush(stderr);
    }
    return CFR_STATUS_SUCCESS;
}

static Status graph_reserve_edges(CompactGraph *graph, size_t amount,
                                  size_t *offset_out) {
    if (graph == NULL || amount == 0 || offset_out == NULL ||
        amount > SIZE_MAX - graph->edge_count)
        return CFR_STATUS_INVALID_ARGUMENT;

    const size_t required = graph->edge_count + amount;
    if (required > graph->edge_capacity) {
        size_t new_capacity = graph->edge_capacity;
        while (new_capacity < required) {
            if (!checked_double(new_capacity, &new_capacity))
                return CFR_STATUS_OUT_OF_MEMORY;
        }
        if (new_capacity > SIZE_MAX / sizeof(*graph->edges))
            return CFR_STATUS_OUT_OF_MEMORY;
        CompactEdge *grown =
            realloc(graph->edges, new_capacity * sizeof(*grown));
        if (grown == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
        graph->edges = grown;
        graph->edge_capacity = new_capacity;
    }

    *offset_out = graph->edge_count;
    graph->edge_count = required;
    return CFR_STATUS_SUCCESS;
}

static Status graph_build_node(CompactGraph *graph, BlackjackState *state,
                               size_t *index_out) {
    CompactState compact;
    Status status = compact_state_from_blackjack(state, &compact);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    size_t cell = 0;
    size_t index = 0;
    bool found = false;
    status = graph_find(graph, &compact, &cell, &index, &found);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (found) {
        *index_out = index;
        return CFR_STATUS_SUCCESS;
    }

    status = graph_append_node(graph, &compact, &index);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    bool terminal = false;
    status = graph->operations->is_terminal(graph->game->context,
                                            (const GameState *)state,
                                            &terminal);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (terminal) {
        Utility utility = 0.0;
        status = graph->operations->terminal_utility(
            graph->game->context, (const GameState *)state, CFR_PLAYER_0,
            &utility);
        if (status != CFR_STATUS_SUCCESS || !isfinite(utility))
            return status == CFR_STATUS_SUCCESS ? CFR_STATUS_NUMERIC_ERROR
                                                : status;
        graph->nodes[index].kind = COMPACT_TERMINAL;
        graph->nodes[index].terminal_utility = utility;
        *index_out = index;
        return CFR_STATUS_SUCCESS;
    }

    Actor actor = {0};
    status = graph->operations->current_actor(
        graph->game->context, (const GameState *)state, &actor);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    Action actions[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {0};
    Probability probabilities[CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS] = {0};
    size_t action_count = 0;
    if (actor.kind == CFR_ACTOR_CHANCE &&
        graph->operations->chance_outcomes != NULL) {
        status = graph->operations->chance_outcomes(
            graph->game->context, (const GameState *)state, actions,
            probabilities, CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS, &action_count);
    } else {
        status = graph->operations->legal_actions(
            graph->game->context, (const GameState *)state, actions,
            CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS, &action_count);
    }
    if (status != CFR_STATUS_SUCCESS || action_count == 0 ||
        action_count > CFR_BLACKJACK_MAX_POSSIBLE_ACTIONS)
        return status == CFR_STATUS_SUCCESS ? CFR_STATUS_INVALID_ARGUMENT
                                            : status;

    size_t edge_offset = 0;
    status = graph_reserve_edges(graph, action_count, &edge_offset);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    graph->nodes[index].edge_offset = edge_offset;
    graph->nodes[index].edge_count = action_count;

    if (actor.kind == CFR_ACTOR_CHANCE) {
        graph->nodes[index].kind = COMPACT_CHANCE;
    } else if (actor.kind == CFR_ACTOR_PLAYER &&
               actor.player == CFR_PLAYER_0 && action_count == 2) {
        InfoSetKey key = -1;
        status = graph->operations->information_set_key(
            graph->game->context, (const GameState *)state, &key);
        if (status != CFR_STATUS_SUCCESS || key < 0 ||
            key >= BLACKJACK_INFOSET_COUNT)
            return status == CFR_STATUS_SUCCESS ? CFR_STATUS_INVALID_ARGUMENT
                                                : status;
        graph->nodes[index].kind = COMPACT_PLAYER;
        graph->nodes[index].information_key = key;
        graph->nodes[index].next_in_group = graph->group_heads[key];
        graph->group_heads[key] = index;
    } else {
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    for (size_t action_index = 0; action_index < action_count;
         action_index++) {
        Probability probability = actor.kind == CFR_ACTOR_CHANCE
                                      ? probabilities[action_index]
                                      : 1.0;
        if (!isfinite(probability) || probability < 0.0 || probability > 1.0)
            return CFR_STATUS_INVALID_ARGUMENT;

        status = graph->operations->apply_action(
            graph->game->context, (GameState *)state, actions[action_index]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        size_t child = EMPTY_INDEX;
        Status child_status = graph_build_node(graph, state, &child);
        status = graph->operations->undo_action(graph->game->context,
                                                (GameState *)state);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (child_status != CFR_STATUS_SUCCESS)
            return child_status;

        graph->edges[edge_offset + action_index] = (CompactEdge){
            .child = child,
            .probability = probability,
            .action = actions[action_index],
        };
    }

    *index_out = index;
    return CFR_STATUS_SUCCESS;
}

static Status graph_counterfactual_reach(CompactGraph *graph,
                                         size_t root_index) {
    if (graph == NULL || root_index >= graph->node_count)
        return CFR_STATUS_INVALID_ARGUMENT;

    graph->nodes[root_index].counterfactual_reach = 1.0;
    graph->nodes[root_index].raw_occurrences = 1;
    for (size_t depth = 0; depth <= graph->maximum_depth; depth++) {
        size_t index = graph->depth_heads[depth];
        while (index != EMPTY_INDEX) {
            if (index >= graph->node_count)
                return CFR_STATUS_INVALID_ARGUMENT;
            CompactNode *node = &graph->nodes[index];
            if (!isfinite(node->counterfactual_reach) ||
                node->counterfactual_reach < 0.0)
                return CFR_STATUS_NUMERIC_ERROR;
            for (size_t action = 0; action < node->edge_count; action++) {
                const CompactEdge *edge =
                    &graph->edges[node->edge_offset + action];
                if (edge->child >= graph->node_count ||
                    graph->nodes[edge->child].state.depth != depth + 1)
                    return CFR_STATUS_INVALID_ARGUMENT;
                const double weight = node->kind == COMPACT_CHANCE
                                          ? edge->probability
                                          : 1.0;
                const double addition = node->counterfactual_reach * weight;
                const double candidate =
                    graph->nodes[edge->child].counterfactual_reach + addition;
                if (!isfinite(addition) || !isfinite(candidate))
                    return CFR_STATUS_NUMERIC_ERROR;
                graph->nodes[edge->child].counterfactual_reach = candidate;
                if (graph->nodes[edge->child].raw_occurrences >
                    SIZE_MAX - node->raw_occurrences)
                    return CFR_STATUS_NUMERIC_ERROR;
                graph->nodes[edge->child].raw_occurrences +=
                    node->raw_occurrences;
            }
            if (graph->raw_visit_count > SIZE_MAX - node->raw_occurrences)
                return CFR_STATUS_NUMERIC_ERROR;
            graph->raw_visit_count += node->raw_occurrences;
            index = node->next_at_depth;
        }
    }
    return CFR_STATUS_SUCCESS;
}

static Status policy_load(InfoStore *store, const CompactGraph *graph,
                          CompactPolicy policies[BLACKJACK_INFOSET_COUNT]) {
    if (store == NULL || graph == NULL || policies == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        if (graph->group_heads[key] == EMPTY_INDEX)
            continue;
        InfoNode *node = NULL;
        Status status =
            cfr_info_store_find(store, (InfoSetKey)key, &node);
        if (status != CFR_STATUS_SUCCESS || node == NULL ||
            node->action_count != 2)
            return status == CFR_STATUS_SUCCESS ? CFR_STATUS_INVALID_ARGUMENT
                                                : status;
        size_t required = 0;
        status = cfr_evaluation_average_strategy(
            store, (InfoSetKey)key, policies[key].learned, 2, &required);
        if (status != CFR_STATUS_SUCCESS || required != 2)
            return status == CFR_STATUS_SUCCESS ? CFR_STATUS_INVALID_ARGUMENT
                                                : status;
        policies[key].present = true;
        policies[key].node = node;
    }
    return CFR_STATUS_SUCCESS;
}

static Status profile_value(CompactGraph *graph, const CompactPolicy *policies,
                            size_t index, Utility *value_out) {
    if (graph == NULL || policies == NULL || value_out == NULL ||
        index >= graph->node_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    CompactNode *node = &graph->nodes[index];
    if (node->profile_ready) {
        *value_out = node->profile_value;
        return CFR_STATUS_SUCCESS;
    }

    Utility result = 0.0;
    if (node->kind == COMPACT_TERMINAL) {
        result = node->terminal_utility;
    } else if (node->kind == COMPACT_CHANCE) {
        for (size_t action = 0; action < node->edge_count; action++) {
            const CompactEdge *edge =
                &graph->edges[node->edge_offset + action];
            Utility child = 0.0;
            Status status = profile_value(graph, policies, edge->child, &child);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            result += edge->probability * child;
        }
    } else if (node->kind == COMPACT_PLAYER) {
        const InfoSetKey key = node->information_key;
        if (key < 0 || key >= BLACKJACK_INFOSET_COUNT ||
            !policies[key].present || node->edge_count != 2)
            return CFR_STATUS_INVALID_ARGUMENT;
        for (size_t action = 0; action < 2; action++) {
            Utility child = 0.0;
            Status status = profile_value(
                graph, policies, graph->edges[node->edge_offset + action].child,
                &child);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            result += policies[key].learned[action] * child;
        }
    } else {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (!isfinite(result))
        return CFR_STATUS_NUMERIC_ERROR;
    node->profile_value = result;
    node->profile_ready = true;
    *value_out = result;
    return CFR_STATUS_SUCCESS;
}

static Status best_response_value(CompactGraph *graph,
                                  const CompactPolicy *policies, size_t index,
                                  Utility *value_out) {
    if (graph == NULL || policies == NULL || value_out == NULL ||
        index >= graph->node_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    CompactNode *node = &graph->nodes[index];
    if (node->best_response_ready) {
        *value_out = node->best_response_value;
        return CFR_STATUS_SUCCESS;
    }

    Utility result = 0.0;
    if (node->kind == COMPACT_TERMINAL) {
        result = node->terminal_utility;
    } else if (node->kind == COMPACT_CHANCE) {
        for (size_t action = 0; action < node->edge_count; action++) {
            const CompactEdge *edge =
                &graph->edges[node->edge_offset + action];
            Utility child = 0.0;
            Status status =
                best_response_value(graph, policies, edge->child, &child);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            result += edge->probability * child;
        }
    } else if (node->kind == COMPACT_PLAYER) {
        const InfoSetKey key = node->information_key;
        if (key < 0 || key >= BLACKJACK_INFOSET_COUNT ||
            !policies[key].selected || node->edge_count != 2 ||
            policies[key].selected_action >= 2)
            return CFR_STATUS_INVALID_ARGUMENT;
        const size_t child =
            graph->edges[node->edge_offset +
                         policies[key].selected_action]
                .child;
        Status status = best_response_value(graph, policies, child, &result);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    } else {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (!isfinite(result))
        return CFR_STATUS_NUMERIC_ERROR;
    node->best_response_value = result;
    node->best_response_ready = true;
    *value_out = result;
    return CFR_STATUS_SUCCESS;
}

static int information_low_total(size_t key) {
    const size_t block = key / 22;
    const int total = (int)(key % 22);
    const bool soft = (block % 2) != 0;
    return soft ? total - 10 : total;
}

static Status solve_best_response(CompactGraph *graph,
                                  CompactPolicy *policies) {
    if (graph == NULL || policies == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (int low_total = 21; low_total >= 0; low_total--) {
        for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
            CompactPolicy *policy = &policies[key];
            if (!policy->present || information_low_total(key) != low_total)
                continue;

            size_t node_index = graph->group_heads[key];
            while (node_index != EMPTY_INDEX) {
                if (node_index >= graph->node_count)
                    return CFR_STATUS_INVALID_ARGUMENT;
                const CompactNode *node = &graph->nodes[node_index];
                if (node->kind != COMPACT_PLAYER ||
                    node->information_key != (InfoSetKey)key ||
                    node->edge_count != 2 ||
                    !isfinite(node->counterfactual_reach) ||
                    node->counterfactual_reach < 0.0)
                    return CFR_STATUS_INVALID_ARGUMENT;
                policy->counterfactual_reach += node->counterfactual_reach;
                for (size_t action = 0; action < 2; action++) {
                    Utility child_value = 0.0;
                    Status status = best_response_value(
                        graph, policies,
                        graph->edges[node->edge_offset + action].child,
                        &child_value);
                    if (status != CFR_STATUS_SUCCESS)
                        return status;
                    policy->action_total[action] +=
                        node->counterfactual_reach * child_value;
                }
                node_index = node->next_in_group;
            }
            if (!isfinite(policy->counterfactual_reach) ||
                policy->counterfactual_reach <= 0.0 ||
                !isfinite(policy->action_total[0]) ||
                !isfinite(policy->action_total[1]))
                return CFR_STATUS_NUMERIC_ERROR;
            policy->selected_action =
                policy->action_total[0] >= policy->action_total[1] ? 0 : 1;
            policy->selected = true;
        }
    }
    return CFR_STATUS_SUCCESS;
}

static Status iteration_value(CompactGraph *graph,
                              const CompactPolicy *policies, size_t index,
                              Utility *value_out) {
    if (graph == NULL || policies == NULL || value_out == NULL ||
        index >= graph->node_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    CompactNode *node = &graph->nodes[index];
    if (node->iteration_ready) {
        *value_out = node->iteration_value;
        return CFR_STATUS_SUCCESS;
    }

    Utility result = 0.0;
    if (node->kind == COMPACT_TERMINAL) {
        result = node->terminal_utility;
    } else if (node->kind == COMPACT_CHANCE) {
        for (size_t action = 0; action < node->edge_count; action++) {
            const CompactEdge *edge =
                &graph->edges[node->edge_offset + action];
            Utility child = 0.0;
            Status status =
                iteration_value(graph, policies, edge->child, &child);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            result += edge->probability * child;
        }
    } else if (node->kind == COMPACT_PLAYER) {
        const InfoSetKey key = node->information_key;
        if (key < 0 || key >= BLACKJACK_INFOSET_COUNT ||
            !policies[key].present || node->edge_count != 2)
            return CFR_STATUS_INVALID_ARGUMENT;
        for (size_t action = 0; action < 2; action++) {
            Utility child = 0.0;
            Status status = iteration_value(
                graph, policies, graph->edges[node->edge_offset + action].child,
                &child);
            if (status != CFR_STATUS_SUCCESS)
                return status;
            result += policies[key].current[action] * child;
        }
    } else {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (!isfinite(result))
        return CFR_STATUS_NUMERIC_ERROR;
    node->iteration_value = result;
    node->iteration_ready = true;
    *value_out = result;
    return CFR_STATUS_SUCCESS;
}

static Status prepare_iteration(CompactGraph *graph,
                                CompactPolicy *policies, size_t root_index) {
    if (graph == NULL || policies == NULL || root_index >= graph->node_count)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        if (!policies[key].present)
            continue;
        Status status = cfr_info_node_current_strategy(
            policies[key].node, policies[key].current, 2);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    for (size_t index = 0; index < graph->node_count; index++) {
        graph->nodes[index].own_reach = 0.0;
        graph->nodes[index].iteration_ready = false;
    }
    graph->nodes[root_index].own_reach = 1.0;

    for (size_t depth = 0; depth <= graph->maximum_depth; depth++) {
        size_t index = graph->depth_heads[depth];
        while (index != EMPTY_INDEX) {
            if (index >= graph->node_count)
                return CFR_STATUS_INVALID_ARGUMENT;
            CompactNode *node = &graph->nodes[index];
            if (!isfinite(node->own_reach) || node->own_reach < 0.0)
                return CFR_STATUS_NUMERIC_ERROR;
            for (size_t action = 0; action < node->edge_count; action++) {
                const CompactEdge *edge =
                    &graph->edges[node->edge_offset + action];
                Probability weight = 1.0;
                if (node->kind == COMPACT_PLAYER) {
                    const InfoSetKey key = node->information_key;
                    if (key < 0 || key >= BLACKJACK_INFOSET_COUNT ||
                        !policies[key].present || action >= 2)
                        return CFR_STATUS_INVALID_ARGUMENT;
                    weight = policies[key].current[action];
                }
                const double addition = node->own_reach * weight;
                const double candidate =
                    graph->nodes[edge->child].own_reach + addition;
                if (!isfinite(addition) || !isfinite(candidate))
                    return CFR_STATUS_NUMERIC_ERROR;
                graph->nodes[edge->child].own_reach = candidate;
            }
            index = node->next_at_depth;
        }
    }
    return CFR_STATUS_SUCCESS;
}

static Status compact_cfr_iteration(CompactGraph *graph,
                                    CompactPolicy *policies,
                                    size_t root_index,
                                    size_t iteration_number,
                                    TrainerVariant variant) {
    if (graph == NULL || policies == NULL ||
        root_index >= graph->node_count || iteration_number == 0 ||
        (variant != CFR_TRAINER_VARIANT_CFR &&
         variant != CFR_TRAINER_VARIANT_CFR_PLUS))
        return CFR_STATUS_INVALID_ARGUMENT;
    Status status = prepare_iteration(graph, policies, root_index);
    if (status != CFR_STATUS_SUCCESS)
        return status;

    Utility root_value = 0.0;
    status = iteration_value(graph, policies, root_index, &root_value);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    (void)root_value;

    Utility regret_delta[BLACKJACK_INFOSET_COUNT][2] = {{0.0}};
    double strategy_delta[BLACKJACK_INFOSET_COUNT][2] = {{0.0}};
    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        if (!policies[key].present)
            continue;
        size_t node_index = graph->group_heads[key];
        while (node_index != EMPTY_INDEX) {
            if (node_index >= graph->node_count)
                return CFR_STATUS_INVALID_ARGUMENT;
            CompactNode *node = &graph->nodes[node_index];
            if (!node->iteration_ready || node->edge_count != 2 ||
                node->information_key != (InfoSetKey)key)
                return CFR_STATUS_INVALID_ARGUMENT;
            for (size_t action = 0; action < 2; action++) {
                Utility child_value = 0.0;
                status = iteration_value(
                    graph, policies,
                    graph->edges[node->edge_offset + action].child,
                    &child_value);
                if (status != CFR_STATUS_SUCCESS)
                    return status;
                regret_delta[key][action] +=
                    node->counterfactual_reach *
                    (child_value - node->iteration_value);
                strategy_delta[key][action] +=
                    node->own_reach * policies[key].current[action] *
                    (variant == CFR_TRAINER_VARIANT_CFR_PLUS
                         ? (double)iteration_number
                         : 1.0);
                if (!isfinite(regret_delta[key][action]) ||
                    !isfinite(strategy_delta[key][action]))
                    return CFR_STATUS_NUMERIC_ERROR;
            }
            node_index = node->next_in_group;
        }
    }

    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        if (!policies[key].present)
            continue;
        status = cfr_info_node_check_deltas(
            policies[key].node, regret_delta[key], strategy_delta[key], 2);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        if (!policies[key].present)
            continue;
        status = cfr_info_node_apply_deltas(
            policies[key].node, regret_delta[key], strategy_delta[key], 2);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (variant == CFR_TRAINER_VARIANT_CFR_PLUS) {
            for (size_t action = 0; action < 2; action++) {
                if (policies[key].node->regret_sums[action] < 0.0)
                    policies[key].node->regret_sums[action] = 0.0;
            }
        }
    }
    return CFR_STATUS_SUCCESS;
}

static Status compact_train(CompactGraph *graph, CompactPolicy *policies,
                            size_t root_index, Trainer *trainer,
                            size_t amount) {
    if (graph == NULL || policies == NULL || trainer == NULL || amount == 0 ||
        (trainer->variant != CFR_TRAINER_VARIANT_CFR &&
         trainer->variant != CFR_TRAINER_VARIANT_CFR_PLUS))
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t completed = 0; completed < amount; completed++) {
        if (trainer->training_iterations == SIZE_MAX)
            return CFR_STATUS_NUMERIC_ERROR;
        const size_t iteration = trainer->training_iterations + 1;
        Status status = compact_cfr_iteration(
            graph, policies, root_index, iteration, trainer->variant);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        trainer->training_iterations++;
        if (trainer->stats.iterations != SIZE_MAX)
            trainer->stats.iterations++;
        if (trainer->stats.traversals != SIZE_MAX)
            trainer->stats.traversals++;
        if (trainer->stats.visited_nodes >
            SIZE_MAX - graph->raw_visit_count)
            trainer->stats.visited_nodes = SIZE_MAX;
        else
            trainer->stats.visited_nodes += graph->raw_visit_count;
        (void)fprintf(stderr,
                      "compact-report iterations=%zu raw_visited_nodes=%zu\n",
                      trainer->training_iterations,
                      trainer->stats.visited_nodes);
        (void)fflush(stderr);
    }
    return CFR_STATUS_SUCCESS;
}

static const char *status_name(Status status) {
    switch (status) {
    case CFR_STATUS_SUCCESS:
        return "success";
    case CFR_STATUS_INVALID_ARGUMENT:
        return "invalid-argument";
    case CFR_STATUS_OUT_OF_MEMORY:
        return "out-of-memory";
    case CFR_STATUS_ILLEGAL_ACTION:
        return "illegal-action";
    case CFR_STATUS_BUFFER_TOO_SMALL:
        return "buffer-too-small";
    case CFR_STATUS_NUMERIC_ERROR:
        return "numeric-error";
    case CFR_STATUS_IO_ERROR:
        return "io-error";
    case CFR_STATUS_FORMAT_ERROR:
        return "format-error";
    default:
        return "unknown";
    }
}

static int fail_status(const char *operation, Status status) {
    (void)fprintf(stderr, "error: %s: %s (%d)\n", operation,
                  status_name(status), (int)status);
    return EXIT_FAILURE;
}

static bool parse_positive_size(const char *text, size_t *value_out) {
    if (text == NULL || value_out == NULL || *text == '\0' || *text == '-')
        return false;
    char *end = NULL;
    errno = 0;
    const uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > SIZE_MAX)
        return false;
    *value_out = (size_t)parsed;
    return true;
}

static Status write_checkpoint(const char *path, const Trainer *trainer) {
    if (path == NULL || trainer == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    FILE *stream = fopen(path, "wbx");
    if (stream == NULL)
        return CFR_STATUS_IO_ERROR;
    Status status = cfr_checkpoint_write(stream, trainer);
    if (fclose(stream) != 0 && status == CFR_STATUS_SUCCESS)
        status = CFR_STATUS_IO_ERROR;
    return status;
}

int main(int argc, char **argv) {
    if (argc != 2 && argc != 4 && argc != 5 && argc != 7) {
        (void)fprintf(stderr,
                      "usage: %s CHECKPOINT [PLAYER_CARD DEALER_UP_CARD "
                      "PLAYER_CARD]\n"
                      "       %s CHECKPOINT TRAIN_ITERATIONS "
                      "OUTPUT_CHECKPOINT [PLAYER_CARD DEALER_UP_CARD "
                      "PLAYER_CARD]\n",
                      argc > 0 ? argv[0] : "blackjack-compact-eval",
                      argc > 0 ? argv[0] : "blackjack-compact-eval");
        return EXIT_FAILURE;
    }

    const Game *game = cfr_blackjack_descriptor();
    BlackjackState state = {0};
    InfoStore store = {0};
    Trainer trainer = {0};
    CompactGraph graph = {0};
    CompactPolicy policies[BLACKJACK_INFOSET_COUNT] = {0};
    FILE *checkpoint = NULL;
    bool store_initialized = false;
    int result = EXIT_FAILURE;

    Status status = cfr_blackjack_state_init(&state);
    if (status != CFR_STATUS_SUCCESS)
        return fail_status("initialize blackjack", status);
    const int first_card_argument = argc == 5 ? 2 : 4;
    for (int argument = first_card_argument;
         (argc == 5 || argc == 7) && argument < argc; argument++) {
        char *end = NULL;
        errno = 0;
        const long rank = strtol(argv[argument], &end, 10);
        if (errno != 0 || end == argv[argument] || *end != '\0' || rank < 1 ||
            rank > 10) {
            (void)fprintf(stderr, "error: invalid card rank: %s\n",
                          argv[argument]);
            return EXIT_FAILURE;
        }
        status = cfr_game_apply_action(
            game, cfr_blackjack_state_as_game_state(&state), (Action)rank);
        if (status != CFR_STATUS_SUCCESS)
            return fail_status("apply visible deal", status);
    }

    checkpoint = fopen(argv[1], "rb");
    if (checkpoint == NULL) {
        (void)fprintf(stderr, "error: open %s: %s\n", argv[1],
                      strerror(errno));
        return EXIT_FAILURE;
    }
    status = cfr_checkpoint_read(
        checkpoint, game, cfr_blackjack_state_as_game_state(&state), &store,
        &trainer);
    if (fclose(checkpoint) != 0 && status == CFR_STATUS_SUCCESS)
        status = CFR_STATUS_IO_ERROR;
    checkpoint = NULL;
    if (status != CFR_STATUS_SUCCESS)
        return fail_status("load checkpoint", status);
    store_initialized = true;

    status = graph_init(&graph, game);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("initialize compact graph", status);
        goto cleanup;
    }
    size_t root_index = EMPTY_INDEX;
    status = graph_build_node(&graph, &state, &root_index);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("build compact graph", status);
        goto cleanup;
    }
    status = graph_counterfactual_reach(&graph, root_index);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("calculate counterfactual reach", status);
        goto cleanup;
    }
    status = policy_load(&store, &graph, policies);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("load average policy", status);
        goto cleanup;
    }
    if (argc == 4 || argc == 7) {
        size_t amount = 0;
        if (!parse_positive_size(argv[2], &amount)) {
            (void)fprintf(stderr, "error: invalid training amount: %s\n",
                          argv[2]);
            goto cleanup;
        }
        status = compact_train(&graph, policies, root_index, &trainer, amount);
        if (status != CFR_STATUS_SUCCESS) {
            result = fail_status("run compact CFR+ training", status);
            goto cleanup;
        }
        status = write_checkpoint(argv[3], &trainer);
        if (status != CFR_STATUS_SUCCESS) {
            result = fail_status("write compact checkpoint", status);
            goto cleanup;
        }
        status = policy_load(&store, &graph, policies);
        if (status != CFR_STATUS_SUCCESS) {
            result = fail_status("refresh average policy", status);
            goto cleanup;
        }
    }

    Utility learned_value = 0.0;
    status = profile_value(&graph, policies, root_index, &learned_value);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("evaluate average policy", status);
        goto cleanup;
    }
    status = solve_best_response(&graph, policies);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("solve compact best response", status);
        goto cleanup;
    }
    Utility best_value = 0.0;
    status = best_response_value(&graph, policies, root_index, &best_value);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("evaluate best response", status);
        goto cleanup;
    }

    const Utility improvement = best_value - learned_value;
    if (!isfinite(improvement) || improvement < -1e-10) {
        result = fail_status("validate improvement", CFR_STATUS_NUMERIC_ERROR);
        goto cleanup;
    }
    (void)printf("graph nodes=%zu edges=%zu maximum_depth=%zu "
                 "raw_visited_nodes=%zu\n",
                 graph.node_count, graph.edge_count, graph.maximum_depth,
                 graph.raw_visit_count);
    (void)printf("evaluation training_iterations=%zu "
                 "average_value_player_0=%.17g "
                 "best_response_value_player_0=%.17g "
                 "improvement_player_0=%.17g exploitability=%.17g\n",
                 trainer.training_iterations, learned_value, best_value,
                 improvement < 0.0 ? 0.0 : improvement,
                 improvement < 0.0 ? 0.0 : improvement / 2.0);

    size_t disagreements = 0;
    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        const CompactPolicy *policy = &policies[key];
        if (!policy->present)
            continue;
        const size_t block = key / 22;
        const int total = (int)(key % 22);
        const bool soft = (block % 2) != 0;
        const size_t dealer = block / 2 + 1;
        const size_t learned_action =
            policy->learned[0] >= policy->learned[1] ? 0 : 1;
        const Utility hit_value =
            policy->action_total[0] / policy->counterfactual_reach;
        const Utility stand_value =
            policy->action_total[1] / policy->counterfactual_reach;
        if (learned_action != policy->selected_action)
            disagreements++;
        (void)printf(
            "policy key=%zu dealer=%zu hand=%s total=%d learned_hit=%.17g "
            "learned_stand=%.17g best=%s hit_value=%.17g "
            "stand_value=%.17g action_gap=%.17g argmax_match=%s\n",
            key, dealer, soft ? "soft" : "hard", total,
            policy->learned[0], policy->learned[1],
            policy->selected_action == 0 ? "hit" : "stand", hit_value,
            stand_value, fabs(hit_value - stand_value),
            learned_action == policy->selected_action ? "yes" : "no");
    }
    (void)printf("comparison argmax_disagreements=%zu\n", disagreements);
    result = EXIT_SUCCESS;

cleanup:
    graph_destroy(&graph);
    if (store_initialized) {
        status = cfr_info_store_destroy(&store);
        if (status != CFR_STATUS_SUCCESS)
            result = fail_status("destroy information store", status);
    }
    return result;
}

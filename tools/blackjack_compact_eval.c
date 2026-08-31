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

#define BLACKJACK_INFOSET_COUNT (10 * 2 * 22 * 3)
#define COMPACT_MAX_PLAYER_ACTIONS 4
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
    uint8_t total;
    uint8_t card_count;
    uint8_t is_soft;
    uint8_t can_split;
    uint8_t stake_multiplier;
    uint8_t from_split;
} CompactHand;

typedef struct {
    uint8_t phase;
    CompactHand player_hands[CFR_BLACKJACK_MAX_PLAYER_HANDS];
    uint8_t player_hand_count;
    uint8_t active_player_hand;
    CompactHand dealer_hand;
    uint8_t dealer_up_card;
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
    size_t depth;
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
    size_t *topological_order;
    size_t maximum_depth;
    size_t raw_visit_count;
    bool raw_visit_count_saturated;
    const Game *game;
    const GameOperations *operations;
} CompactGraph;

typedef struct {
    bool present;
    bool selected;
    size_t selected_action;
    size_t basic_action;
    size_t action_count;
    InfoNode *node;
    Probability current[COMPACT_MAX_PLAYER_ACTIONS];
    Probability learned[COMPACT_MAX_PLAYER_ACTIONS];
    Utility action_total[COMPACT_MAX_PLAYER_ACTIONS];
    double counterfactual_reach;
} CompactPolicy;

enum {
    COMPACT_ACTION_HIT = 0,
    COMPACT_ACTION_STAND = 1,
    COMPACT_ACTION_DOUBLE = 2,
    COMPACT_ACTION_SPLIT = 3
};

static void graph_destroy(CompactGraph *graph) {
    if (graph == NULL)
        return;
    free(graph->topological_order);
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
    *graph = temporary;
    return CFR_STATUS_SUCCESS;
}

static Status compact_hand_from_blackjack(const BlackjackHand *hand,
                                          CompactHand *compact) {
    if (hand == NULL || compact == NULL || hand->total < 0 ||
        hand->total > UINT8_MAX || hand->card_count > UINT8_MAX ||
        hand->stake_multiplier > UINT8_MAX)
        return CFR_STATUS_INVALID_ARGUMENT;

    *compact = (CompactHand){
        .total = (uint8_t)hand->total,
        .card_count = (uint8_t)hand->card_count,
        .is_soft = hand->is_soft ? 1 : 0,
        .can_split = hand->can_split ? 1 : 0,
        .stake_multiplier = (uint8_t)hand->stake_multiplier,
        .from_split = hand->from_split ? 1 : 0,
    };
    return CFR_STATUS_SUCCESS;
}

static void compact_normalize_finished_hand(CompactHand *hand) {
    const bool natural = hand->card_count == 2 && hand->total == 21 &&
                         hand->from_split == 0;

    hand->card_count = natural ? 2 : 0;
    if (hand->total > 21)
        hand->total = 22;
    else if (hand->total < 17)
        hand->total = 16;
    hand->is_soft = 0;
    hand->can_split = 0;
    hand->from_split = 0;
}

static int compact_hand_compare(const CompactHand *left,
                                const CompactHand *right) {
    const uint8_t left_fields[] = {
        left->total, left->card_count, left->is_soft, left->can_split,
        left->stake_multiplier, left->from_split};
    const uint8_t right_fields[] = {
        right->total, right->card_count, right->is_soft, right->can_split,
        right->stake_multiplier, right->from_split};

    for (size_t index = 0; index < sizeof(left_fields); index++) {
        if (left_fields[index] < right_fields[index])
            return -1;
        if (left_fields[index] > right_fields[index])
            return 1;
    }
    return 0;
}

static void compact_sort_hands(CompactHand *hands, size_t count) {
    for (size_t index = 1; index < count; index++) {
        CompactHand hand = hands[index];
        size_t position = index;
        while (position > 0 &&
               compact_hand_compare(&hand, &hands[position - 1]) < 0) {
            hands[position] = hands[position - 1];
            position--;
        }
        hands[position] = hand;
    }
}

static bool phase_has_live_player_hands(BlackjackPhase phase) {
    switch (phase) {
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_FIRST:
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_UP_CARD:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_SECOND:
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD:
    case CFR_BLACKJACK_PHASE_PLAYER_TURN:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_HIT:
    case CFR_BLACKJACK_PHASE_DEAL_PLAYER_DOUBLE:
    case CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND:
        return true;
    case CFR_BLACKJACK_PHASE_DEAL_DEALER_HIT:
    case CFR_BLACKJACK_PHASE_TERMINAL:
    default:
        return false;
    }
}

static bool compact_active_hand_can_split(const BlackjackState *state) {
    const BlackjackHand *hand =
        &state->player_hands[state->active_player_hand];

    return state->phase == CFR_BLACKJACK_PHASE_PLAYER_TURN &&
           state->player_hand_count < CFR_BLACKJACK_MAX_PLAYER_HANDS &&
           hand->card_count == 2 && hand->stake_multiplier == 1 &&
           hand->can_split && !(hand->from_split && hand->is_soft);
}

static void compact_normalize_live_hand(CompactHand *hand,
                                        bool retain_pair_rank) {
    if (hand->card_count > 2) {
        hand->card_count = 3;
        hand->can_split = 0;
        hand->from_split = 0;
    } else if (hand->card_count == 2 && !retain_pair_rank) {
        hand->can_split = 0;
        hand->from_split = 0;
    }
}

static Status compact_state_from_blackjack(const BlackjackState *state,
                                           CompactState *compact) {
    if (state == NULL || compact == NULL || state->phase < 0 ||
        state->phase > CFR_BLACKJACK_PHASE_DEAL_SPLIT_HAND ||
        state->player_hand_count > CFR_BLACKJACK_MAX_PLAYER_HANDS ||
        state->player_hand_count > UINT8_MAX ||
        state->active_player_hand > UINT8_MAX ||
        state->dealer_up_card < 0 || state->dealer_up_card > UINT8_MAX ||
        state->undo_count > CFR_BLACKJACK_UNDO_HISTORY_CAPACITY)
        return CFR_STATUS_INVALID_ARGUMENT;

    CompactState result = {0};
    result.phase = (uint8_t)state->phase;
    result.player_hand_count = (uint8_t)state->player_hand_count;
    result.active_player_hand = (uint8_t)state->active_player_hand;
    for (size_t index = 0; index < CFR_BLACKJACK_MAX_PLAYER_HANDS; index++) {
        Status status = compact_hand_from_blackjack(
            &state->player_hands[index], &result.player_hands[index]);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (index >= state->player_hand_count)
            continue;
        const bool live = phase_has_live_player_hands(state->phase) &&
                          index >= state->active_player_hand;
        if (!live) {
            compact_normalize_finished_hand(&result.player_hands[index]);
        } else {
            const bool retain_pair_rank =
                index == state->active_player_hand &&
                (compact_active_hand_can_split(state) ||
                 (state->phase ==
                      CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD &&
                  state->player_hands[index].can_split));
            compact_normalize_live_hand(&result.player_hands[index],
                                        retain_pair_rank);
        }
    }
    if (phase_has_live_player_hands(state->phase)) {
        compact_sort_hands(result.player_hands,
                           state->active_player_hand);
    } else {
        compact_sort_hands(result.player_hands, state->player_hand_count);
        result.active_player_hand = 0;
    }
    Status status =
        compact_hand_from_blackjack(&state->dealer_hand, &result.dealer_hand);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    if (state->phase == CFR_BLACKJACK_PHASE_TERMINAL) {
        compact_normalize_finished_hand(&result.dealer_hand);
    } else {
        compact_normalize_live_hand(&result.dealer_hand, false);
        if (state->phase > CFR_BLACKJACK_PHASE_DEAL_DEALER_HOLE_CARD &&
            result.dealer_hand.card_count == 2)
            result.dealer_hand.card_count = 3;
    }
    result.dealer_up_card = (uint8_t)state->dealer_up_card;
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
    };
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
               actor.player == CFR_PLAYER_0 && action_count >= 2 &&
               action_count <= COMPACT_MAX_PLAYER_ACTIONS) {
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
        const Action expected_actions[COMPACT_MAX_PLAYER_ACTIONS] = {
            CFR_BLACKJACK_ACTION_HIT, CFR_BLACKJACK_ACTION_STAND,
            CFR_BLACKJACK_ACTION_DOUBLE_DOWN, CFR_BLACKJACK_ACTION_SPLIT};
        for (size_t action_index = 0; action_index < action_count;
             action_index++) {
            if (actions[action_index] != expected_actions[action_index])
                return CFR_STATUS_INVALID_ARGUMENT;
        }
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

static Status graph_build_topological_order(CompactGraph *graph,
                                            size_t root_index) {
    if (graph == NULL || root_index >= graph->node_count ||
        graph->topological_order != NULL || graph->node_count == 0 ||
        graph->node_count > SIZE_MAX / sizeof(size_t))
        return CFR_STATUS_INVALID_ARGUMENT;

    size_t *indegrees = calloc(graph->node_count, sizeof(*indegrees));
    size_t *queue = malloc(graph->node_count * sizeof(*queue));
    size_t *order = malloc(graph->node_count * sizeof(*order));
    if (indegrees == NULL || queue == NULL || order == NULL) {
        free(order);
        free(queue);
        free(indegrees);
        return CFR_STATUS_OUT_OF_MEMORY;
    }

    Status result = CFR_STATUS_SUCCESS;
    for (size_t index = 0; index < graph->node_count; index++) {
        const CompactNode *node = &graph->nodes[index];
        for (size_t action = 0; action < node->edge_count; action++) {
            const size_t child =
                graph->edges[node->edge_offset + action].child;
            if (child >= graph->node_count || indegrees[child] == SIZE_MAX) {
                result = CFR_STATUS_INVALID_ARGUMENT;
                goto cleanup;
            }
            indegrees[child]++;
        }
    }

    size_t queue_head = 0;
    size_t queue_tail = 0;
    for (size_t index = 0; index < graph->node_count; index++) {
        if (indegrees[index] == 0)
            queue[queue_tail++] = index;
    }
    if (queue_tail != 1 || queue[0] != root_index) {
        result = CFR_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }

    size_t order_count = 0;
    while (queue_head < queue_tail) {
        const size_t index = queue[queue_head++];
        CompactNode *node = &graph->nodes[index];
        order[order_count++] = index;
        if (node->depth > graph->maximum_depth)
            graph->maximum_depth = node->depth;
        for (size_t action = 0; action < node->edge_count; action++) {
            const size_t child =
                graph->edges[node->edge_offset + action].child;
            CompactNode *child_node = &graph->nodes[child];
            if (child_node->depth < node->depth + 1)
                child_node->depth = node->depth + 1;
            if (indegrees[child] == 0) {
                result = CFR_STATUS_INVALID_ARGUMENT;
                goto cleanup;
            }
            indegrees[child]--;
            if (indegrees[child] == 0)
                queue[queue_tail++] = child;
        }
    }
    if (order_count != graph->node_count) {
        result = CFR_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }
    graph->topological_order = order;
    order = NULL;

cleanup:
    free(order);
    free(queue);
    free(indegrees);
    return result;
}

static Status graph_counterfactual_reach(CompactGraph *graph,
                                         size_t root_index) {
    if (graph == NULL || root_index >= graph->node_count ||
        graph->topological_order == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    graph->nodes[root_index].counterfactual_reach = 1.0;
    graph->nodes[root_index].raw_occurrences = 1;
    for (size_t position = 0; position < graph->node_count; position++) {
        const size_t index = graph->topological_order[position];
        if (index >= graph->node_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        CompactNode *node = &graph->nodes[index];
        if (!isfinite(node->counterfactual_reach) ||
            node->counterfactual_reach < 0.0)
            return CFR_STATUS_NUMERIC_ERROR;
        for (size_t action = 0; action < node->edge_count; action++) {
            const CompactEdge *edge =
                &graph->edges[node->edge_offset + action];
            if (edge->child >= graph->node_count)
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
                SIZE_MAX - node->raw_occurrences) {
                graph->nodes[edge->child].raw_occurrences = SIZE_MAX;
                graph->raw_visit_count_saturated = true;
            } else {
                graph->nodes[edge->child].raw_occurrences +=
                    node->raw_occurrences;
            }
        }
        if (graph->raw_visit_count > SIZE_MAX - node->raw_occurrences) {
            graph->raw_visit_count = SIZE_MAX;
            graph->raw_visit_count_saturated = true;
        } else {
            graph->raw_visit_count += node->raw_occurrences;
        }
    }
    return CFR_STATUS_SUCCESS;
}

static Status policy_load(InfoStore *store, const CompactGraph *graph,
                          CompactPolicy policies[BLACKJACK_INFOSET_COUNT],
                          bool create_missing) {
    if (store == NULL || graph == NULL || policies == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        if (graph->group_heads[key] == EMPTY_INDEX)
            continue;
        InfoNode *node = NULL;
        Status status =
            cfr_info_store_find(store, (InfoSetKey)key, &node);
        if (status == CFR_STATUS_NOT_FOUND && create_missing) {
            const size_t group_head = graph->group_heads[key];
            if (group_head >= graph->node_count)
                return CFR_STATUS_INVALID_ARGUMENT;
            status = cfr_info_store_get_or_create(
                store, (InfoSetKey)key,
                graph->nodes[group_head].edge_count, &node);
        }
        if (status != CFR_STATUS_SUCCESS || node == NULL ||
            node->action_count < 2 ||
            node->action_count > COMPACT_MAX_PLAYER_ACTIONS)
            return status == CFR_STATUS_SUCCESS ? CFR_STATUS_INVALID_ARGUMENT
                                                : status;
        size_t required = 0;
        status = cfr_evaluation_average_strategy(
            store, (InfoSetKey)key, policies[key].learned,
            COMPACT_MAX_PLAYER_ACTIONS, &required);
        if (status != CFR_STATUS_SUCCESS || required != node->action_count)
            return status == CFR_STATUS_SUCCESS ? CFR_STATUS_INVALID_ARGUMENT
                                                : status;
        policies[key].present = true;
        policies[key].node = node;
        policies[key].action_count = node->action_count;
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
            !policies[key].present ||
            node->edge_count != policies[key].action_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        for (size_t action = 0; action < policies[key].action_count; action++) {
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

static const char *compact_action_name(size_t action) {
    switch (action) {
    case COMPACT_ACTION_HIT:
        return "hit";
    case COMPACT_ACTION_STAND:
        return "stand";
    case COMPACT_ACTION_DOUBLE:
        return "double";
    case COMPACT_ACTION_SPLIT:
        return "split";
    default:
        return "unknown";
    }
}

static const char *decision_class_name(size_t decision_class) {
    switch (decision_class) {
    case 0:
        return "regular";
    case 1:
        return "double";
    case 2:
        return "split";
    default:
        return "unknown";
    }
}

/*
 * Total-dependent infinite-deck S17 strategy with DAS and no surrender.
 * Conditional doubles and splits use the chart's hit/stand fallbacks.
 */
static size_t basic_hard_action(int total, size_t dealer,
                                bool double_available) {
    if (total >= 17)
        return COMPACT_ACTION_STAND;
    if (total >= 13)
        return dealer >= 2 && dealer <= 6 ? COMPACT_ACTION_STAND
                                          : COMPACT_ACTION_HIT;
    if (total == 12)
        return dealer >= 4 && dealer <= 6 ? COMPACT_ACTION_STAND
                                          : COMPACT_ACTION_HIT;
    if (double_available) {
        if (total == 11 && dealer != 1)
            return COMPACT_ACTION_DOUBLE;
        if (total == 10 && dealer >= 2 && dealer <= 9)
            return COMPACT_ACTION_DOUBLE;
        if (total == 9 && dealer >= 3 && dealer <= 6)
            return COMPACT_ACTION_DOUBLE;
    }
    return COMPACT_ACTION_HIT;
}

static size_t basic_soft_action(int total, size_t dealer,
                                bool double_available) {
    if (total >= 20)
        return COMPACT_ACTION_STAND;
    if (total == 19)
        return COMPACT_ACTION_STAND;
    if (total == 18) {
        if (double_available && dealer >= 3 && dealer <= 6)
            return COMPACT_ACTION_DOUBLE;
        if (dealer >= 2 && dealer <= 8)
            return COMPACT_ACTION_STAND;
        return COMPACT_ACTION_HIT;
    }
    if (double_available) {
        if (total == 17 && dealer >= 3 && dealer <= 6)
            return COMPACT_ACTION_DOUBLE;
        if (total == 16 && dealer >= 4 && dealer <= 6)
            return COMPACT_ACTION_DOUBLE;
        if (total >= 14 && total <= 15 && dealer >= 5 && dealer <= 6)
            return COMPACT_ACTION_DOUBLE;
        if (total == 13 && dealer == 6)
            return COMPACT_ACTION_DOUBLE;
    }
    return COMPACT_ACTION_HIT;
}

static size_t basic_pair_action(int total, bool soft, size_t dealer) {
    if (soft && total == 12)
        return COMPACT_ACTION_SPLIT;
    if (soft)
        return SIZE_MAX;

    switch (total) {
    case 4:
        return dealer >= 2 && dealer <= 7 ? COMPACT_ACTION_SPLIT : SIZE_MAX;
    case 6:
        return dealer >= 2 && dealer <= 7 ? COMPACT_ACTION_SPLIT : SIZE_MAX;
    case 8:
        return dealer >= 5 && dealer <= 6 ? COMPACT_ACTION_SPLIT : SIZE_MAX;
    case 12:
        return dealer >= 2 && dealer <= 6 ? COMPACT_ACTION_SPLIT : SIZE_MAX;
    case 14:
        return dealer >= 2 && dealer <= 7 ? COMPACT_ACTION_SPLIT : SIZE_MAX;
    case 16:
        return COMPACT_ACTION_SPLIT;
    case 18:
        return (dealer >= 2 && dealer <= 6) || dealer == 8 || dealer == 9
                   ? COMPACT_ACTION_SPLIT
                   : COMPACT_ACTION_STAND;
    case 20:
        return COMPACT_ACTION_STAND;
    default:
        return SIZE_MAX;
    }
}

static Status basic_action_for_key(size_t key, size_t action_count,
                                   size_t *action_out) {
    if (key >= BLACKJACK_INFOSET_COUNT || action_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    const size_t block = key / 66;
    const size_t remainder = key % 66;
    const int total = (int)(remainder / 3);
    const size_t decision_class = remainder % 3;
    const bool soft = (block % 2) != 0;
    const size_t dealer = block / 2 + 1;
    if (dealer < 1 || dealer > 10 || action_count != decision_class + 2)
        return CFR_STATUS_INVALID_ARGUMENT;

    size_t action = SIZE_MAX;
    if (decision_class == 2)
        action = basic_pair_action(total, soft, dealer);
    if (action == SIZE_MAX) {
        const bool double_available = decision_class != 0;
        action = soft ? basic_soft_action(total, dealer, double_available)
                      : basic_hard_action(total, dealer, double_available);
    }
    if (action >= action_count)
        return CFR_STATUS_INVALID_ARGUMENT;
    *action_out = action;
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
            !policies[key].selected ||
            node->edge_count != policies[key].action_count ||
            policies[key].selected_action >= policies[key].action_count)
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

static void clear_selected_policy_cache(CompactGraph *graph) {
    for (size_t index = 0; index < graph->node_count; index++)
        graph->nodes[index].best_response_ready = false;
}

static Status evaluate_basic_strategy(CompactGraph *graph,
                                      CompactPolicy *policies,
                                      size_t root_index,
                                      Utility *value_out) {
    if (graph == NULL || policies == NULL || value_out == NULL ||
        root_index >= graph->node_count)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        CompactPolicy *policy = &policies[key];
        if (!policy->present)
            continue;
        Status status = basic_action_for_key(key, policy->action_count,
                                             &policy->basic_action);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        policy->selected_action = policy->basic_action;
        policy->selected = true;
    }
    clear_selected_policy_cache(graph);
    return best_response_value(graph, policies, root_index, value_out);
}

static Status solve_best_response(CompactGraph *graph,
                                  CompactPolicy *policies) {
    if (graph == NULL || policies == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;

    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        CompactPolicy *policy = &policies[key];
        if (!policy->present)
            continue;
        size_t best_action = 0;
        for (size_t action = 1; action < policy->action_count; action++) {
            if (policy->learned[action] > policy->learned[best_action])
                best_action = action;
        }
        policy->selected_action = best_action;
        policy->selected = true;
    }

    for (size_t pass = 0; pass < 128; pass++) {
        clear_selected_policy_cache(graph);
        for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
            CompactPolicy *policy = &policies[key];
            if (!policy->present)
                continue;
            policy->counterfactual_reach = 0.0;
            for (size_t action = 0; action < policy->action_count; action++)
                policy->action_total[action] = 0.0;

            size_t node_index = graph->group_heads[key];
            while (node_index != EMPTY_INDEX) {
                if (node_index >= graph->node_count)
                    return CFR_STATUS_INVALID_ARGUMENT;
                const CompactNode *node = &graph->nodes[node_index];
                if (node->kind != COMPACT_PLAYER ||
                    node->information_key != (InfoSetKey)key ||
                    node->edge_count != policy->action_count ||
                    !isfinite(node->counterfactual_reach) ||
                    node->counterfactual_reach < 0.0)
                    return CFR_STATUS_INVALID_ARGUMENT;
                policy->counterfactual_reach += node->counterfactual_reach;
                for (size_t action = 0; action < policy->action_count;
                     action++) {
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
                policy->counterfactual_reach <= 0.0)
                return CFR_STATUS_NUMERIC_ERROR;
            for (size_t action = 0; action < policy->action_count; action++) {
                if (!isfinite(policy->action_total[action]))
                    return CFR_STATUS_NUMERIC_ERROR;
            }
        }

        size_t changes = 0;
        for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
            CompactPolicy *policy = &policies[key];
            if (!policy->present)
                continue;
            size_t best_action = 0;
            for (size_t action = 1; action < policy->action_count; action++) {
                if (policy->action_total[action] >
                    policy->action_total[best_action])
                    best_action = action;
            }
            if (best_action != policy->selected_action) {
                policy->selected_action = best_action;
                changes++;
            }
        }
        if (changes == 0)
            return CFR_STATUS_SUCCESS;
    }
    return CFR_STATUS_NUMERIC_ERROR;
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
            !policies[key].present ||
            node->edge_count != policies[key].action_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        for (size_t action = 0; action < policies[key].action_count; action++) {
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
            policies[key].node, policies[key].current,
            COMPACT_MAX_PLAYER_ACTIONS);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    for (size_t index = 0; index < graph->node_count; index++) {
        graph->nodes[index].own_reach = 0.0;
        graph->nodes[index].iteration_ready = false;
    }
    graph->nodes[root_index].own_reach = 1.0;

    if (graph->topological_order == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    for (size_t position = 0; position < graph->node_count; position++) {
        const size_t index = graph->topological_order[position];
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
                    !policies[key].present ||
                    action >= policies[key].action_count ||
                    node->edge_count != policies[key].action_count)
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

    Utility regret_delta[BLACKJACK_INFOSET_COUNT]
                        [COMPACT_MAX_PLAYER_ACTIONS] = {{0.0}};
    double strategy_delta[BLACKJACK_INFOSET_COUNT]
                         [COMPACT_MAX_PLAYER_ACTIONS] = {{0.0}};
    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        if (!policies[key].present)
            continue;
        size_t node_index = graph->group_heads[key];
        while (node_index != EMPTY_INDEX) {
            if (node_index >= graph->node_count)
                return CFR_STATUS_INVALID_ARGUMENT;
            CompactNode *node = &graph->nodes[node_index];
            if (!node->iteration_ready ||
                node->edge_count != policies[key].action_count ||
                node->information_key != (InfoSetKey)key)
                return CFR_STATUS_INVALID_ARGUMENT;
            for (size_t action = 0; action < policies[key].action_count;
                 action++) {
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
            policies[key].node, regret_delta[key], strategy_delta[key],
            policies[key].action_count);
        if (status != CFR_STATUS_SUCCESS)
            return status;
    }
    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        if (!policies[key].present)
            continue;
        status = cfr_info_node_apply_deltas(
            policies[key].node, regret_delta[key], strategy_delta[key],
            policies[key].action_count);
        if (status != CFR_STATUS_SUCCESS)
            return status;
        if (variant == CFR_TRAINER_VARIANT_CFR_PLUS) {
            for (size_t action = 0; action < policies[key].action_count;
                 action++) {
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
    const bool fresh = argc > 1 && argv[1] != NULL &&
                       strcmp(argv[1], "--new-cfr-plus") == 0;
    if ((fresh && argc != 4 && argc != 7) ||
        (!fresh && argc != 2 && argc != 4 && argc != 5 && argc != 7)) {
        (void)fprintf(stderr,
                      "usage: %s CHECKPOINT [PLAYER_CARD DEALER_UP_CARD "
                      "PLAYER_CARD]\n"
                      "       %s CHECKPOINT TRAIN_ITERATIONS "
                      "OUTPUT_CHECKPOINT [PLAYER_CARD DEALER_UP_CARD "
                      "PLAYER_CARD]\n"
                      "       %s --new-cfr-plus TRAIN_ITERATIONS "
                      "OUTPUT_CHECKPOINT [PLAYER_CARD DEALER_UP_CARD "
                      "PLAYER_CARD]\n",
                      argc > 0 ? argv[0] : "blackjack-compact-eval",
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

    if (fresh) {
        status = cfr_info_store_init(&store);
        if (status != CFR_STATUS_SUCCESS)
            return fail_status("initialize information store", status);
        store_initialized = true;
        status = cfr_trainer_init_plus(
            &trainer, game, cfr_blackjack_state_as_game_state(&state),
            &store);
        if (status != CFR_STATUS_SUCCESS) {
            result = fail_status("initialize CFR+ trainer", status);
            goto cleanup;
        }
    } else {
        checkpoint = fopen(argv[1], "rb");
        if (checkpoint == NULL) {
            (void)fprintf(stderr, "error: open %s: %s\n", argv[1],
                          strerror(errno));
            return EXIT_FAILURE;
        }
        status = cfr_checkpoint_read(
            checkpoint, game, cfr_blackjack_state_as_game_state(&state),
            &store, &trainer);
        if (fclose(checkpoint) != 0 && status == CFR_STATUS_SUCCESS)
            status = CFR_STATUS_IO_ERROR;
        checkpoint = NULL;
        if (status != CFR_STATUS_SUCCESS)
            return fail_status("load checkpoint", status);
        store_initialized = true;
    }

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
    status = graph_build_topological_order(&graph, root_index);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("order compact graph", status);
        goto cleanup;
    }
    status = graph_counterfactual_reach(&graph, root_index);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("calculate counterfactual reach", status);
        goto cleanup;
    }
    status = policy_load(&store, &graph, policies, fresh);
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
        status = policy_load(&store, &graph, policies, false);
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
    Utility basic_value = 0.0;
    status = evaluate_basic_strategy(&graph, policies, root_index,
                                     &basic_value);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("evaluate basic strategy", status);
        goto cleanup;
    }
    status = solve_best_response(&graph, policies);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("solve candidate deterministic policy", status);
        goto cleanup;
    }
    Utility candidate_value = 0.0;
    status = best_response_value(&graph, policies, root_index,
                                 &candidate_value);
    if (status != CFR_STATUS_SUCCESS) {
        result = fail_status("evaluate candidate deterministic policy",
                             status);
        goto cleanup;
    }

    const Utility candidate_improvement = candidate_value - learned_value;
    const Utility learned_minus_basic = learned_value - basic_value;
    if (!isfinite(candidate_improvement) || !isfinite(learned_minus_basic) ||
        candidate_improvement < -1e-10) {
        result = fail_status("validate policy comparison",
                             CFR_STATUS_NUMERIC_ERROR);
        goto cleanup;
    }
    (void)printf("graph nodes=%zu edges=%zu maximum_depth=%zu "
                 "raw_visited_nodes=%s%zu\n",
                 graph.node_count, graph.edge_count, graph.maximum_depth,
                 graph.raw_visit_count_saturated ? ">=" : "",
                 graph.raw_visit_count);
    (void)printf("evaluation training_iterations=%zu "
                 "average_value_player_0=%.17g "
                 "basic_strategy_value_player_0=%.17g "
                 "learned_minus_basic=%.17g "
                 "candidate_policy_value_player_0=%.17g "
                 "candidate_improvement_proxy=%.17g\n",
                 trainer.training_iterations, learned_value, basic_value,
                 learned_minus_basic, candidate_value,
                 candidate_improvement < 0.0 ? 0.0
                                             : candidate_improvement);

    size_t basic_disagreements = 0;
    size_t candidate_disagreements = 0;
    size_t policy_count = 0;
    for (size_t key = 0; key < BLACKJACK_INFOSET_COUNT; key++) {
        const CompactPolicy *policy = &policies[key];
        if (!policy->present)
            continue;
        const size_t block = key / 66;
        const size_t remainder = key % 66;
        const int total = (int)(remainder / 3);
        const size_t decision_class = remainder % 3;
        const bool soft = (block % 2) != 0;
        const size_t dealer = block / 2 + 1;
        size_t learned_action = 0;
        size_t second_action = 0;
        for (size_t action = 1; action < policy->action_count; action++) {
            if (policy->learned[action] >
                policy->learned[learned_action])
                learned_action = action;
        }
        for (size_t action = 0; action < policy->action_count; action++) {
            if (action == policy->selected_action)
                continue;
            if (second_action == policy->selected_action ||
                policy->action_total[action] >
                    policy->action_total[second_action])
                second_action = action;
        }
        const Utility hit_value = policy->action_total[0] /
                                  policy->counterfactual_reach;
        const Utility stand_value = policy->action_total[1] /
                                    policy->counterfactual_reach;
        const Utility double_value =
            policy->action_count > 2
                ? policy->action_total[2] / policy->counterfactual_reach
                : 0.0;
        const Utility split_value =
            policy->action_count > 3
                ? policy->action_total[3] / policy->counterfactual_reach
                : 0.0;
        const Utility action_gap =
            (policy->action_total[policy->selected_action] -
             policy->action_total[second_action]) /
            policy->counterfactual_reach;
        if (learned_action != policy->basic_action)
            basic_disagreements++;
        if (learned_action != policy->selected_action)
            candidate_disagreements++;
        policy_count++;
        (void)printf(
            "policy key=%zu dealer=%zu hand=%s total=%d class=%s "
            "action_count=%zu learned_hit=%.17g learned_stand=%.17g "
            "learned_double=%.17g learned_split=%.17g learned=%s "
            "basic=%s candidate=%s candidate_hit_value=%.17g "
            "candidate_stand_value=%.17g candidate_double_value=%.17g "
            "candidate_split_value=%.17g action_gap=%.17g "
            "basic_match=%s candidate_match=%s\n",
            key, dealer, soft ? "soft" : "hard", total,
            decision_class_name(decision_class), policy->action_count,
            policy->learned[0], policy->learned[1],
            policy->action_count > 2 ? policy->learned[2] : 0.0,
            policy->action_count > 3 ? policy->learned[3] : 0.0,
            compact_action_name(learned_action),
            compact_action_name(policy->basic_action),
            compact_action_name(policy->selected_action), hit_value,
            stand_value, double_value, split_value, action_gap,
            learned_action == policy->basic_action ? "yes" : "no",
            learned_action == policy->selected_action ? "yes" : "no");
    }
    (void)printf("comparison policies=%zu basic_argmax_disagreements=%zu "
                 "candidate_argmax_disagreements=%zu\n",
                 policy_count, basic_disagreements,
                 candidate_disagreements);
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

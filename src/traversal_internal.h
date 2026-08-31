#ifndef CFR_TRAVERSAL_INTERNAL_H
#define CFR_TRAVERSAL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "cfr/game.h"
#include "cfr/info_node.h"
#include "cfr/traversal.h"

typedef struct {
    const GameOperations *operations;
    const void *context;
    size_t max_legal_actions;
    size_t strategic_player_count;
} CfrTraversalAdapter;

typedef struct CfrFullTraversalWorkspace CfrFullTraversalWorkspace;

bool cfr_traversal_operations_supported(const GameOperations *operations);

Status cfr_traversal_collect_chance_outcomes(
    const CfrTraversalAdapter *adapter, const GameState *state,
    Action *actions, Probability *probabilities, size_t capacity,
    size_t *count_out);

size_t cfr_traversal_hash_node(const InfoNode *node);

void cfr_traversal_initialize_index_table(size_t *table, size_t capacity,
                                          size_t empty_value);

Status cfr_traversal_grow_array(void *array, size_t element_size,
                                size_t *capacity, void **grown_out);

Status cfr_full_traversal_workspace_create(
    size_t max_legal_actions, CfrFullTraversalWorkspace **workspace_out);

void cfr_full_traversal_workspace_destroy(
    CfrFullTraversalWorkspace *workspace);

Status cfr_full_traverse_in_workspace(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    size_t iteration, bool regret_matching_plus, Utility *utility_out,
    TraversalStats *stats_out, CfrFullTraversalWorkspace *workspace);

#endif

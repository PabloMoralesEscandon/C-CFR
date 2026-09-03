#ifndef CFR_MCCFR_INTERNAL_H
#define CFR_MCCFR_INTERNAL_H

#include "cfr/mccfr.h"

typedef struct {
    Action actions[CFR_TRAVERSAL_MAX_ACTIONS];
    Probability probabilities[CFR_TRAVERSAL_MAX_ACTIONS];
    Utility utilities[CFR_TRAVERSAL_MAX_ACTIONS];
} MccfrFrame;

typedef struct {
    InfoNode *node;
    size_t action_count;
    size_t arena_offset;
    size_t table_cell;
} MccfrDeltaEntry;

typedef struct {
    const InfoNode *node;
    size_t sampled_action;
    size_t action_count;
    size_t table_cell;
    Action actions[CFR_TRAVERSAL_MAX_ACTIONS];
    Probability probabilities[CFR_TRAVERSAL_MAX_ACTIONS];
} MccfrStrategySnapshot;

#define CFR_MCCFR_NODE_CACHE_BITS 9
#define CFR_MCCFR_NODE_CACHE_CAPACITY \
    ((size_t)1 << CFR_MCCFR_NODE_CACHE_BITS)

typedef struct {
    InfoSetKey key;
    InfoNode *node;
} MccfrNodeCacheEntry;

typedef struct {
    MccfrFrame *frames;
    size_t frame_capacity;

    size_t *delta_table;
    size_t delta_table_capacity;
    size_t delta_table_used;
    MccfrDeltaEntry *delta_entries;
    size_t delta_entry_count;
    size_t delta_entry_capacity;

    size_t *snapshot_table;
    size_t snapshot_table_capacity;
    size_t snapshot_table_used;
    MccfrStrategySnapshot *snapshots;
    size_t snapshot_count;
    size_t snapshot_capacity;

    double *arena;
    size_t arena_used;
    size_t arena_capacity;

    size_t visits;
    MccfrRng rng;

    /* Traversal-local lookup cache; borrowed nodes remain owned by the store. */
    InfoStore *cached_store;
    MccfrNodeCacheEntry node_cache[CFR_MCCFR_NODE_CACHE_CAPACITY];
} MccfrWorkspace;

Status cfr_mccfr_workspace_init(MccfrWorkspace *workspace,
                                size_t max_legal_actions);

void cfr_mccfr_workspace_destroy(MccfrWorkspace *workspace);

Status cfr_mccfr_external_traverse_in_workspace(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    MccfrRng *rng, Utility *utility_out, TraversalStats *stats_out,
    MccfrWorkspace *workspace);

#endif

#ifndef CFR_MCCFR_SEQUENTIAL_INTERNAL_H
#define CFR_MCCFR_SEQUENTIAL_INTERNAL_H

#include "cfr/mccfr.h"

typedef struct {
    Action actions[CFR_TRAVERSAL_MAX_ACTIONS];
    Probability probabilities[CFR_TRAVERSAL_MAX_ACTIONS];
    Utility utilities[CFR_TRAVERSAL_MAX_ACTIONS];
} MccfrSequentialFrame;

typedef struct {
    InfoNode *node;
    size_t action_count;
    size_t arena_offset;
    size_t table_cell;
} MccfrSequentialDeltaEntry;

typedef struct {
    const InfoNode *node;
    size_t action_index;
    size_t action_count;
    size_t table_cell;
    Action actions[CFR_TRAVERSAL_MAX_ACTIONS];
} MccfrSequentialSampleEntry;

typedef struct {
    MccfrSequentialFrame *frames;
    size_t frame_capacity;

    size_t *delta_table;
    size_t delta_table_capacity;
    size_t delta_table_used;
    MccfrSequentialDeltaEntry *delta_entries;
    size_t delta_entry_count;
    size_t delta_entry_capacity;

    size_t *sample_table;
    size_t sample_table_capacity;
    size_t sample_table_used;
    MccfrSequentialSampleEntry *sample_entries;
    size_t sample_entry_count;
    size_t sample_entry_capacity;

    double *arena;
    size_t arena_used;
    size_t arena_capacity;

    size_t visits;
    MccfrRng rng;
} MccfrSequentialWorkspace;

Status cfr_mccfr_sequential_workspace_init(MccfrSequentialWorkspace *workspace,
                                           size_t max_legal_actions);

void cfr_mccfr_sequential_workspace_destroy(
    MccfrSequentialWorkspace *workspace);

Status cfr_mccfr_sequential_external_traverse_in_workspace(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    MccfrRng *rng, Utility *utility_out, TraversalStats *stats_out,
    MccfrSequentialWorkspace *workspace);

Status
cfr_mccfr_sequential_external_traverse(const Game *game, GameState *state,
                                       InfoStore *store, Player target_player,
                                       MccfrRng *rng, Utility *utility_out);

Status cfr_mccfr_sequential_external_traverse_with_stats(
    const Game *game, GameState *state, InfoStore *store, Player target_player,
    MccfrRng *rng, Utility *utility_out, TraversalStats *stats_out);

#endif

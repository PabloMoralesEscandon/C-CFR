#ifndef CFR_INFO_NODE_INTERNAL_H
#define CFR_INFO_NODE_INTERNAL_H

#include "cfr/info_node.h"

/*
 * Allocates and initializes a node owned by an information store.
 *
 * Nodes wider than the inline capacity store both accumulator arrays after the
 * node in the same allocation. The caller must destroy the returned node and
 * free its allocation.
 */
Status cfr_info_node_create(InfoSetKey key, size_t action_count,
                            InfoNode **node_out);

Status cfr_info_node_owned_size(size_t action_count, size_t *size_out);

Status cfr_info_node_init_owned(void *storage, size_t storage_size,
                                InfoSetKey key, size_t action_count,
                                InfoNode **node_out);

void cfr_info_node_apply_validated_deltas(
    InfoNode *node, const Utility *delta_regret,
    const double *delta_strategy_sum, size_t action_count);

#endif

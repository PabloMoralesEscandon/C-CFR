#ifndef CFR_INFO_NODE_INTERNAL_H
#define CFR_INFO_NODE_INTERNAL_H

#include "cfr/info_node.h"

void cfr_info_node_lock(const InfoNode *node);

void cfr_info_node_unlock(const InfoNode *node);

Status cfr_info_node_check_deltas_locked(
    const InfoNode *node, const Utility *delta_regret,
    const double *delta_strategy_sum, size_t action_count);

void cfr_info_node_apply_validated_deltas(
    InfoNode *node, const Utility *delta_regret,
    const double *delta_strategy_sum, size_t action_count);

#endif

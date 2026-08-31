#ifndef CFR_INFO_NODE_INTERNAL_H
#define CFR_INFO_NODE_INTERNAL_H

#include "cfr/info_node.h"

void cfr_info_node_apply_validated_deltas(
    InfoNode *node, const Utility *delta_regret,
    const double *delta_strategy_sum, size_t action_count);

#endif

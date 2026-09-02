#ifndef CFR_INFO_STORE_INTERNAL_H
#define CFR_INFO_STORE_INTERNAL_H

#include "cfr/info_store.h"

struct CfrInfoStoreEntry {
    InfoNode *node;
    InfoSetKey key;
};

Status cfr_info_store_snapshot_sorted(const InfoStore *info_store,
                                      const InfoNode ***nodes_out,
                                      size_t *count_out);

#endif

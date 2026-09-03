#ifndef CFR_INFO_STORE_INTERNAL_H
#define CFR_INFO_STORE_INTERNAL_H

#include <stdbool.h>

#include "cfr/info_store.h"

struct CfrInfoStoreEntry {
    InfoNode *node;
    InfoSetKey key;
};

Status cfr_info_store_snapshot_sorted(const InfoStore *info_store,
                                      const InfoNode ***nodes_out,
                                      size_t *count_out);

Status cfr_info_store_get_or_create_sequential(
    InfoStore *info_store, InfoSetKey key, size_t action_count,
    InfoNode **node_out);

bool cfr_info_store_is_concurrent(const InfoStore *info_store);

typedef struct CfrInfoArenaBlock {
    struct CfrInfoArenaBlock *next;
    size_t used;
    size_t capacity;
    max_align_t alignment;
    unsigned char data[];
} InfoArenaBlock;

#endif

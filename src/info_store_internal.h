#ifndef CFR_INFO_STORE_INTERNAL_H
#define CFR_INFO_STORE_INTERNAL_H

#include "cfr/info_store.h"

struct CfrInfoStoreEntry {
    InfoNode *node;
    InfoSetKey key;
};

typedef struct CfrInfoArenaBlock {
    struct CfrInfoArenaBlock *next;
    size_t used;
    size_t capacity;
    max_align_t alignment;
    unsigned char data[];
} InfoArenaBlock;

#endif

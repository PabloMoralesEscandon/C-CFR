
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/info_store.h"
#include "info_node_internal.h"
#include "info_store_internal.h"
#include "spin_wait_internal.h"

enum { INITIAL_STORE_CAPACITY = 8 };

#ifndef CFR_INFO_STORE_CONCURRENT_SHARD_BITS
#define CFR_INFO_STORE_CONCURRENT_SHARD_BITS 2
#endif

enum {
    CONCURRENT_SHARD_BITS = CFR_INFO_STORE_CONCURRENT_SHARD_BITS,
    CONCURRENT_SHARD_COUNT = 1 << CONCURRENT_SHARD_BITS,
    CONCURRENT_INITIAL_SHARD_CAPACITY = 8,
    CONCURRENT_CACHE_LINE_SIZE = 64,
    CONCURRENT_NODE_TAG_MASK = 7
};

_Static_assert(CONCURRENT_SHARD_BITS > 0 && CONCURRENT_SHARD_BITS < 16,
               "concurrent shard bits must be between 1 and 15");

#define INFO_ARENA_BLOCK_CAPACITY ((size_t)1048576)
#define CONCURRENT_INITIAL_ARENA_BLOCK_CAPACITY ((size_t)65536)

#define HASH_MULTIPLIER UINT64_C(11400714819323198485)

typedef enum {
    LOCATE_INVALID_ARGUMENT,
    LOCATE_ENTRY_FOUND,
    LOCATE_EMPTY_SLOT_FOUND,
    LOCATE_STORE_FULL
} LocateResult;

typedef struct {
    uintptr_t tagged_node;
} ConcurrentInfoStoreSlot;

_Static_assert(_Alignof(InfoNode) > CONCURRENT_NODE_TAG_MASK,
               "InfoNode alignment must leave three tag bits");

typedef struct CfrConcurrentInfoStoreTable {
    struct CfrConcurrentInfoStoreTable *retired;
    size_t capacity;
    ConcurrentInfoStoreSlot slots[];
} ConcurrentInfoStoreTable;

typedef union {
    struct {
        ConcurrentInfoStoreTable *active;
    } fields;
    unsigned char cache_line[CONCURRENT_CACHE_LINE_SIZE];
} ConcurrentShardReadState;

typedef union {
    struct {
        InfoArenaBlock *node_blocks;
        size_t size;
        size_t collision_count;
        size_t growth_count;
        unsigned char writer_lock;
    } fields;
    unsigned char cache_line[CONCURRENT_CACHE_LINE_SIZE];
} ConcurrentShardWriteState;

typedef struct {
    ConcurrentShardReadState read;
    ConcurrentShardWriteState write;
} ConcurrentInfoStoreShard;

typedef struct {
    size_t inherited_collision_count;
    size_t inherited_growth_count;
    size_t reserve_growth_count;
    void *shard_allocation;
    ConcurrentInfoStoreShard *shards;
} ConcurrentInfoStore;

_Static_assert(sizeof(ConcurrentShardReadState) ==
                   CONCURRENT_CACHE_LINE_SIZE,
               "concurrent shard read state must occupy one cache line");
_Static_assert(sizeof(ConcurrentShardWriteState) ==
                   CONCURRENT_CACHE_LINE_SIZE,
               "concurrent shard write state must occupy one cache line");
_Static_assert(sizeof(ConcurrentInfoStoreShard) ==
                   2 * CONCURRENT_CACHE_LINE_SIZE,
               "concurrent shards must keep read and write state separate");

#define STORE_WRITER_ACTIVE (SIZE_MAX ^ (SIZE_MAX >> 1))
#define STORE_WRITER_PENDING (STORE_WRITER_ACTIVE >> 1)
#define STORE_WRITER_MASK (STORE_WRITER_ACTIVE | STORE_WRITER_PENDING)

static void store_read_lock(const InfoStore *info_store) {
    size_t *synchronization = (size_t *)&info_store->synchronization;
    size_t state = __atomic_load_n(synchronization, __ATOMIC_RELAXED);
    size_t spin_count = 0;

    for (;;) {
        while ((state & STORE_WRITER_MASK) != 0) {
            cfr_spin_wait(&spin_count);
            state = __atomic_load_n(synchronization, __ATOMIC_RELAXED);
        }
        if (__atomic_compare_exchange_n(synchronization, &state, state + 1,
                                        true, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }
        cfr_spin_wait(&spin_count);
    }
}

static void store_read_unlock(const InfoStore *info_store) {
    __atomic_fetch_sub((size_t *)&info_store->synchronization, 1,
                       __ATOMIC_RELEASE);
}

static void store_write_lock(InfoStore *info_store) {
    unsigned char expected_gate = 0;
    size_t spin_count = 0;

    while (!__atomic_compare_exchange_n(&info_store->writer_gate,
                                         &expected_gate, 1, true,
                                         __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED)) {
        expected_gate = 0;
        while (__atomic_load_n(&info_store->writer_gate,
                               __ATOMIC_RELAXED) != 0) {
            cfr_spin_wait(&spin_count);
        }
    }
    __atomic_fetch_or(&info_store->synchronization, STORE_WRITER_PENDING,
                      __ATOMIC_ACQUIRE);
    size_t expected = STORE_WRITER_PENDING;
    while (!__atomic_compare_exchange_n(
        &info_store->synchronization, &expected, STORE_WRITER_ACTIVE, true,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = STORE_WRITER_PENDING;
        cfr_spin_wait(&spin_count);
    }
}

static void store_write_unlock(InfoStore *info_store) {
    __atomic_store_n(&info_store->synchronization, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&info_store->writer_gate, 0, __ATOMIC_RELEASE);
}

static void concurrent_shard_lock(ConcurrentInfoStoreShard *shard) {
    unsigned char expected = 0;
    size_t spin_count = 0;

    while (!__atomic_compare_exchange_n(
        &shard->write.fields.writer_lock, &expected, 1, true,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 0;
        while (__atomic_load_n(&shard->write.fields.writer_lock,
                               __ATOMIC_RELAXED) != 0) {
            cfr_spin_wait(&spin_count);
        }
    }
}

static void concurrent_shard_unlock(ConcurrentInfoStoreShard *shard) {
    __atomic_store_n(&shard->write.fields.writer_lock, 0, __ATOMIC_RELEASE);
}

static size_t saturating_add_size(size_t left, size_t right) {
    return left > SIZE_MAX - right ? SIZE_MAX : left + right;
}

static void add_collisions(InfoStore *info_store, size_t amount) {
    size_t current;
    size_t desired;

    if (amount == 0)
        return;
    current =
        __atomic_load_n(&info_store->collision_count, __ATOMIC_RELAXED);
    do {
        desired = current > SIZE_MAX - amount ? SIZE_MAX : current + amount;
    } while (!__atomic_compare_exchange_n(
        &info_store->collision_count, &current, desired, true,
        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
}

typedef struct {
    InfoArenaBlock *block;
    size_t used;
} ArenaMark;

static ArenaMark arena_mark(const InfoStore *info_store) {
    InfoArenaBlock *block = info_store->node_blocks;

    return (ArenaMark){.block = block,
                       .used = block == NULL ? 0 : block->used};
}

static void arena_rollback(InfoStore *info_store, ArenaMark mark) {
    InfoArenaBlock *block = info_store->node_blocks;

    while (block != mark.block) {
        InfoArenaBlock *next = block->next;

        free(block);
        block = next;
    }
    info_store->node_blocks = block;
    if (block != NULL)
        block->used = mark.used;
}

static Status arena_allocate_node(InfoStore *info_store, InfoSetKey key,
                                  size_t action_count,
                                  InfoNode **node_out) {
    const size_t alignment = _Alignof(InfoNode);
    size_t node_size;
    size_t offset;
    Status status = cfr_info_node_owned_size(action_count, &node_size);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    InfoArenaBlock *block = info_store->node_blocks;
    if (block == NULL) {
        offset = 0;
    } else {
        if (block->used > SIZE_MAX - (alignment - 1))
            return CFR_STATUS_OUT_OF_MEMORY;
        offset = (block->used + alignment - 1) & ~(alignment - 1);
    }
    if (block == NULL || offset > block->capacity ||
        node_size > block->capacity - offset) {
        const size_t capacity =
            node_size > INFO_ARENA_BLOCK_CAPACITY
                ? node_size
                : INFO_ARENA_BLOCK_CAPACITY;

        if (capacity > SIZE_MAX - sizeof(*block))
            return CFR_STATUS_OUT_OF_MEMORY;
        InfoArenaBlock *created = malloc(sizeof(*created) + capacity);
        if (created == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
        created->next = block;
        created->used = 0;
        created->capacity = capacity;
        info_store->node_blocks = created;
        block = created;
        offset = 0;
    }
    status = cfr_info_node_init_owned(block->data + offset,
                                      block->capacity - offset, key,
                                      action_count, node_out);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    block->used = offset + node_size;
    return CFR_STATUS_SUCCESS;
}

static ArenaMark concurrent_arena_mark(
    const ConcurrentInfoStoreShard *shard) {
    InfoArenaBlock *block = shard->write.fields.node_blocks;

    return (ArenaMark){.block = block,
                       .used = block == NULL ? 0 : block->used};
}

static void concurrent_arena_rollback(ConcurrentInfoStoreShard *shard,
                                      ArenaMark mark) {
    InfoArenaBlock *block = shard->write.fields.node_blocks;

    while (block != mark.block) {
        InfoArenaBlock *next = block->next;

        free(block);
        block = next;
    }
    shard->write.fields.node_blocks = block;
    if (block != NULL)
        block->used = mark.used;
}

static size_t concurrent_arena_block_capacity(const InfoArenaBlock *block,
                                              size_t node_size) {
    size_t capacity = CONCURRENT_INITIAL_ARENA_BLOCK_CAPACITY;

    if (block != NULL) {
        capacity = block->capacity;
        if (capacity < INFO_ARENA_BLOCK_CAPACITY) {
            if (capacity > INFO_ARENA_BLOCK_CAPACITY / 2) {
                capacity = INFO_ARENA_BLOCK_CAPACITY;
            } else {
                capacity *= 2;
            }
        }
    }
    return node_size > capacity ? node_size : capacity;
}

static Status concurrent_arena_allocate_node(
    ConcurrentInfoStoreShard *shard, InfoSetKey key, size_t action_count,
    InfoNode **node_out) {
    const size_t alignment = _Alignof(InfoNode);
    size_t node_size;
    size_t offset;
    Status status = cfr_info_node_owned_size(action_count, &node_size);

    if (status != CFR_STATUS_SUCCESS)
        return status;
    InfoArenaBlock *block = shard->write.fields.node_blocks;
    if (block == NULL) {
        offset = 0;
    } else {
        if (block->used > SIZE_MAX - (alignment - 1))
            return CFR_STATUS_OUT_OF_MEMORY;
        offset = (block->used + alignment - 1) & ~(alignment - 1);
    }
    if (block == NULL || offset > block->capacity ||
        node_size > block->capacity - offset) {
        const size_t capacity =
            concurrent_arena_block_capacity(block, node_size);

        if (capacity > SIZE_MAX - sizeof(*block))
            return CFR_STATUS_OUT_OF_MEMORY;
        InfoArenaBlock *created = malloc(sizeof(*created) + capacity);
        if (created == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
        created->next = block;
        created->used = 0;
        created->capacity = capacity;
        shard->write.fields.node_blocks = created;
        block = created;
        offset = 0;
    }
    status = cfr_info_node_init_owned(block->data + offset,
                                      block->capacity - offset, key,
                                      action_count, node_out);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    block->used = offset + node_size;
    return CFR_STATUS_SUCCESS;
}

static uint64_t sortable_node_key(const InfoNode *node) {
    return ((uint64_t)node->key) ^ (UINT64_C(1) << 63);
}

static Status radix_sort_nodes(const InfoNode **nodes, size_t count) {
    enum { RADIX_BITS = 8, RADIX_SIZE = 1 << RADIX_BITS };
    const InfoNode **temporary;
    const InfoNode **source = nodes;
    const InfoNode **destination;

    if (count < 2)
        return CFR_STATUS_SUCCESS;
    if (count > SIZE_MAX / sizeof(*temporary))
        return CFR_STATUS_INVALID_ARGUMENT;
    temporary = malloc(count * sizeof(*temporary));
    if (temporary == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    destination = temporary;
    for (unsigned int shift = 0; shift < 64; shift += RADIX_BITS) {
        size_t positions[RADIX_SIZE] = {0};
        size_t next = 0;

        for (size_t index = 0; index < count; index += 1) {
            const size_t digit =
                (size_t)((sortable_node_key(source[index]) >> shift) &
                         (RADIX_SIZE - 1));

            positions[digit] += 1;
        }
        for (size_t digit = 0; digit < RADIX_SIZE; digit += 1) {
            const size_t amount = positions[digit];

            positions[digit] = next;
            next += amount;
        }
        for (size_t index = 0; index < count; index += 1) {
            const size_t digit =
                (size_t)((sortable_node_key(source[index]) >> shift) &
                         (RADIX_SIZE - 1));

            destination[positions[digit]] = source[index];
            positions[digit] += 1;
        }
        const InfoNode **swap = source;

        source = destination;
        destination = swap;
    }
    free(temporary);
    return CFR_STATUS_SUCCESS;
}

static uint64_t disperse(InfoSetKey key) {
    uint64_t value = (uint64_t)key;
    return value * HASH_MULTIPLIER;
}

static size_t initial_index(InfoSetKey key, size_t capacity) {
    const unsigned int shift = (unsigned int)__builtin_clzll(
        (unsigned long long)(capacity - 1));

    return (size_t)(disperse(key) >> shift);
}

static size_t concurrent_shard_index(uint64_t hash) {
    return (size_t)(hash >> (64 - CONCURRENT_SHARD_BITS));
}

static uint64_t concurrent_local_hash(uint64_t hash) {
    return (hash << CONCURRENT_SHARD_BITS) |
           (hash >> (64 - CONCURRENT_SHARD_BITS));
}

static uintptr_t concurrent_tag_node(InfoNode *node, uint64_t hash) {
    return (uintptr_t)node | (uintptr_t)(hash & CONCURRENT_NODE_TAG_MASK);
}

static InfoNode *concurrent_untag_node(uintptr_t tagged_node) {
    return (InfoNode *)(tagged_node & ~(uintptr_t)CONCURRENT_NODE_TAG_MASK);
}

static size_t concurrent_initial_index(uint64_t hash, size_t capacity) {
    const unsigned int shift = (unsigned int)__builtin_clzll(
        (unsigned long long)(capacity - 1));

    return (size_t)(concurrent_local_hash(hash) >> shift);
}

static ConcurrentInfoStoreTable *concurrent_table_create(size_t capacity) {
    ConcurrentInfoStoreTable *table;

    if (capacity < CONCURRENT_INITIAL_SHARD_CAPACITY ||
        (capacity & (capacity - 1)) != 0 ||
        capacity >
            (SIZE_MAX - sizeof(*table)) / sizeof(*table->slots)) {
        return NULL;
    }
    table = malloc(sizeof(*table) + capacity * sizeof(*table->slots));
    if (table == NULL)
        return NULL;
    table->retired = NULL;
    table->capacity = capacity;
    for (size_t index = 0; index < capacity; index += 1)
        table->slots[index].tagged_node = 0;
    return table;
}

static void concurrent_table_destroy_chain(ConcurrentInfoStoreTable *table) {
    while (table != NULL) {
        ConcurrentInfoStoreTable *retired = table->retired;

        free(table);
        table = retired;
    }
}

static LocateResult concurrent_locate(const ConcurrentInfoStoreTable *table,
                                      uint64_t hash, InfoSetKey key,
                                      size_t *collision_count,
                                      size_t *index_out) {
    if (table == NULL || table->capacity == 0 || index_out == NULL)
        return LOCATE_INVALID_ARGUMENT;
    const size_t mask = table->capacity - 1;
    const size_t first = concurrent_initial_index(hash, table->capacity);

    for (size_t offset = 0; offset < table->capacity; offset += 1) {
        const size_t index = (first + offset) & mask;
        const uintptr_t tagged_node = __atomic_load_n(
            &table->slots[index].tagged_node, __ATOMIC_ACQUIRE);

        if (tagged_node == 0) {
            *index_out = index;
            return LOCATE_EMPTY_SLOT_FOUND;
        }
        if ((tagged_node & CONCURRENT_NODE_TAG_MASK) ==
                (hash & CONCURRENT_NODE_TAG_MASK) &&
            concurrent_untag_node(tagged_node)->key == key) {
            *index_out = index;
            return LOCATE_ENTRY_FOUND;
        }
        if (collision_count != NULL && *collision_count < SIZE_MAX)
            *collision_count += 1;
    }
    return LOCATE_STORE_FULL;
}

static Status concurrent_table_insert_existing(
    ConcurrentInfoStoreTable *table, InfoNode *node) {
    size_t index;
    const uint64_t hash = disperse(node->key);
    const LocateResult result =
        concurrent_locate(table, hash, node->key, NULL, &index);

    if (result != LOCATE_EMPTY_SLOT_FOUND)
        return CFR_STATUS_INVALID_ARGUMENT;
    table->slots[index].tagged_node = concurrent_tag_node(node, hash);
    return CFR_STATUS_SUCCESS;
}

static Status concurrent_table_rehash(
    const ConcurrentInfoStoreTable *old_table, size_t new_capacity,
    ConcurrentInfoStoreTable **table_out) {
    ConcurrentInfoStoreTable *new_table;

    if (old_table == NULL || new_capacity <= old_table->capacity ||
        table_out == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    new_table = concurrent_table_create(new_capacity);
    if (new_table == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    for (size_t index = 0; index < old_table->capacity; index += 1) {
        const uintptr_t tagged_node = __atomic_load_n(
            &old_table->slots[index].tagged_node, __ATOMIC_RELAXED);

        if (tagged_node == 0)
            continue;
        InfoNode *node = concurrent_untag_node(tagged_node);
        const Status status = concurrent_table_insert_existing(new_table, node);

        if (status != CFR_STATUS_SUCCESS) {
            free(new_table);
            return status;
        }
    }
    *table_out = new_table;
    return CFR_STATUS_SUCCESS;
}

static Status concurrent_shard_rebuild(ConcurrentInfoStoreShard *shard,
                                       size_t new_capacity) {
    ConcurrentInfoStoreTable *old_table = shard->read.fields.active;
    ConcurrentInfoStoreTable *new_table = NULL;

    if (old_table == NULL || new_capacity <= old_table->capacity)
        return CFR_STATUS_INVALID_ARGUMENT;
    const Status status =
        concurrent_table_rehash(old_table, new_capacity, &new_table);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    new_table->retired = old_table;
    __atomic_store_n(&shard->read.fields.active, new_table, __ATOMIC_RELEASE);
    shard->write.fields.growth_count += 1;
    return CFR_STATUS_SUCCESS;
}

static LocateResult locate(const InfoStore *info_store, size_t *collision_count,
                           InfoSetKey key, size_t *index_out) {
    if (info_store == NULL || info_store->entries == NULL ||
        info_store->capacity == 0 || index_out == NULL)
        return LOCATE_INVALID_ARGUMENT;
    size_t index = initial_index(key, info_store->capacity);
    for (size_t i = 0; i < info_store->capacity; i++) {
        size_t current_index = (index + i) & (info_store->capacity - 1);
        const InfoStoreEntry *slot = &info_store->entries[current_index];
        if (slot->node == NULL) {
            *index_out = current_index;
            return LOCATE_EMPTY_SLOT_FOUND;
        }
        if (slot->key == key) {
            *index_out = current_index;
            return LOCATE_ENTRY_FOUND;
        }
        if (collision_count != NULL) {
            if (*collision_count < SIZE_MAX)
                *collision_count += 1;
        }
    }
    return LOCATE_STORE_FULL;
}

static Status rebuild(InfoStore *info_store, size_t new_capacity) {
    if (info_store == NULL || info_store->entries == NULL ||
        info_store->capacity == 0 || new_capacity <= info_store->capacity)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (new_capacity > (SIZE_MAX / sizeof(InfoStoreEntry)))
        return CFR_STATUS_OUT_OF_MEMORY;
    InfoStoreEntry *temp_entries =
        malloc(new_capacity * sizeof(InfoStoreEntry));
    if (temp_entries == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    for (size_t i = 0; i < new_capacity; i++)
        temp_entries[i] = (InfoStoreEntry){0};
    InfoStore temp_store = {0};
    temp_store.capacity = new_capacity;
    temp_store.entries = temp_entries;

    for (size_t i = 0; i < info_store->capacity; i++) {
        if (info_store->entries[i].node != NULL) {
            size_t index;
            LocateResult result = locate(&temp_store, NULL,
                                         info_store->entries[i].key, &index);
            if (result != LOCATE_EMPTY_SLOT_FOUND) {
                free(temp_entries);
                return CFR_STATUS_INVALID_ARGUMENT;
            }

            temp_entries[index] = info_store->entries[i];
            ++temp_store.size;
        }
    }
    if (info_store->size != temp_store.size) {
        free(temp_entries);
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    InfoStoreEntry *old_entries = info_store->entries;

    __atomic_store_n(&info_store->entries, temp_entries, __ATOMIC_RELEASE);
    __atomic_store_n(&info_store->capacity, new_capacity, __ATOMIC_RELEASE);
    free(old_entries);
    ++info_store->growth_count;
    return CFR_STATUS_SUCCESS;
}

static Status resize(InfoStore *info_store) {
    if (info_store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (info_store->capacity > (SIZE_MAX / 2))
        return CFR_STATUS_OUT_OF_MEMORY;
    return rebuild(info_store, 2 * info_store->capacity);
}

static ConcurrentInfoStore *concurrent_state_load(
    const InfoStore *info_store) {
    if (info_store == NULL)
        return NULL;
    return __atomic_load_n(&info_store->concurrent_state, __ATOMIC_ACQUIRE);
}

bool cfr_info_store_is_concurrent(const InfoStore *info_store) {
    return concurrent_state_load(info_store) != NULL;
}

static Status concurrent_capacity_for_nodes(size_t minimum_nodes,
                                            size_t minimum_capacity,
                                            size_t *capacity_out) {
    size_t capacity = minimum_capacity;

    if (capacity < CONCURRENT_INITIAL_SHARD_CAPACITY)
        capacity = CONCURRENT_INITIAL_SHARD_CAPACITY;
    while (minimum_nodes > capacity - capacity / 4) {
        if (capacity > SIZE_MAX / 2)
            return CFR_STATUS_OUT_OF_MEMORY;
        capacity *= 2;
    }
    *capacity_out = capacity;
    return CFR_STATUS_SUCCESS;
}

static void concurrent_state_destroy_tables(ConcurrentInfoStore *state) {
    if (state == NULL || state->shards == NULL)
        return;
    for (size_t shard_index = 0;
         shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
        concurrent_table_destroy_chain(
            state->shards[shard_index].read.fields.active);
        state->shards[shard_index].read.fields.active = NULL;
    }
}

Status cfr_info_store_prepare_concurrent(InfoStore *info_store) {
    ConcurrentInfoStore *state;
    size_t shard_sizes[CONCURRENT_SHARD_COUNT] = {0};
    size_t total_capacity = 0;
    Status status = CFR_STATUS_SUCCESS;

    if (info_store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (concurrent_state_load(info_store) != NULL)
        return CFR_STATUS_SUCCESS;

    store_write_lock(info_store);
    if (concurrent_state_load(info_store) != NULL)
        goto unlock;
    if (info_store->entries == NULL || info_store->capacity == 0) {
        status = CFR_STATUS_INVALID_ARGUMENT;
        goto unlock;
    }

    state = malloc(sizeof(*state));
    if (state == NULL) {
        status = CFR_STATUS_OUT_OF_MEMORY;
        goto unlock;
    }
    state->inherited_collision_count = info_store->collision_count;
    state->inherited_growth_count = info_store->growth_count;
    state->reserve_growth_count = 0;
    state->shard_allocation = NULL;
    state->shards = NULL;
    const size_t shard_bytes =
        sizeof(*state->shards) * CONCURRENT_SHARD_COUNT;
    state->shard_allocation =
        malloc(shard_bytes + CONCURRENT_CACHE_LINE_SIZE - 1);
    if (state->shard_allocation == NULL) {
        free(state);
        status = CFR_STATUS_OUT_OF_MEMORY;
        goto unlock;
    }
    const uintptr_t shard_address =
        ((uintptr_t)state->shard_allocation +
         CONCURRENT_CACHE_LINE_SIZE - 1) &
        ~(uintptr_t)(CONCURRENT_CACHE_LINE_SIZE - 1);
    state->shards = (ConcurrentInfoStoreShard *)shard_address;
    for (size_t shard_index = 0;
         shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
        state->shards[shard_index] = (ConcurrentInfoStoreShard){0};
    }

    for (size_t index = 0; index < info_store->capacity; index += 1) {
        InfoNode *node = info_store->entries[index].node;

        if (node == NULL)
            continue;
        const size_t shard_index = concurrent_shard_index(disperse(node->key));

        if (shard_sizes[shard_index] == SIZE_MAX) {
            status = CFR_STATUS_OUT_OF_MEMORY;
            goto fail;
        }
        shard_sizes[shard_index] += 1;
    }

    size_t base_capacity = info_store->capacity / CONCURRENT_SHARD_COUNT;
    if (base_capacity < CONCURRENT_INITIAL_SHARD_CAPACITY)
        base_capacity = CONCURRENT_INITIAL_SHARD_CAPACITY;
    for (size_t shard_index = 0;
         shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
        size_t capacity;

        status = concurrent_capacity_for_nodes(
            shard_sizes[shard_index], base_capacity, &capacity);
        if (status != CFR_STATUS_SUCCESS)
            goto fail;
        ConcurrentInfoStoreTable *table = concurrent_table_create(capacity);
        if (table == NULL) {
            status = CFR_STATUS_OUT_OF_MEMORY;
            goto fail;
        }
        state->shards[shard_index].read.fields.active = table;
        if (total_capacity > SIZE_MAX - capacity) {
            status = CFR_STATUS_OUT_OF_MEMORY;
            goto fail;
        }
        total_capacity += capacity;
    }

    for (size_t index = 0; index < info_store->capacity; index += 1) {
        InfoNode *node = info_store->entries[index].node;

        if (node == NULL)
            continue;
        const size_t shard_index = concurrent_shard_index(disperse(node->key));
        ConcurrentInfoStoreShard *shard = &state->shards[shard_index];

        status = concurrent_table_insert_existing(
            shard->read.fields.active, node);
        if (status != CFR_STATUS_SUCCESS)
            goto fail;
        shard->write.fields.size += 1;
    }
    if (info_store->size != 0) {
        size_t counted = 0;

        for (size_t shard_index = 0;
             shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
            counted += state->shards[shard_index].write.fields.size;
        }
        if (counted != info_store->size) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto fail;
        }
    }

    free(info_store->entries);
    info_store->entries = NULL;
    info_store->capacity = total_capacity;
    __atomic_store_n(&info_store->concurrent_state, state, __ATOMIC_RELEASE);
    goto unlock;

fail:
    concurrent_state_destroy_tables(state);
    free(state->shard_allocation);
    free(state);

unlock:
    store_write_unlock(info_store);
    return status;
}

static Status concurrent_find_node(const ConcurrentInfoStore *state,
                                   InfoSetKey key, InfoNode **node_out) {
    const uint64_t hash = disperse(key);
    const size_t shard_index = concurrent_shard_index(hash);
    const ConcurrentInfoStoreShard *shard = &state->shards[shard_index];
    const ConcurrentInfoStoreTable *table = __atomic_load_n(
        &shard->read.fields.active, __ATOMIC_ACQUIRE);
    size_t index;
    const LocateResult result =
        concurrent_locate(table, hash, key, NULL, &index);

    if (result == LOCATE_ENTRY_FOUND) {
        const uintptr_t tagged_node = __atomic_load_n(
            &table->slots[index].tagged_node, __ATOMIC_ACQUIRE);

        *node_out = concurrent_untag_node(tagged_node);
        return CFR_STATUS_SUCCESS;
    }
    if (result == LOCATE_EMPTY_SLOT_FOUND || result == LOCATE_STORE_FULL) {
        *node_out = NULL;
        return CFR_STATUS_NOT_FOUND;
    }
    return CFR_STATUS_INVALID_ARGUMENT;
}

static Status concurrent_get_or_create(ConcurrentInfoStore *state,
                                       InfoSetKey key, size_t action_count,
                                       InfoNode **node_out) {
    const uint64_t hash = disperse(key);
    const size_t shard_index = concurrent_shard_index(hash);
    ConcurrentInfoStoreShard *shard = &state->shards[shard_index];
    ConcurrentInfoStoreTable *table = __atomic_load_n(
        &shard->read.fields.active, __ATOMIC_ACQUIRE);
    size_t index;
    LocateResult result = concurrent_locate(table, hash, key, NULL, &index);

    if (result == LOCATE_ENTRY_FOUND) {
        const uintptr_t tagged_node = __atomic_load_n(
            &table->slots[index].tagged_node, __ATOMIC_ACQUIRE);
        InfoNode *node = concurrent_untag_node(tagged_node);

        if (node->action_count != action_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        *node_out = node;
        return CFR_STATUS_SUCCESS;
    }
    if (result == LOCATE_INVALID_ARGUMENT)
        return CFR_STATUS_INVALID_ARGUMENT;

    concurrent_shard_lock(shard);
    table = shard->read.fields.active;
    size_t collisions = 0;
    result = concurrent_locate(table, hash, key, &collisions, &index);
    shard->write.fields.collision_count = saturating_add_size(
        shard->write.fields.collision_count, collisions);
    if (result == LOCATE_INVALID_ARGUMENT) {
        concurrent_shard_unlock(shard);
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (result == LOCATE_ENTRY_FOUND) {
        InfoNode *node = concurrent_untag_node(
            table->slots[index].tagged_node);
        const bool count_matches = node->action_count == action_count;

        concurrent_shard_unlock(shard);
        if (!count_matches)
            return CFR_STATUS_INVALID_ARGUMENT;
        *node_out = node;
        return CFR_STATUS_SUCCESS;
    }

    const ArenaMark mark = concurrent_arena_mark(shard);
    InfoNode *node = NULL;
    Status status = concurrent_arena_allocate_node(
        shard, key, action_count, &node);
    if (status != CFR_STATUS_SUCCESS) {
        concurrent_arena_rollback(shard, mark);
        concurrent_shard_unlock(shard);
        return status;
    }
    if (result == LOCATE_STORE_FULL ||
        shard->write.fields.size >=
            table->capacity - table->capacity / 4) {
        if (table->capacity > SIZE_MAX / 2) {
            concurrent_arena_rollback(shard, mark);
            concurrent_shard_unlock(shard);
            return CFR_STATUS_OUT_OF_MEMORY;
        }
        status = concurrent_shard_rebuild(shard, table->capacity * 2);
        if (status != CFR_STATUS_SUCCESS) {
            concurrent_arena_rollback(shard, mark);
            concurrent_shard_unlock(shard);
            return status;
        }
        table = shard->read.fields.active;
        collisions = 0;
        result = concurrent_locate(table, hash, key, &collisions, &index);
        shard->write.fields.collision_count = saturating_add_size(
            shard->write.fields.collision_count, collisions);
        if (result != LOCATE_EMPTY_SLOT_FOUND) {
            concurrent_arena_rollback(shard, mark);
            concurrent_shard_unlock(shard);
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }
    __atomic_store_n(&table->slots[index].tagged_node,
                     concurrent_tag_node(node, hash), __ATOMIC_RELEASE);
    shard->write.fields.size += 1;
    *node_out = node;
    concurrent_shard_unlock(shard);
    return CFR_STATUS_SUCCESS;
}

static void concurrent_lock_all(ConcurrentInfoStore *state) {
    for (size_t shard_index = 0;
         shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
        concurrent_shard_lock(&state->shards[shard_index]);
    }
}

static void concurrent_unlock_all(ConcurrentInfoStore *state) {
    for (size_t shard_index = CONCURRENT_SHARD_COUNT; shard_index > 0;
         shard_index -= 1) {
        concurrent_shard_unlock(&state->shards[shard_index - 1]);
    }
}

static Status concurrent_get_stats(ConcurrentInfoStore *state,
                                   InfoStoreStats *stats_out) {
    InfoStoreStats stats;

    concurrent_lock_all(state);
    stats = (InfoStoreStats){
        .collision_count = state->inherited_collision_count,
        .growth_count = saturating_add_size(
            state->inherited_growth_count, state->reserve_growth_count),
    };
    for (size_t shard_index = 0;
         shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
        const ConcurrentInfoStoreShard *shard = &state->shards[shard_index];
        const ConcurrentInfoStoreTable *table = shard->read.fields.active;

        if (table == NULL) {
            concurrent_unlock_all(state);
            return CFR_STATUS_INVALID_ARGUMENT;
        }
        stats.size = saturating_add_size(stats.size,
                                         shard->write.fields.size);
        stats.capacity = saturating_add_size(stats.capacity, table->capacity);
        stats.collision_count = saturating_add_size(
            stats.collision_count, shard->write.fields.collision_count);
        stats.growth_count = saturating_add_size(
            stats.growth_count, shard->write.fields.growth_count);
    }
    concurrent_unlock_all(state);
    *stats_out = stats;
    return CFR_STATUS_SUCCESS;
}

static Status concurrent_reserve(ConcurrentInfoStore *state,
                                 size_t minimum_node_capacity) {
    ConcurrentInfoStoreTable *replacements[CONCURRENT_SHARD_COUNT] = {0};
    size_t targets[CONCURRENT_SHARD_COUNT] = {0};
    const size_t minimum_per_shard =
        minimum_node_capacity / CONCURRENT_SHARD_COUNT +
        (minimum_node_capacity % CONCURRENT_SHARD_COUNT != 0 ? 1 : 0);
    bool changed = false;
    Status status = CFR_STATUS_SUCCESS;

    concurrent_lock_all(state);
    for (size_t shard_index = 0;
         shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
        ConcurrentInfoStoreShard *shard = &state->shards[shard_index];
        ConcurrentInfoStoreTable *table = shard->read.fields.active;

        if (table == NULL) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        status = concurrent_capacity_for_nodes(
            minimum_per_shard > shard->write.fields.size
                ? minimum_per_shard
                : shard->write.fields.size,
            table->capacity, &targets[shard_index]);
        if (status != CFR_STATUS_SUCCESS)
            goto cleanup;
        if (targets[shard_index] == table->capacity)
            continue;
        status = concurrent_table_rehash(
            table, targets[shard_index], &replacements[shard_index]);
        if (status != CFR_STATUS_SUCCESS)
            goto cleanup;
        changed = true;
    }
    if (changed) {
        for (size_t shard_index = 0;
             shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
            ConcurrentInfoStoreTable *replacement =
                replacements[shard_index];

            if (replacement == NULL)
                continue;
            ConcurrentInfoStoreShard *shard = &state->shards[shard_index];
            ConcurrentInfoStoreTable *old_table = shard->read.fields.active;

            replacement->retired = old_table;
            __atomic_store_n(&shard->read.fields.active, replacement,
                             __ATOMIC_RELEASE);
            replacements[shard_index] = NULL;
        }
        state->reserve_growth_count = saturating_add_size(
            state->reserve_growth_count, 1);
    }

cleanup:
    for (size_t shard_index = 0;
         shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
        free(replacements[shard_index]);
    }
    concurrent_unlock_all(state);
    return status;
}

static Status concurrent_snapshot_sorted(ConcurrentInfoStore *state,
                                         const InfoNode ***nodes_out,
                                         size_t *count_out) {
    const InfoNode **nodes = NULL;
    size_t expected_count = 0;
    size_t count = 0;
    Status status = CFR_STATUS_SUCCESS;

    concurrent_lock_all(state);
    for (size_t shard_index = 0;
         shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
        const size_t shard_size =
            state->shards[shard_index].write.fields.size;

        if (expected_count > SIZE_MAX - shard_size) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto unlock;
        }
        expected_count += shard_size;
    }
    if (expected_count > SIZE_MAX / sizeof(*nodes)) {
        status = CFR_STATUS_INVALID_ARGUMENT;
        goto unlock;
    }
    if (expected_count > 0) {
        nodes = malloc(expected_count * sizeof(*nodes));
        if (nodes == NULL) {
            status = CFR_STATUS_OUT_OF_MEMORY;
            goto unlock;
        }
    }
    for (size_t shard_index = 0;
         shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
        const ConcurrentInfoStoreTable *table =
            state->shards[shard_index].read.fields.active;

        if (table == NULL) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto unlock;
        }
        for (size_t index = 0; index < table->capacity; index += 1) {
            const uintptr_t tagged_node = table->slots[index].tagged_node;

            if (tagged_node == 0)
                continue;
            const InfoNode *node = concurrent_untag_node(tagged_node);
            if (count == expected_count) {
                status = CFR_STATUS_INVALID_ARGUMENT;
                goto unlock;
            }
            nodes[count] = node;
            count += 1;
        }
    }
    if (count != expected_count)
        status = CFR_STATUS_INVALID_ARGUMENT;

unlock:
    concurrent_unlock_all(state);
    if (status != CFR_STATUS_SUCCESS)
        goto cleanup;
    status = radix_sort_nodes(nodes, count);
    if (status != CFR_STATUS_SUCCESS)
        goto cleanup;
    for (size_t index = 1; index < count; index += 1) {
        if (nodes[index - 1]->key == nodes[index]->key) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }
    *nodes_out = nodes;
    *count_out = count;
    return CFR_STATUS_SUCCESS;

cleanup:
    free(nodes);
    return status;
}

Status cfr_info_store_init(InfoStore *info_store) {
    if (info_store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (INITIAL_STORE_CAPACITY > (SIZE_MAX / sizeof(InfoStoreEntry)))
        return CFR_STATUS_INVALID_ARGUMENT;
    InfoStoreEntry *temp =
        malloc(sizeof(InfoStoreEntry) * INITIAL_STORE_CAPACITY);
    if (temp == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    for (size_t i = 0; i < INITIAL_STORE_CAPACITY; i++)
        temp[i] = (InfoStoreEntry){0};
    info_store->capacity = INITIAL_STORE_CAPACITY;
    info_store->growth_count = 0;
    info_store->collision_count = 0;
    info_store->size = 0;
    info_store->entries = temp;
    info_store->node_blocks = NULL;
    info_store->synchronization = 0;
    info_store->writer_gate = 0;
    info_store->concurrent_state = NULL;
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_reserve(InfoStore *info_store,
                              size_t minimum_node_capacity) {
    Status status = CFR_STATUS_SUCCESS;

    if (info_store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    ConcurrentInfoStore *concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL)
        return concurrent_reserve(concurrent, minimum_node_capacity);
    if (__atomic_load_n(&info_store->entries, __ATOMIC_ACQUIRE) == NULL ||
        __atomic_load_n(&info_store->capacity, __ATOMIC_ACQUIRE) == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    store_write_lock(info_store);
    concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL) {
        store_write_unlock(info_store);
        return concurrent_reserve(concurrent, minimum_node_capacity);
    }
    if (info_store->entries == NULL || info_store->capacity == 0) {
        status = CFR_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }
    size_t target = info_store->capacity;
    while (minimum_node_capacity > target - target / 4) {
        if (target > SIZE_MAX / 2) {
            status = CFR_STATUS_OUT_OF_MEMORY;
            goto cleanup;
        }
        target *= 2;
    }
    if (target != info_store->capacity)
        status = rebuild(info_store, target);

cleanup:
    store_write_unlock(info_store);
    return status;
}

Status cfr_info_store_destroy(InfoStore *info_store) {
    if (info_store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    ConcurrentInfoStore *concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL) {
        for (size_t shard_index = 0;
             shard_index < CONCURRENT_SHARD_COUNT; shard_index += 1) {
            InfoArenaBlock *concurrent_block =
                concurrent->shards[shard_index].write.fields.node_blocks;

            while (concurrent_block != NULL) {
                InfoArenaBlock *next = concurrent_block->next;

                free(concurrent_block);
                concurrent_block = next;
            }
        }
        concurrent_state_destroy_tables(concurrent);
        free(concurrent->shard_allocation);
        free(concurrent);
    }
    InfoArenaBlock *block = info_store->node_blocks;
    while (block != NULL) {
        InfoArenaBlock *next = block->next;

        free(block);
        block = next;
    }
    info_store->node_blocks = NULL;
    free(info_store->entries);
    info_store->entries = NULL;
    info_store->capacity = 0;
    info_store->collision_count = 0;
    info_store->growth_count = 0;
    info_store->size = 0;
    info_store->synchronization = 0;
    info_store->writer_gate = 0;
    info_store->concurrent_state = NULL;
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_find(InfoStore *info_store, InfoSetKey key,
                           InfoNode **node_out) {
    if (info_store == NULL || node_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    ConcurrentInfoStore *concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL)
        return concurrent_find_node(concurrent, key, node_out);
    size_t index;
    size_t collisions = 0;

    store_read_lock(info_store);
    concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL) {
        store_read_unlock(info_store);
        return concurrent_find_node(concurrent, key, node_out);
    }
    LocateResult result = locate(info_store, &collisions, key, &index);
    switch (result) {

    case LOCATE_ENTRY_FOUND:
        *node_out = info_store->entries[index].node;
        break;

    case LOCATE_EMPTY_SLOT_FOUND:
    case LOCATE_STORE_FULL:
        *node_out = NULL;
        break;

    case LOCATE_INVALID_ARGUMENT:
        break;
    }
    store_read_unlock(info_store);
    add_collisions(info_store, collisions);

    if (result == LOCATE_ENTRY_FOUND)
        return CFR_STATUS_SUCCESS;
    if (result == LOCATE_EMPTY_SLOT_FOUND || result == LOCATE_STORE_FULL)
        return CFR_STATUS_NOT_FOUND;
    return CFR_STATUS_INVALID_ARGUMENT;
}

Status cfr_info_store_get_or_create(InfoStore *info_store, InfoSetKey key,
                                    size_t action_count, InfoNode **node_out) {
    if (info_store == NULL || node_out == NULL || action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    ConcurrentInfoStore *concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL)
        return concurrent_get_or_create(concurrent, key, action_count,
                                        node_out);
    size_t index;
    size_t collisions = 0;

    store_read_lock(info_store);
    concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL) {
        store_read_unlock(info_store);
        return concurrent_get_or_create(concurrent, key, action_count,
                                        node_out);
    }
    LocateResult result = locate(info_store, &collisions, key, &index);
    if (result == LOCATE_INVALID_ARGUMENT) {
        store_read_unlock(info_store);
        add_collisions(info_store, collisions);
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (result == LOCATE_ENTRY_FOUND) {
        InfoNode *node = info_store->entries[index].node;
        const bool count_matches = node->action_count == action_count;

        store_read_unlock(info_store);
        add_collisions(info_store, collisions);
        if (!count_matches)
            return CFR_STATUS_INVALID_ARGUMENT;
        *node_out = node;
        return CFR_STATUS_SUCCESS;
    }
    store_read_unlock(info_store);
    add_collisions(info_store, collisions);
    collisions = 0;

    store_write_lock(info_store);
    result = locate(info_store, NULL, key, &index);
    if (result == LOCATE_INVALID_ARGUMENT) {
        store_write_unlock(info_store);
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (result == LOCATE_ENTRY_FOUND) {
        InfoNode *node = info_store->entries[index].node;
        const bool count_matches = node->action_count == action_count;

        store_write_unlock(info_store);
        if (!count_matches)
            return CFR_STATUS_INVALID_ARGUMENT;
        *node_out = node;
        return CFR_STATUS_SUCCESS;
    }

    const ArenaMark mark = arena_mark(info_store);
    InfoNode *temp = NULL;
    Status status = arena_allocate_node(info_store, key, action_count, &temp);

    if (status != CFR_STATUS_SUCCESS) {
        arena_rollback(info_store, mark);
        store_write_unlock(info_store);
        return status;
    }
    if (result == LOCATE_STORE_FULL ||
        (info_store->size >= info_store->capacity - info_store->capacity / 4)) {
        Status resize_status = resize(info_store);
        if (resize_status != CFR_STATUS_SUCCESS) {
            arena_rollback(info_store, mark);
            store_write_unlock(info_store);
            return resize_status;
        }
        collisions = 0;
        result = locate(info_store, &collisions, key, &index);
        if (result != LOCATE_EMPTY_SLOT_FOUND) {
            arena_rollback(info_store, mark);
            store_write_unlock(info_store);
            add_collisions(info_store, collisions);
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }
    info_store->entries[index].key = key;
    info_store->entries[index].node = temp;
    ++info_store->size;
    *node_out = info_store->entries[index].node;
    store_write_unlock(info_store);
    add_collisions(info_store, collisions);
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_get_or_create_sequential(
    InfoStore *info_store, InfoSetKey key, size_t action_count,
    InfoNode **node_out) {
    if (info_store == NULL || node_out == NULL || action_count == 0)
        return CFR_STATUS_INVALID_ARGUMENT;
    size_t index;
    LocateResult result =
        locate(info_store, &(info_store->collision_count), key, &index);
    if (result == LOCATE_INVALID_ARGUMENT)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (result == LOCATE_ENTRY_FOUND) {
        if (info_store->entries[index].node->action_count != action_count)
            return CFR_STATUS_INVALID_ARGUMENT;
        *node_out = info_store->entries[index].node;
        return CFR_STATUS_SUCCESS;
    }
    const ArenaMark mark = arena_mark(info_store);
    InfoNode *temp = NULL;
    Status init = arena_allocate_node(info_store, key, action_count, &temp);
    if (init != CFR_STATUS_SUCCESS) {
        arena_rollback(info_store, mark);
        return init;
    }
    if (result == LOCATE_STORE_FULL ||
        (info_store->size >= info_store->capacity - info_store->capacity / 4)) {
        Status resize_status = resize(info_store);
        if (resize_status != CFR_STATUS_SUCCESS) {
            arena_rollback(info_store, mark);
            return resize_status;
        }
        result =
            locate(info_store, &(info_store->collision_count), key, &index);
        if (result != LOCATE_EMPTY_SLOT_FOUND) {
            arena_rollback(info_store, mark);
            return CFR_STATUS_INVALID_ARGUMENT;
        }
    }
    info_store->entries[index].key = key;
    info_store->entries[index].node = temp;
    ++info_store->size;
    *node_out = info_store->entries[index].node;
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_get_stats(const InfoStore *info_store,
                                InfoStoreStats *stats_out) {
    if (info_store == NULL || stats_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    ConcurrentInfoStore *concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL)
        return concurrent_get_stats(concurrent, stats_out);
    store_read_lock(info_store);
    concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL) {
        store_read_unlock(info_store);
        return concurrent_get_stats(concurrent, stats_out);
    }
    stats_out->capacity = info_store->capacity;
    stats_out->collision_count =
        __atomic_load_n(&info_store->collision_count, __ATOMIC_RELAXED);
    stats_out->growth_count = info_store->growth_count;
    stats_out->size = info_store->size;
    store_read_unlock(info_store);
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_find_const(const InfoStore *info_store, InfoSetKey key,
                                 const InfoNode **node_out) {
    if (info_store == NULL || node_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    ConcurrentInfoStore *concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL) {
        InfoNode *node = NULL;
        const Status status = concurrent_find_node(concurrent, key, &node);

        if (status == CFR_STATUS_SUCCESS || status == CFR_STATUS_NOT_FOUND)
            *node_out = node;
        return status;
    }
    size_t index;
    store_read_lock(info_store);
    concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL) {
        InfoNode *node = NULL;

        store_read_unlock(info_store);
        const Status status = concurrent_find_node(concurrent, key, &node);
        if (status == CFR_STATUS_SUCCESS || status == CFR_STATUS_NOT_FOUND)
            *node_out = node;
        return status;
    }
    LocateResult result = locate(info_store, NULL, key, &index);
    if (result == LOCATE_ENTRY_FOUND) {
        *node_out = info_store->entries[index].node;
    } else if (result == LOCATE_EMPTY_SLOT_FOUND ||
               result == LOCATE_STORE_FULL) {
        *node_out = NULL;
    }
    store_read_unlock(info_store);
    if (result == LOCATE_ENTRY_FOUND) {
        return CFR_STATUS_SUCCESS;
    } else if (result == LOCATE_EMPTY_SLOT_FOUND ||
               result == LOCATE_STORE_FULL) {
        return CFR_STATUS_NOT_FOUND;
    } else
        return CFR_STATUS_INVALID_ARGUMENT;
}

Status cfr_info_store_snapshot_sorted(const InfoStore *info_store,
                                      const InfoNode ***nodes_out,
                                      size_t *count_out) {
    const InfoNode **nodes = NULL;
    size_t count = 0;
    size_t index;
    Status status = CFR_STATUS_SUCCESS;

    if (info_store == NULL || nodes_out == NULL || count_out == NULL) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    ConcurrentInfoStore *concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL)
        return concurrent_snapshot_sorted(concurrent, nodes_out, count_out);
    store_read_lock(info_store);
    concurrent = concurrent_state_load(info_store);
    if (concurrent != NULL) {
        store_read_unlock(info_store);
        return concurrent_snapshot_sorted(concurrent, nodes_out, count_out);
    }
    if (info_store->entries == NULL || info_store->capacity == 0 ||
        info_store->size > info_store->capacity ||
        info_store->size > SIZE_MAX / sizeof(*nodes)) {
        store_read_unlock(info_store);
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    const size_t expected_count = info_store->size;
    if (expected_count > 0) {
        nodes = malloc(expected_count * sizeof(*nodes));
        if (nodes == NULL) {
            store_read_unlock(info_store);
            return CFR_STATUS_OUT_OF_MEMORY;
        }
    }
    for (index = 0; index < info_store->capacity; index += 1) {
        const InfoNode *node = info_store->entries[index].node;

        if (node == NULL)
            continue;
        if (count == expected_count) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto unlock;
        }
        nodes[count] = node;
        count += 1;
    }
    if (count != expected_count) {
        status = CFR_STATUS_INVALID_ARGUMENT;
        goto unlock;
    }
unlock:
    store_read_unlock(info_store);
    if (status != CFR_STATUS_SUCCESS)
        goto cleanup;
    status = radix_sort_nodes(nodes, count);
    if (status != CFR_STATUS_SUCCESS)
        goto cleanup;
    for (index = 1; index < count; index += 1) {
        if (nodes[index - 1]->key == nodes[index]->key) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }
    *nodes_out = nodes;
    *count_out = count;
    nodes = NULL;

cleanup:
    free(nodes);
    return status;
}

Status cfr_info_store_visit_sorted(const InfoStore *info_store,
                                   InfoStoreConstVisitor visitor,
                                   void *context) {
    const InfoNode **nodes = NULL;
    size_t count = 0;
    Status status;

    if (visitor == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    status = cfr_info_store_snapshot_sorted(info_store, &nodes, &count);
    if (status != CFR_STATUS_SUCCESS)
        return status;
    for (size_t index = 0; index < count; index += 1) {
        status = visitor(nodes[index], context);
        if (status != CFR_STATUS_SUCCESS)
            break;
    }
    free(nodes);
    return status;
}

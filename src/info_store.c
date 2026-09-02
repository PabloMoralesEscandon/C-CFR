
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/info_store.h"
#include "info_node_internal.h"
#include "info_store_internal.h"
#include "spin_wait_internal.h"

enum { INITIAL_STORE_CAPACITY = 8 };

#define INFO_ARENA_BLOCK_CAPACITY ((size_t)1048576)

#define HASH_MULTIPLIER UINT64_C(11400714819323198485)

typedef enum {
    LOCATE_INVALID_ARGUMENT,
    LOCATE_ENTRY_FOUND,
    LOCATE_EMPTY_SLOT_FOUND,
    LOCATE_STORE_FULL
} LocateResult;

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
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_reserve(InfoStore *info_store,
                              size_t minimum_node_capacity) {
    Status status = CFR_STATUS_SUCCESS;

    if (info_store == NULL ||
        __atomic_load_n(&info_store->entries, __ATOMIC_ACQUIRE) == NULL ||
        __atomic_load_n(&info_store->capacity, __ATOMIC_ACQUIRE) == 0) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    store_write_lock(info_store);
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
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_find(InfoStore *info_store, InfoSetKey key,
                           InfoNode **node_out) {
    if (info_store == NULL || node_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    size_t index;
    size_t collisions = 0;

    store_read_lock(info_store);
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
    size_t index;
    size_t collisions = 0;

    store_read_lock(info_store);
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

Status cfr_info_store_get_stats(const InfoStore *info_store,
                                InfoStoreStats *stats_out) {
    if (info_store == NULL || stats_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    store_read_lock(info_store);
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
    size_t index;
    store_read_lock(info_store);
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
    store_read_lock(info_store);
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

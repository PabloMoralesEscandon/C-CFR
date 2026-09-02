
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/info_store.h"
#include "info_store_internal.h"

enum { INITIAL_STORE_CAPACITY = 8 };

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
    atomic_size_t *synchronization =
        (atomic_size_t *)&info_store->synchronization;
    size_t state =
        atomic_load_explicit(synchronization, memory_order_relaxed);

    for (;;) {
        while ((state & STORE_WRITER_MASK) != 0) {
            state =
                atomic_load_explicit(synchronization, memory_order_relaxed);
        }
        if (atomic_compare_exchange_weak_explicit(
                synchronization, &state, state + 1, memory_order_acquire,
                memory_order_relaxed)) {
            return;
        }
    }
}

static void store_read_unlock(const InfoStore *info_store) {
    atomic_fetch_sub_explicit(
        (atomic_size_t *)&info_store->synchronization, 1,
        memory_order_release);
}

static void store_write_lock(InfoStore *info_store) {
    if (atomic_exchange_explicit(&info_store->writer_gate, true,
                                 memory_order_acquire)) {
        do {
            while (atomic_load_explicit(&info_store->writer_gate,
                                        memory_order_relaxed)) {
            }
        } while (atomic_exchange_explicit(&info_store->writer_gate, true,
                                           memory_order_acquire));
    }
    atomic_fetch_or_explicit(&info_store->synchronization,
                             STORE_WRITER_PENDING, memory_order_acquire);
    size_t expected = STORE_WRITER_PENDING;
    while (!atomic_compare_exchange_weak_explicit(
        &info_store->synchronization, &expected, STORE_WRITER_ACTIVE,
        memory_order_acquire, memory_order_relaxed)) {
        expected = STORE_WRITER_PENDING;
    }
}

static void store_write_unlock(InfoStore *info_store) {
    atomic_store_explicit(&info_store->synchronization, 0,
                          memory_order_release);
    atomic_store_explicit(&info_store->writer_gate, false,
                          memory_order_release);
}

static void add_collisions(InfoStore *info_store, size_t amount) {
    size_t current;
    size_t desired;

    if (amount == 0)
        return;
    current = atomic_load_explicit(&info_store->collision_count,
                                   memory_order_relaxed);
    do {
        desired = current > SIZE_MAX - amount ? SIZE_MAX : current + amount;
    } while (!atomic_compare_exchange_weak_explicit(
        &info_store->collision_count, &current, desired, memory_order_relaxed,
        memory_order_relaxed));
}

static int compare_node_keys(const void *left_pointer,
                             const void *right_pointer) {
    const InfoNode *left = *(const InfoNode *const *)left_pointer;
    const InfoNode *right = *(const InfoNode *const *)right_pointer;

    if (left->key < right->key)
        return -1;
    if (left->key > right->key)
        return 1;
    return 0;
}

static uint64_t disperse(InfoSetKey key) {
    uint64_t value = (uint64_t)key;
    return value * HASH_MULTIPLIER;
}

static size_t bits_needed(size_t capacity) {
    size_t bits = 0;

    while (capacity > 1) {
        capacity >>= 1;
        bits += 1;
    }
    return bits;
}

static size_t initial_index(InfoSetKey key, size_t capacity) {
    uint64_t product = disperse(key);
    size_t bits = bits_needed(capacity);
    return product >> (64 - bits);
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

static Status resize(InfoStore *info_store) {
    if (info_store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (info_store->capacity > (SIZE_MAX / 2))
        return CFR_STATUS_OUT_OF_MEMORY;
    size_t new_capacity = 2 * info_store->capacity;
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
    free(info_store->entries);
    info_store->entries = temp_entries;
    info_store->capacity = new_capacity;
    ++info_store->growth_count;
    return CFR_STATUS_SUCCESS;
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
    atomic_init(&info_store->collision_count, 0);
    info_store->size = 0;
    info_store->entries = temp;
    atomic_init(&info_store->synchronization, 0);
    atomic_init(&info_store->writer_gate, false);
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_destroy(InfoStore *info_store) {
    if (info_store == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < info_store->capacity; i++) {
        if (info_store->entries[i].node != NULL) {
            cfr_info_node_destroy(info_store->entries[i].node);
            free(info_store->entries[i].node);
            info_store->entries[i].node = NULL;
            info_store->entries[i].key = 0;
        }
    }
    free(info_store->entries);
    info_store->entries = NULL;
    info_store->capacity = 0;
    atomic_init(&info_store->collision_count, 0);
    info_store->growth_count = 0;
    info_store->size = 0;
    atomic_init(&info_store->synchronization, 0);
    atomic_init(&info_store->writer_gate, false);
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

    InfoNode *temp = malloc(sizeof(InfoNode));
    if (temp == NULL)
        return CFR_STATUS_OUT_OF_MEMORY;
    *temp = (InfoNode){0};
    Status init = cfr_info_node_init(temp, key, action_count);
    if (init != CFR_STATUS_SUCCESS) {
        cfr_info_node_destroy(temp);
        free(temp);
        temp = NULL;
        return init;
    }

    store_write_lock(info_store);
    collisions = 0;
    result = locate(info_store, NULL, key, &index);
    if (result == LOCATE_INVALID_ARGUMENT) {
        store_write_unlock(info_store);
        cfr_info_node_destroy(temp);
        free(temp);
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (result == LOCATE_ENTRY_FOUND) {
        InfoNode *node = info_store->entries[index].node;
        const bool count_matches = node->action_count == action_count;

        store_write_unlock(info_store);
        cfr_info_node_destroy(temp);
        free(temp);
        if (!count_matches)
            return CFR_STATUS_INVALID_ARGUMENT;
        *node_out = node;
        return CFR_STATUS_SUCCESS;
    }
    if (result == LOCATE_STORE_FULL ||
        (info_store->size >= info_store->capacity - info_store->capacity / 4)) {
        Status resize_status = resize(info_store);
        if (resize_status != CFR_STATUS_SUCCESS) {
            store_write_unlock(info_store);
            cfr_info_node_destroy(temp);
            free(temp);
            temp = NULL;
            return resize_status;
        }
        collisions = 0;
        result = locate(info_store, &collisions, key, &index);
        if (result != LOCATE_EMPTY_SLOT_FOUND) {
            store_write_unlock(info_store);
            add_collisions(info_store, collisions);
            cfr_info_node_destroy(temp);
            free(temp);
            temp = NULL;
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
    stats_out->collision_count = atomic_load_explicit(
        &info_store->collision_count, memory_order_relaxed);
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
    if (count > 1)
        qsort(nodes, count, sizeof(*nodes), compare_node_keys);
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

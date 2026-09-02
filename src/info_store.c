
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cfr/info_store.h"
#include "info_node_internal.h"
#include "info_store_internal.h"

enum { INITIAL_STORE_CAPACITY = 8 };

#define HASH_MULTIPLIER UINT64_C(11400714819323198485)

typedef enum {
    LOCATE_INVALID_ARGUMENT,
    LOCATE_ENTRY_FOUND,
    LOCATE_EMPTY_SLOT_FOUND,
    LOCATE_STORE_FULL
} LocateResult;

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
    free(info_store->entries);
    info_store->entries = temp_entries;
    info_store->capacity = new_capacity;
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
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_reserve(InfoStore *info_store,
                              size_t minimum_node_capacity) {
    if (info_store == NULL || info_store->entries == NULL ||
        info_store->capacity == 0) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    size_t target = info_store->capacity;
    while (minimum_node_capacity > target - target / 4) {
        if (target > SIZE_MAX / 2)
            return CFR_STATUS_OUT_OF_MEMORY;
        target *= 2;
    }
    if (target == info_store->capacity)
        return CFR_STATUS_SUCCESS;
    return rebuild(info_store, target);
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
    info_store->collision_count = 0;
    info_store->growth_count = 0;
    info_store->size = 0;
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_find(InfoStore *info_store, InfoSetKey key,
                           InfoNode **node_out) {
    if (info_store == NULL || node_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    size_t index;
    LocateResult result =
        locate(info_store, &(info_store->collision_count), key, &index);
    switch (result) {

    case LOCATE_ENTRY_FOUND:
        *node_out = info_store->entries[index].node;
        return CFR_STATUS_SUCCESS;

    case LOCATE_EMPTY_SLOT_FOUND:
    case LOCATE_STORE_FULL:
        *node_out = NULL;
        return CFR_STATUS_NOT_FOUND;

    case LOCATE_INVALID_ARGUMENT:
        return CFR_STATUS_INVALID_ARGUMENT;
    }

    return CFR_STATUS_INVALID_ARGUMENT;
}

Status cfr_info_store_get_or_create(InfoStore *info_store, InfoSetKey key,
                                    size_t action_count, InfoNode **node_out) {
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
    InfoNode *temp = NULL;
    Status init = cfr_info_node_create(key, action_count, &temp);
    if (init != CFR_STATUS_SUCCESS) {
        return init;
    }
    if (result == LOCATE_STORE_FULL ||
        (info_store->size >= info_store->capacity - info_store->capacity / 4)) {
        Status resize_status = resize(info_store);
        if (resize_status != CFR_STATUS_SUCCESS) {
            cfr_info_node_destroy(temp);
            free(temp);
            temp = NULL;
            return resize_status;
        }
        result =
            locate(info_store, &(info_store->collision_count), key, &index);
        if (result != LOCATE_EMPTY_SLOT_FOUND) {
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
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_get_stats(const InfoStore *info_store,
                                InfoStoreStats *stats_out) {
    if (info_store == NULL || stats_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    stats_out->capacity = info_store->capacity;
    stats_out->collision_count = info_store->collision_count;
    stats_out->growth_count = info_store->growth_count;
    stats_out->size = info_store->size;
    return CFR_STATUS_SUCCESS;
}

Status cfr_info_store_find_const(const InfoStore *info_store, InfoSetKey key,
                                 const InfoNode **node_out) {
    if (info_store == NULL || node_out == NULL)
        return CFR_STATUS_INVALID_ARGUMENT;
    size_t index;
    LocateResult result = locate(info_store, NULL, key, &index);
    if (result == LOCATE_INVALID_ARGUMENT)
        return CFR_STATUS_INVALID_ARGUMENT;
    if (result == LOCATE_ENTRY_FOUND) {
        *node_out = info_store->entries[index].node;
        return CFR_STATUS_SUCCESS;
    } else if (result == LOCATE_EMPTY_SLOT_FOUND ||
               result == LOCATE_STORE_FULL) {
        *node_out = NULL;
        return CFR_STATUS_NOT_FOUND;
    } else
        return CFR_STATUS_INVALID_ARGUMENT;
}

Status cfr_info_store_visit_sorted(const InfoStore *info_store,
                                   InfoStoreConstVisitor visitor,
                                   void *context) {
    const InfoNode **nodes = NULL;
    size_t count = 0;
    size_t index;
    Status status = CFR_STATUS_SUCCESS;

    if (info_store == NULL || info_store->entries == NULL ||
        info_store->capacity == 0 || visitor == NULL ||
        info_store->size > info_store->capacity ||
        info_store->size > SIZE_MAX / sizeof(*nodes)) {
        return CFR_STATUS_INVALID_ARGUMENT;
    }
    if (info_store->size > 0) {
        nodes = malloc(info_store->size * sizeof(*nodes));
        if (nodes == NULL)
            return CFR_STATUS_OUT_OF_MEMORY;
    }
    for (index = 0; index < info_store->capacity; index += 1) {
        const InfoNode *node = info_store->entries[index].node;

        if (node == NULL)
            continue;
        if (count == info_store->size) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
        nodes[count] = node;
        count += 1;
    }
    if (count != info_store->size) {
        status = CFR_STATUS_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (count > 1)
        qsort(nodes, count, sizeof(*nodes), compare_node_keys);
    for (index = 1; index < count; index += 1) {
        if (nodes[index - 1]->key == nodes[index]->key) {
            status = CFR_STATUS_INVALID_ARGUMENT;
            goto cleanup;
        }
    }
    for (index = 0; index < count; index += 1) {
        status = visitor(nodes[index], context);
        if (status != CFR_STATUS_SUCCESS)
            break;
    }

cleanup:
    free(nodes);
    return status;
}

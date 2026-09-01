#ifndef CFR_INFO_STORE_H
#define CFR_INFO_STORE_H

#include <stddef.h>

#include "cfr/info_node.h"
#include "cfr/types.h"

CFR_EXTERN_C_BEGIN

typedef struct CfrInfoStoreEntry InfoStoreEntry;

/*
 * Maps information-set keys to learning nodes.
 *
 * The caller owns the InfoStore structure. The store owns the cell array, each
 * InfoNode, and each node's internal arrays. The caller must not modify
 * InfoStore fields.
 *
 * The caller must not copy an initialized store by assignment. A copy would
 * contain the same pointers and would not have independent ownership.
 *
 * The store does not support deleting individual nodes. cfr_info_store_destroy
 * frees all resources owned by the store.
 */
typedef struct {
    /* Owned array of private cells. The caller does not use this pointer. */
    InfoStoreEntry *entries;
    /* Number of nodes in the store. */
    size_t size;
    /* Number of cells allocated in the array. */
    size_t capacity;
    /* Cells with other keys encountered by find and get_or_create. */
    size_t collision_count;
    /* Number of successful growth operations. */
    size_t growth_count;
} InfoStore;

/* Contains a snapshot of the store statistics. */
typedef struct {
    /* Number of nodes in the store. */
    size_t size;
    /* Number of cells allocated in the array. */
    size_t capacity;
    /*
     * Cumulative number of cells with other keys encountered by find and
     * get_or_create. The counter saturates at SIZE_MAX and does not reset.
     */
    size_t collision_count;
    /* Number of successful growth operations. */
    size_t growth_count;
} InfoStoreStats;

/*
 * Receives one borrowed learning node while visiting a store.
 *
 * The callback must not retain or modify node. context belongs to the caller
 * and can be null. Returning an error stops the visit and propagates that
 * status to the caller.
 */
typedef Status (*InfoStoreConstVisitor)(const InfoNode *node, void *context);

/*
 * Initializes info_store and allocates the initial table.
 *
 * info_store must be zero-initialized or previously destroyed. The function
 * returns CFR_STATUS_OUT_OF_MEMORY when it cannot allocate the table. An error
 * preserves info_store.
 */
Status cfr_info_store_init(InfoStore *info_store);

/*
 * Ensures that info_store can hold minimum_node_capacity nodes without growth.
 *
 * info_store must be initialized. The function preserves all nodes and their
 * addresses. It does not reduce the current capacity. A zero minimum is a
 * successful no-op. A successful capacity increase counts as one growth.
 *
 * An invalid argument produces CFR_STATUS_INVALID_ARGUMENT. An allocation
 * failure produces CFR_STATUS_OUT_OF_MEMORY and preserves the store.
 */
Status cfr_info_store_reserve(InfoStore *info_store,
                              size_t minimum_node_capacity);

/*
 * Destroys info_store and sets all its fields to zero.
 *
 * The function frees the cells, nodes, and nodes' internal arrays. A
 * zero-initialized or previously destroyed store produces CFR_STATUS_SUCCESS.
 * A null pointer produces CFR_STATUS_INVALID_ARGUMENT.
 *
 * Destruction invalidates all pointers borrowed from the store. The function
 * does not free the caller-owned InfoStore structure.
 */
Status cfr_info_store_destroy(InfoStore *info_store);

/*
 * Finds key and writes the associated node to node_out.
 *
 * info_store must be initialized. If the key exists, node_out receives a
 * borrowed pointer. The caller must not destroy or free the node. Its address
 * remains stable as the store grows. The pointer becomes invalid when the
 * caller destroys the store.
 *
 * If the key does not exist, the function writes a null pointer and returns
 * CFR_STATUS_NOT_FOUND. An invalid argument produces
 * CFR_STATUS_INVALID_ARGUMENT and preserves node_out.
 *
 * Probing can increase collision_count even when the function does not find the
 * key.
 */
Status cfr_info_store_find(InfoStore *info_store, InfoSetKey key,
                           InfoNode **node_out);

/*
 * Gets the node for key or creates a node with action_count.
 *
 * info_store must be initialized. action_count must be greater than zero. If
 * the key exists, action_count must match the node's number of actions. A
 * different count produces CFR_STATUS_INVALID_ARGUMENT and preserves the node.
 *
 * If the key does not exist, the function creates a node. It increases capacity
 * before an insertion would exceed three quarters of the cells. Each growth
 * doubles the capacity and preserves node addresses.
 *
 * node_out receives a borrowed pointer only when the function completes
 * successfully. The caller must not destroy or free the node. Its address
 * remains stable as the store grows. The pointer becomes invalid when the
 * caller destroys the store.
 *
 * An error preserves node_out and does not publish a partial node. It preserves
 * existing nodes. Probing can increase collision_count before the error.
 */
Status cfr_info_store_get_or_create(InfoStore *info_store, InfoSetKey key,
                                    size_t action_count, InfoNode **node_out);

/*
 * Copies the current statistics to stats_out.
 *
 * The copy contains no pointers and does not change after another operation.
 * An invalid argument produces CFR_STATUS_INVALID_ARGUMENT and preserves
 * stats_out.
 */
Status cfr_info_store_get_stats(const InfoStore *info_store,
                                InfoStoreStats *stats_out);

/*
 * Finds key and publishes a const borrowed pointer to the associated node.
 *
 * info_store must be initialized. If the key exists, node_out receives a const
 * borrowed pointer. The caller must not destroy or free the node. Its address
 * remains stable as the store grows. The pointer becomes invalid when the
 * caller destroys the store.
 *
 * If the key does not exist, the function publishes a null pointer and returns
 * CFR_STATUS_NOT_FOUND. An invalid argument produces
 * CFR_STATUS_INVALID_ARGUMENT and preserves node_out.
 *
 * The function does not modify nodes. It also does not modify size, capacity,
 * collision_count, or growth_count.
 */
Status cfr_info_store_find_const(const InfoStore *info_store, InfoSetKey key,
                                 const InfoNode **node_out);

/*
 * Visits every node in strictly increasing information-set key order.
 *
 * info_store must be initialized and visitor must not be null. The function
 * borrows every node only for the duration of its callback. It does not modify
 * the store or its statistics. Temporary pointer storage is allocated when
 * the store is nonempty.
 *
 * Success visits every node exactly once. A callback error is returned
 * unchanged after the completed callbacks. Invalid arguments produce
 * CFR_STATUS_INVALID_ARGUMENT; temporary allocation failure produces
 * CFR_STATUS_OUT_OF_MEMORY.
 */
Status cfr_info_store_visit_sorted(const InfoStore *info_store,
                                   InfoStoreConstVisitor visitor,
                                   void *context);

CFR_EXTERN_C_END

#endif

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
 *
 * After initialization, lookup, insertion, capacity reservation, statistics,
 * and sorted visits can run concurrently. The store can be prepared for a
 * sharded concurrent access path whose existing-key lookups do not take a
 * lock. Initialization and destruction require exclusive ownership. During
 * concurrent use, callers must use the public operations instead of reading
 * the fields directly.
 */
typedef struct {
    /* Owned array of private cells. The caller does not use this pointer. */
    InfoStoreEntry *entries;
    /* Private linked arena blocks that own all information nodes. */
    void *node_blocks;
    /* Number of nodes in the store. */
    size_t size;
    /* Number of cells allocated in the array. */
    size_t capacity;
    /* Private collision diagnostic. Use cfr_info_store_get_stats. */
    size_t collision_count;
    /* Number of successful growth operations. */
    size_t growth_count;
    /* Private reader/writer lock state. The caller must not access it. */
    size_t synchronization;
    unsigned char writer_gate;
    /* Private sharded concurrent state. The caller must not access it. */
    void *concurrent_state;
} InfoStore;

/* Contains a snapshot of the store statistics. */
typedef struct {
    /* Number of nodes in the store. */
    size_t size;
    /* Number of cells allocated in the array. */
    size_t capacity;
    /*
     * Collision diagnostic that saturates at SIZE_MAX and does not reset.
     * Sequential mode counts lookup probes. To keep concurrent reads free of
     * shared writes, prepared concurrent mode counts only probes made while a
     * shard writer is creating or rechecking a node.
     */
    size_t collision_count;
    /* Number of successful growth operations. */
    size_t growth_count;
} InfoStoreStats;

/*
 * Receives one borrowed learning node while visiting a store.
 *
 * The callback must not retain or modify node. During concurrent training it
 * may inspect only immutable fields such as key and action_count. To read
 * learning data, it must use a read-only operation such as
 * cfr_info_node_average_strategy; direct array access is not synchronized.
 * context belongs to the caller and can be null. Returning an error stops the
 * visit and propagates that status to the caller.
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
 * Prepares a sharded access path for concurrent readers and writers.
 *
 * Calling this operation before starting worker threads keeps the one-time
 * conversion and allocation cost outside concurrent training. The concurrent
 * MCCFR trainer also prepares the path before its first traversal, so calling
 * this function is an optional performance hint rather than a correctness
 * requirement.
 *
 * Existing nodes and their addresses are preserved. Once prepared, the store
 * remains in concurrent mode until destruction. A previously prepared store
 * produces CFR_STATUS_SUCCESS. An allocation failure preserves the original
 * sequential representation.
 */
Status cfr_info_store_prepare_concurrent(InfoStore *info_store);

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
 * before an insertion would exceed three quarters of the relevant table. A
 * sequential growth doubles the store table; a concurrent growth doubles the
 * affected shard. Both preserve node addresses.
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
 * CFR_STATUS_OUT_OF_MEMORY. A concurrent insertion can appear either in this
 * visit or in the next one; the structural snapshot itself is consistent.
 */
Status cfr_info_store_visit_sorted(const InfoStore *info_store,
                                   InfoStoreConstVisitor visitor,
                                   void *context);

CFR_EXTERN_C_END

#endif

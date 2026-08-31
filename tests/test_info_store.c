#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfr/info_store.h"
#include "support/test_allocator.h"
#include "test_suite.h"

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            failures += 1;                                                      \
        }                                                                      \
    } while (0)

static InfoNode *sentinel_node(void) {
    return (InfoNode *)(uintptr_t)1;
}

static void initialize_store(InfoStore *store) {
    memset(store, 0, sizeof(*store));
    CHECK(cfr_info_store_init(store) == CFR_STATUS_SUCCESS);
}

static void destroy_store(InfoStore *store) {
    CHECK(cfr_info_store_destroy(store) == CFR_STATUS_SUCCESS);
}

static InfoStoreStats get_stats(const InfoStore *store) {
    InfoStoreStats stats = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};

    CHECK(cfr_info_store_get_stats(store, &stats) == CFR_STATUS_SUCCESS);
    return stats;
}

static void check_same_structural_stats(const InfoStoreStats *left,
                                        const InfoStoreStats *right) {
    CHECK(left->size == right->size);
    CHECK(left->capacity == right->capacity);
    CHECK(left->growth_count == right->growth_count);
}

static void test_initialization_and_empty_store(void) {
    InfoStore store = {0};
    InfoStoreStats stats;
    InfoStoreStats sentinel_stats = {11, 12, 13, 14};
    InfoNode *node = sentinel_node();

    CHECK(cfr_info_store_init(NULL) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_store_find(NULL, 7, &node) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(node == sentinel_node());
    CHECK(cfr_info_store_get_or_create(NULL, 7, 2, &node) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(node == sentinel_node());
    CHECK(cfr_info_store_get_stats(NULL, &sentinel_stats) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(sentinel_stats.size == 11 && sentinel_stats.capacity == 12 &&
          sentinel_stats.collision_count == 13 &&
          sentinel_stats.growth_count == 14);
    CHECK(cfr_info_store_destroy(NULL) == CFR_STATUS_INVALID_ARGUMENT);

    initialize_store(&store);
    stats = get_stats(&store);
    CHECK(stats.size == 0);
    CHECK(stats.capacity == 8);
    CHECK(stats.collision_count == 0);
    CHECK(stats.growth_count == 0);
    CHECK(cfr_info_store_get_stats(&store, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);

    CHECK(cfr_info_store_find(&store, 7, &node) == CFR_STATUS_NOT_FOUND);
    CHECK(node == NULL);
    stats = get_stats(&store);
    CHECK(stats.size == 0 && stats.capacity == 8);
    CHECK(stats.collision_count == 0 && stats.growth_count == 0);

    node = sentinel_node();
    CHECK(cfr_info_store_find(&store, 7, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_store_get_or_create(&store, 7, 2, NULL) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_store_get_or_create(&store, 7, 0, &node) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(node == sentinel_node());

    destroy_store(&store);
    CHECK(store.entries == NULL);
    CHECK(store.size == 0 && store.capacity == 0);
    CHECK(store.collision_count == 0 && store.growth_count == 0);
    destroy_store(&store);
    CHECK(store.entries == NULL);
    CHECK(store.size == 0 && store.capacity == 0);

    node = sentinel_node();
    CHECK(cfr_info_store_find(&store, 7, &node) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(node == sentinel_node());
}

static void test_insert_find_and_reuse(void) {
    InfoStore store;
    InfoNode *created = NULL;
    InfoNode *found = NULL;
    InfoNode *reused = NULL;
    InfoNode *output = sentinel_node();
    InfoStoreStats before;
    InfoStoreStats after;

    initialize_store(&store);
    CHECK(cfr_info_store_get_or_create(&store, 2, 2, &created) ==
          CFR_STATUS_SUCCESS);
    CHECK(created != NULL);
    CHECK(created->key == 2);
    CHECK(created->action_count == 2);
    CHECK(created->regret_sums[0] == 0.0);
    CHECK(created->strategy_sums[0] == 0.0);
    CHECK(cfr_info_node_add_regret(created, 0, 1.25) == CFR_STATUS_SUCCESS);

    CHECK(cfr_info_store_find(&store, 2, &found) == CFR_STATUS_SUCCESS);
    CHECK(found == created);
    CHECK(found->regret_sums[0] == 1.25);
    CHECK(cfr_info_store_get_or_create(&store, 2, 2, &reused) ==
          CFR_STATUS_SUCCESS);
    CHECK(reused == created);
    CHECK(reused->regret_sums[0] == 1.25);
    before = get_stats(&store);
    CHECK(before.size == 1);

    CHECK(cfr_info_store_get_or_create(&store, 2, 3, &output) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(output == sentinel_node());
    after = get_stats(&store);
    check_same_structural_stats(&before, &after);
    CHECK(created->action_count == 2);
    CHECK(created->regret_sums[0] == 1.25);

    CHECK(cfr_info_store_get_or_create(&store, 3, 1, &output) ==
          CFR_STATUS_SUCCESS);
    after = get_stats(&store);
    CHECK(before.size == 1);
    CHECK(after.size == 2);
    destroy_store(&store);
}

static bool keys_share_initial_slot(InfoSetKey first, InfoSetKey second) {
    InfoStore store;
    InfoNode *node = NULL;
    InfoStoreStats stats;
    bool collide = false;

    initialize_store(&store);
    if (cfr_info_store_get_or_create(&store, first, 1, &node) ==
            CFR_STATUS_SUCCESS &&
        cfr_info_store_get_or_create(&store, second, 1, &node) ==
            CFR_STATUS_SUCCESS) {
        stats = get_stats(&store);
        collide = stats.collision_count == 1;
    } else {
        CHECK(false);
    }
    destroy_store(&store);
    return collide;
}

static bool find_collision_group(InfoSetKey keys[4]) {
    InfoSetKey anchor;

    for (anchor = 0; anchor < 25; anchor += 1) {
        size_t count = 1;
        InfoSetKey candidate;

        keys[0] = anchor;
        for (candidate = anchor + 1; candidate < 25 && count < 4;
             candidate += 1) {
            if (keys_share_initial_slot(anchor, candidate)) {
                keys[count] = candidate;
                count += 1;
            }
        }
        if (count == 4) {
            return true;
        }
    }
    return false;
}

static void test_known_collisions(void) {
    InfoStore store;
    InfoNode *nodes[3] = {NULL, NULL, NULL};
    InfoNode *found = NULL;
    InfoSetKey keys[4] = {0, 0, 0, 0};
    InfoStoreStats stats;
    size_t index;

    CHECK(find_collision_group(keys));
    initialize_store(&store);
    for (index = 0; index < 3; index += 1) {
        CHECK(cfr_info_store_get_or_create(&store, keys[index], 2,
                                           &nodes[index]) ==
              CFR_STATUS_SUCCESS);
        CHECK(nodes[index] != NULL);
    }
    CHECK(nodes[0] != nodes[1]);
    CHECK(nodes[0] != nodes[2]);
    CHECK(nodes[1] != nodes[2]);
    stats = get_stats(&store);
    CHECK(stats.size == 3);
    CHECK(stats.capacity == 8);
    CHECK(stats.collision_count == 3);

    for (index = 0; index < 3; index += 1) {
        CHECK(cfr_info_store_find(&store, keys[index], &found) ==
              CFR_STATUS_SUCCESS);
        CHECK(found == nodes[index]);
    }
    stats = get_stats(&store);
    CHECK(stats.collision_count == 6);

    found = sentinel_node();
    CHECK(cfr_info_store_find(&store, keys[3], &found) ==
          CFR_STATUS_NOT_FOUND);
    CHECK(found == NULL);
    stats = get_stats(&store);
    CHECK(stats.collision_count == 9);

    store.collision_count = SIZE_MAX;
    CHECK(cfr_info_store_get_or_create(&store, keys[2], 2, &found) ==
          CFR_STATUS_SUCCESS);
    CHECK(found == nodes[2]);
    CHECK(store.collision_count == SIZE_MAX);
    destroy_store(&store);
}

static void test_extreme_signed_keys(void) {
    InfoStore store;
    const InfoSetKey keys[] = {INT64_MIN, INT64_MAX, -1, 0, 1};
    InfoNode *nodes[sizeof(keys) / sizeof(keys[0])] = {0};
    InfoNode *found = NULL;
    size_t index;

    initialize_store(&store);
    for (index = 0; index < sizeof(keys) / sizeof(keys[0]); index += 1) {
        CHECK(cfr_info_store_get_or_create(&store, keys[index], index + 1,
                                           &nodes[index]) ==
              CFR_STATUS_SUCCESS);
        CHECK(nodes[index]->key == keys[index]);
        CHECK(nodes[index]->action_count == index + 1);
    }
    for (index = sizeof(keys) / sizeof(keys[0]); index > 0; index -= 1) {
        const size_t position = index - 1;
        CHECK(cfr_info_store_find(&store, keys[position], &found) ==
              CFR_STATUS_SUCCESS);
        CHECK(found == nodes[position]);
    }
    destroy_store(&store);
}

static void test_growth_preserves_nodes(void) {
    enum { NODE_COUNT = 200 };
    InfoStore store;
    InfoNode *nodes[NODE_COUNT] = {0};
    InfoNode *found = NULL;
    InfoStoreStats stats;
    size_t index;

    initialize_store(&store);
    for (index = 0; index < NODE_COUNT; index += 1) {
        const InfoSetKey key = (InfoSetKey)(index * 7919) - 500;
        const size_t action_count = index % 4 + 1;

        CHECK(cfr_info_store_get_or_create(&store, key, action_count,
                                           &nodes[index]) ==
              CFR_STATUS_SUCCESS);
        CHECK(nodes[index] != NULL);
        CHECK(cfr_info_node_add_regret(nodes[index], 0,
                                       (Utility)index + 0.25) ==
              CFR_STATUS_SUCCESS);
    }

    stats = get_stats(&store);
    CHECK(stats.size == NODE_COUNT);
    CHECK(stats.capacity >= NODE_COUNT);
    CHECK(stats.size <= stats.capacity - stats.capacity / 4);
    CHECK(stats.growth_count >= 2);

    for (index = NODE_COUNT; index > 0; index -= 1) {
        const size_t position = index - 1;
        const InfoSetKey key = (InfoSetKey)(position * 7919) - 500;

        CHECK(cfr_info_store_find(&store, key, &found) == CFR_STATUS_SUCCESS);
        CHECK(found == nodes[position]);
        CHECK(found->key == key);
        CHECK(found->action_count == position % 4 + 1);
        CHECK(found->regret_sums[0] == (Utility)position + 0.25);
    }
    destroy_store(&store);
}

static void test_exact_load_boundary(void) {
    InfoStore store;
    InfoNode *nodes[7] = {0};
    InfoNode *found = NULL;
    InfoStoreStats stats;
    size_t index;

    initialize_store(&store);
    for (index = 0; index < 6; index += 1) {
        CHECK(cfr_info_store_get_or_create(&store,
                                           (InfoSetKey)(5000 + index), 2,
                                           &nodes[index]) ==
              CFR_STATUS_SUCCESS);
    }
    stats = get_stats(&store);
    CHECK(stats.size == 6);
    CHECK(stats.capacity == 8);
    CHECK(stats.growth_count == 0);

    CHECK(cfr_info_store_get_or_create(&store, 5006, 2, &nodes[6]) ==
          CFR_STATUS_SUCCESS);
    stats = get_stats(&store);
    CHECK(stats.size == 7);
    CHECK(stats.capacity == 16);
    CHECK(stats.growth_count == 1);

    for (index = 0; index < 6; index += 1) {
        CHECK(cfr_info_store_find(&store, (InfoSetKey)(5000 + index),
                                  &found) == CFR_STATUS_SUCCESS);
        CHECK(found == nodes[index]);
    }
    destroy_store(&store);
}

static void test_high_bit_hash_quality(void) {
    enum { NODE_COUNT = 200, COLLISION_LIMIT = 2000 };
    InfoStore store;
    InfoNode *nodes[NODE_COUNT] = {0};
    InfoNode *found = NULL;
    InfoStoreStats insertion_stats;
    size_t index;

    initialize_store(&store);
    for (index = 0; index < NODE_COUNT; index += 1) {
        const InfoSetKey key = (InfoSetKey)index << 32;

        CHECK(cfr_info_store_get_or_create(&store, key, 2, &nodes[index]) ==
              CFR_STATUS_SUCCESS);
        CHECK(nodes[index] != NULL);
        CHECK(cfr_info_node_add_regret(nodes[index], 0,
                                       (Utility)index + 0.5) ==
              CFR_STATUS_SUCCESS);
    }

    insertion_stats = get_stats(&store);
    CHECK(insertion_stats.size == NODE_COUNT);
    if (insertion_stats.collision_count >= COLLISION_LIMIT) {
        fprintf(stderr,
                "%s:%d: too many collisions for keys with high bits: "
                "%zu (limit: %d)\n",
                __FILE__, __LINE__, insertion_stats.collision_count,
                COLLISION_LIMIT);
    }
    CHECK(insertion_stats.collision_count < COLLISION_LIMIT);

    for (index = NODE_COUNT; index > 0; index -= 1) {
        const size_t position = index - 1;
        const InfoSetKey key = (InfoSetKey)position << 32;

        CHECK(cfr_info_store_find(&store, key, &found) == CFR_STATUS_SUCCESS);
        CHECK(found == nodes[position]);
        CHECK(found->regret_sums[0] == (Utility)position + 0.5);
    }
    destroy_store(&store);
}

static void test_order_independence(void) {
    InfoStore forward;
    InfoStore reverse;
    const InfoSetKey keys[] = {19, -4, 77, INT64_MIN, 3, 91, 11};
    InfoNode *node = NULL;
    size_t index;

    initialize_store(&forward);
    initialize_store(&reverse);
    for (index = 0; index < sizeof(keys) / sizeof(keys[0]); index += 1) {
        CHECK(cfr_info_store_get_or_create(&forward, keys[index], 2, &node) ==
              CFR_STATUS_SUCCESS);
    }
    for (index = sizeof(keys) / sizeof(keys[0]); index > 0; index -= 1) {
        CHECK(cfr_info_store_get_or_create(&reverse, keys[index - 1], 2,
                                           &node) == CFR_STATUS_SUCCESS);
    }
    for (index = 0; index < sizeof(keys) / sizeof(keys[0]); index += 1) {
        CHECK(cfr_info_store_find(&forward, keys[index], &node) ==
              CFR_STATUS_SUCCESS);
        CHECK(node->key == keys[index]);
        CHECK(cfr_info_store_find(&reverse, keys[index], &node) ==
              CFR_STATUS_SUCCESS);
        CHECK(node->key == keys[index]);
    }
    CHECK(forward.size == reverse.size);
    destroy_store(&forward);
    destroy_store(&reverse);
}

typedef struct {
    InfoSetKey keys[8];
    size_t count;
    size_t stop_after;
} VisitContext;

static Status collect_visited_key(const InfoNode *node, void *raw_context) {
    VisitContext *context = raw_context;

    if (node == NULL || context == NULL || context->count >= 8)
        return CFR_STATUS_INVALID_ARGUMENT;
    context->keys[context->count] = node->key;
    context->count += 1;
    if (context->stop_after != 0 && context->count == context->stop_after)
        return CFR_STATUS_IO_ERROR;
    return CFR_STATUS_SUCCESS;
}

static void test_sorted_visit(void) {
    InfoStore store = {0};
    InfoNode *node = NULL;
    const InfoSetKey insertion_order[] = {7, -4, 99, 0};
    const InfoSetKey sorted_order[] = {-4, 0, 7, 99};
    VisitContext context = {0};
    size_t index;

    CHECK(cfr_info_store_visit_sorted(NULL, collect_visited_key, &context) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_store_visit_sorted(&store, collect_visited_key, &context) ==
          CFR_STATUS_INVALID_ARGUMENT);
    initialize_store(&store);
    CHECK(cfr_info_store_visit_sorted(&store, NULL, &context) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_store_visit_sorted(&store, collect_visited_key, &context) ==
          CFR_STATUS_SUCCESS);
    CHECK(context.count == 0);

    for (index = 0; index < sizeof(insertion_order) / sizeof(insertion_order[0]);
         index += 1) {
        CHECK(cfr_info_store_get_or_create(&store, insertion_order[index], 2,
                                           &node) == CFR_STATUS_SUCCESS);
    }
    CHECK(cfr_info_store_visit_sorted(&store, collect_visited_key, &context) ==
          CFR_STATUS_SUCCESS);
    CHECK(context.count == 4);
    for (index = 0; index < context.count; index += 1)
        CHECK(context.keys[index] == sorted_order[index]);

    context = (VisitContext){.stop_after = 2};
    CHECK(cfr_info_store_visit_sorted(&store, collect_visited_key, &context) ==
          CFR_STATUS_IO_ERROR);
    CHECK(context.count == 2);
    destroy_store(&store);
}

#ifdef CFR_TEST_WRAP_ALLOCATOR
static void check_failed_creation(InfoStore *store,
                                  size_t successful_allocations) {
    const InfoStoreStats before = get_stats(store);
    const size_t live_before = test_allocator_live_allocations();
    InfoNode *output = sentinel_node();
    InfoStoreStats after;

    test_allocator_fail_after(successful_allocations);
    CHECK(cfr_info_store_get_or_create(store, 1234567, 3, &output) ==
          CFR_STATUS_OUT_OF_MEMORY);
    CHECK(output == sentinel_node());
    test_allocator_disable_failures();
    after = get_stats(store);
    check_same_structural_stats(&before, &after);
    CHECK(test_allocator_live_allocations() == live_before);
}

static void test_allocation_failures(void) {
    InfoStore store = {0};
    InfoStoreStats before;
    InfoStoreStats after;
    InfoNode *node = sentinel_node();
    size_t live_before;
    size_t index;

    CHECK(test_allocator_live_allocations() == 0);
    test_allocator_fail_after(0);
    CHECK(cfr_info_store_init(&store) == CFR_STATUS_OUT_OF_MEMORY);
    CHECK(store.entries == NULL);
    CHECK(store.size == 0 && store.capacity == 0);
    CHECK(test_allocator_live_allocations() == 0);

    test_allocator_disable_failures();
    initialize_store(&store);
    check_failed_creation(&store, 0);
    check_failed_creation(&store, 1);
    check_failed_creation(&store, 2);

    before = get_stats(&store);
    live_before = test_allocator_live_allocations();
    node = sentinel_node();
    CHECK(cfr_info_store_get_or_create(&store, 7654321, SIZE_MAX, &node) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(node == sentinel_node());
    after = get_stats(&store);
    check_same_structural_stats(&before, &after);
    CHECK(test_allocator_live_allocations() == live_before);

    for (index = 0; index < 6; index += 1) {
        CHECK(cfr_info_store_get_or_create(&store, (InfoSetKey)(200 + index),
                                           2, &node) == CFR_STATUS_SUCCESS);
    }
    before = get_stats(&store);
    live_before = test_allocator_live_allocations();
    node = sentinel_node();
    test_allocator_fail_after(3);
    CHECK(cfr_info_store_get_or_create(&store, 9999, 2, &node) ==
          CFR_STATUS_OUT_OF_MEMORY);
    CHECK(node == sentinel_node());
    test_allocator_disable_failures();
    after = get_stats(&store);
    check_same_structural_stats(&before, &after);
    CHECK(test_allocator_live_allocations() == live_before);

    for (index = 0; index < 6; index += 1) {
        CHECK(cfr_info_store_find(&store, (InfoSetKey)(200 + index), &node) ==
              CFR_STATUS_SUCCESS);
    }
    destroy_store(&store);
    CHECK(test_allocator_live_allocations() == 0);

    initialize_store(&store);
    CHECK(cfr_info_store_get_or_create(&store, 41, 2, &node) ==
          CFR_STATUS_SUCCESS);
    destroy_store(&store);
    CHECK(test_allocator_live_allocations() == 0);
    initialize_store(&store);
    CHECK(cfr_info_store_get_or_create(&store, 42, 3, &node) ==
          CFR_STATUS_SUCCESS);
    {
        VisitContext context = {0};
        const size_t visit_live_before = test_allocator_live_allocations();

        test_allocator_fail_after(0);
        CHECK(cfr_info_store_visit_sorted(&store, collect_visited_key,
                                          &context) ==
              CFR_STATUS_OUT_OF_MEMORY);
        CHECK(context.count == 0);
        test_allocator_disable_failures();
        CHECK(test_allocator_live_allocations() == visit_live_before);
    }
    destroy_store(&store);
    CHECK(test_allocator_live_allocations() == 0);
}
#endif

int test_info_store(void) {
    failures = 0;

    test_initialization_and_empty_store();
    test_insert_find_and_reuse();
    test_known_collisions();
    test_extreme_signed_keys();
    test_growth_preserves_nodes();
    test_exact_load_boundary();
    test_high_bit_hash_quality();
    test_order_independence();
    test_sorted_visit();
#ifdef CFR_TEST_WRAP_ALLOCATOR
    test_allocation_failures();
    CHECK(test_allocator_live_allocations() == 0);
#endif

    return failures;
}

#ifdef CFR_TEST_INFO_STORE_STANDALONE
int main(void) {
    const int result = test_info_store();

    if (result != 0) {
        fprintf(stderr, "%d InfoStore checks failed.\n", result);
        return 1;
    }

    puts("All InfoStore tests completed successfully.");
    return 0;
}
#endif

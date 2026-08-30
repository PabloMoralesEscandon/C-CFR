#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfr/info_node.h"
#include "support/test_allocator.h"
#include "test_suite.h"

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: no se cumple: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            failures += 1;                                                      \
        }                                                                      \
    } while (0)

static bool near(double left, double right) {
    return fabs(left - right) <= 1e-12;
}

static void check_distribution(const Probability *strategy, size_t count) {
    double sum = 0.0;
    size_t index;

    for (index = 0; index < count; index += 1) {
        CHECK(isfinite(strategy[index]));
        CHECK(strategy[index] >= 0.0);
        CHECK(strategy[index] <= 1.0);
        sum += strategy[index];
    }
    CHECK(near(sum, 1.0));
}

static void initialize(InfoNode *node, InfoSetKey key, size_t action_count) {
    memset(node, 0, sizeof(*node));
    CHECK(cfr_info_node_init(node, key, action_count) == CFR_STATUS_SUCCESS);
}

static void destroy(InfoNode *node) {
    CHECK(cfr_info_node_destroy(node) == CFR_STATUS_SUCCESS);
}

static void check_unchanged(const double *values, const double *expected,
                            size_t count) {
    size_t index;

    for (index = 0; index < count; index += 1) {
        CHECK(values[index] == expected[index] ||
              (isnan(values[index]) && isnan(expected[index])));
    }
}

static void test_initialization_and_destruction(void) {
    InfoNode node = {0};
    InfoNode empty = {0};
    size_t index;

    CHECK(cfr_info_node_init(NULL, 7, 2) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_init(&empty, 7, 0) == CFR_STATUS_INVALID_ARGUMENT);
    CHECK(empty.key == 0);
    CHECK(empty.action_count == 0);
    CHECK(empty.regret_sums == NULL);
    CHECK(empty.strategy_sums == NULL);

    CHECK(cfr_info_node_init(&empty, 7, SIZE_MAX) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(empty.key == 0);
    CHECK(empty.action_count == 0);
    CHECK(empty.regret_sums == NULL);
    CHECK(empty.strategy_sums == NULL);

    CHECK(cfr_info_node_init(&node, -17, 3) == CFR_STATUS_SUCCESS);
    CHECK(node.key == -17);
    CHECK(node.action_count == 3);
    CHECK(node.regret_sums != NULL);
    CHECK(node.strategy_sums != NULL);
    for (index = 0; index < node.action_count; index += 1) {
        CHECK(node.regret_sums[index] == 0.0);
        CHECK(node.strategy_sums[index] == 0.0);
    }

    CHECK(cfr_info_node_destroy(NULL) == CFR_STATUS_INVALID_ARGUMENT);
    destroy(&node);
    CHECK(node.key == 0);
    CHECK(node.action_count == 0);
    CHECK(node.regret_sums == NULL);
    CHECK(node.strategy_sums == NULL);
    destroy(&node);
    CHECK(node.key == 0);
    CHECK(node.action_count == 0);
    CHECK(node.regret_sums == NULL);
    CHECK(node.strategy_sums == NULL);
}

#ifdef CFR_TEST_WRAP_ALLOCATOR
static void test_allocation_failures(void) {
    InfoNode node = {0};

    test_allocator_fail_after(0);
    CHECK(cfr_info_node_init(&node, 21, 2) == CFR_STATUS_OUT_OF_MEMORY);
    CHECK(node.key == 0);
    CHECK(node.action_count == 0);
    CHECK(node.regret_sums == NULL);
    CHECK(node.strategy_sums == NULL);
    CHECK(test_allocator_live_allocations() == 0);

    test_allocator_fail_after(1);
    CHECK(cfr_info_node_init(&node, 21, 2) == CFR_STATUS_OUT_OF_MEMORY);
    CHECK(node.key == 0);
    CHECK(node.action_count == 0);
    CHECK(node.regret_sums == NULL);
    CHECK(node.strategy_sums == NULL);
    CHECK(test_allocator_live_allocations() == 0);

    test_allocator_disable_failures();
    CHECK(cfr_info_node_init(&node, 21, 2) == CFR_STATUS_SUCCESS);
    CHECK(test_allocator_live_allocations() == 2);
    destroy(&node);
    CHECK(test_allocator_live_allocations() == 0);

    CHECK(cfr_info_node_init(&node, 22, 3) == CFR_STATUS_SUCCESS);
    CHECK(test_allocator_live_allocations() == 2);
    destroy(&node);
    CHECK(test_allocator_live_allocations() == 0);
}
#endif

static void test_single_action(void) {
    InfoNode node;
    Probability current[1] = {-1.0};
    Probability average[1] = {-1.0};
    const Probability used[1] = {1.0};

    initialize(&node, 5, 1);
    CHECK(cfr_info_node_current_strategy(&node, current, 1) ==
          CFR_STATUS_SUCCESS);
    CHECK(current[0] == 1.0);
    CHECK(cfr_info_node_add_regret(&node, 0, -DBL_MAX) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_current_strategy(&node, current, 1) ==
          CFR_STATUS_SUCCESS);
    CHECK(current[0] == 1.0);
    CHECK(cfr_info_node_accumulate_strategy(&node, used, 1, 0.25) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_average_strategy(&node, average, 1) ==
          CFR_STATUS_SUCCESS);
    CHECK(average[0] == 1.0);
    destroy(&node);
}

static void test_current_strategy_cases(void) {
    InfoNode node;
    Probability strategy[4] = {91.0, 92.0, 93.0, 94.0};
    Utility regret_snapshot[3];

    initialize(&node, 8, 3);

    CHECK(cfr_info_node_current_strategy(&node, strategy, 4) ==
          CFR_STATUS_SUCCESS);
    check_distribution(strategy, 3);
    CHECK(near(strategy[0], 1.0 / 3.0));
    CHECK(near(strategy[1], 1.0 / 3.0));
    CHECK(near(strategy[2], 1.0 / 3.0));
    CHECK(strategy[3] == 94.0);

    CHECK(cfr_info_node_add_regret(&node, 0, -2.0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_add_regret(&node, 1, -1.0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_add_regret(&node, 2, -8.0) == CFR_STATUS_SUCCESS);
    memcpy(regret_snapshot, node.regret_sums, sizeof(regret_snapshot));
    CHECK(cfr_info_node_current_strategy(&node, strategy, 3) ==
          CFR_STATUS_SUCCESS);
    check_distribution(strategy, 3);
    CHECK(near(strategy[0], 1.0 / 3.0));
    CHECK(near(strategy[1], 1.0 / 3.0));
    CHECK(near(strategy[2], 1.0 / 3.0));
    check_unchanged(node.regret_sums, regret_snapshot, 3);

    CHECK(cfr_info_node_add_regret(&node, 0, 6.0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_current_strategy(&node, strategy, 3) ==
          CFR_STATUS_SUCCESS);
    check_distribution(strategy, 3);
    CHECK(strategy[0] == 1.0);
    CHECK(strategy[1] == 0.0);
    CHECK(strategy[2] == 0.0);

    CHECK(cfr_info_node_add_regret(&node, 1, 3.0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_current_strategy(&node, strategy, 3) ==
          CFR_STATUS_SUCCESS);
    check_distribution(strategy, 3);
    CHECK(near(strategy[0], 2.0 / 3.0));
    CHECK(near(strategy[1], 1.0 / 3.0));
    CHECK(strategy[2] == 0.0);
    destroy(&node);
}

static void test_current_strategy_numeric_limits(void) {
    InfoNode node;
    Probability strategy[3] = {81.0, 82.0, 83.0};

    initialize(&node, 9, 2);
    node.regret_sums[0] = DBL_TRUE_MIN;
    node.regret_sums[1] = 0.0;
    CHECK(cfr_info_node_current_strategy(&node, strategy, 2) ==
          CFR_STATUS_SUCCESS);
    CHECK(strategy[0] == 1.0);
    CHECK(strategy[1] == 0.0);

    node.regret_sums[0] = DBL_MAX;
    node.regret_sums[1] = DBL_MAX / 2.0;
    CHECK(cfr_info_node_current_strategy(&node, strategy, 2) ==
          CFR_STATUS_SUCCESS);
    check_distribution(strategy, 2);
    CHECK(near(strategy[0], 2.0 / 3.0));
    CHECK(near(strategy[1], 1.0 / 3.0));

    node.regret_sums[0] = NAN;
    strategy[0] = 81.0;
    strategy[1] = 82.0;
    CHECK(cfr_info_node_current_strategy(&node, strategy, 2) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(strategy[0] == 81.0 && strategy[1] == 82.0);

    node.regret_sums[0] = INFINITY;
    CHECK(cfr_info_node_current_strategy(&node, strategy, 2) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(strategy[0] == 81.0 && strategy[1] == 82.0);

    node.regret_sums[0] = -INFINITY;
    CHECK(cfr_info_node_current_strategy(&node, strategy, 2) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(strategy[0] == 81.0 && strategy[1] == 82.0);
    destroy(&node);
}

static void test_current_strategy_invalid_arguments(void) {
    InfoNode node;
    InfoNode empty = {0};
    Probability strategy[3] = {71.0, 72.0, 73.0};

    initialize(&node, 10, 2);
    CHECK(cfr_info_node_current_strategy(NULL, strategy, 2) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_current_strategy(&empty, strategy, 2) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_current_strategy(&node, NULL, 2) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_current_strategy(&node, strategy, 0) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(strategy[0] == 71.0 && strategy[1] == 72.0 && strategy[2] == 73.0);
    CHECK(cfr_info_node_current_strategy(&node, strategy, 1) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(strategy[0] == 71.0 && strategy[1] == 72.0 && strategy[2] == 73.0);
    destroy(&node);
}

static void test_add_regret(void) {
    InfoNode node;
    Utility before[3];

    initialize(&node, 11, 3);
    CHECK(cfr_info_node_add_regret(&node, 0, 2.5) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_add_regret(&node, 0, -1.0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_add_regret(&node, 1, -3.0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_add_regret(&node, 2, 0.0) == CFR_STATUS_SUCCESS);
    CHECK(node.regret_sums[0] == 1.5);
    CHECK(node.regret_sums[1] == -3.0);
    CHECK(node.regret_sums[2] == 0.0);

    memcpy(before, node.regret_sums, sizeof(before));
    CHECK(cfr_info_node_add_regret(&node, 3, 1.0) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_add_regret(&node, SIZE_MAX, 1.0) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_add_regret(&node, 0, NAN) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_add_regret(&node, 0, INFINITY) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_add_regret(&node, 0, -INFINITY) ==
          CFR_STATUS_INVALID_ARGUMENT);
    check_unchanged(node.regret_sums, before, 3);

    node.regret_sums[0] = DBL_MAX;
    CHECK(cfr_info_node_add_regret(&node, 0, DBL_MAX) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(node.regret_sums[0] == DBL_MAX);
    CHECK(node.regret_sums[1] == -3.0);

    CHECK(cfr_info_node_add_regret(NULL, 0, 1.0) ==
          CFR_STATUS_INVALID_ARGUMENT);
    destroy(&node);
    CHECK(cfr_info_node_add_regret(&node, 0, 1.0) ==
          CFR_STATUS_INVALID_ARGUMENT);
}

static void test_weighted_average(void) {
    InfoNode node;
    const Probability uniform[2] = {0.5, 0.5};
    const Probability left[2] = {1.0, 0.0};
    const Probability right[2] = {0.0, 1.0};
    Probability average[3] = {61.0, 62.0, 63.0};

    initialize(&node, 12, 2);
    CHECK(cfr_info_node_average_strategy(&node, average, 3) ==
          CFR_STATUS_SUCCESS);
    check_distribution(average, 2);
    CHECK(near(average[0], 0.5));
    CHECK(near(average[1], 0.5));
    CHECK(average[2] == 63.0);

    CHECK(cfr_info_node_accumulate_strategy(&node, uniform, 2, 1.0) ==
          CFR_STATUS_SUCCESS);
    CHECK(near(node.strategy_sums[0], 0.5));
    CHECK(near(node.strategy_sums[1], 0.5));
    CHECK(cfr_info_node_accumulate_strategy(&node, left, 2, 0.5) ==
          CFR_STATUS_SUCCESS);
    CHECK(near(node.strategy_sums[0], 1.0));
    CHECK(near(node.strategy_sums[1], 0.5));

    CHECK(cfr_info_node_average_strategy(&node, average, 2) ==
          CFR_STATUS_SUCCESS);
    check_distribution(average, 2);
    CHECK(near(average[0], 2.0 / 3.0));
    CHECK(near(average[1], 1.0 / 3.0));

    CHECK(cfr_info_node_accumulate_strategy(&node, right, 2, 0.0) ==
          CFR_STATUS_SUCCESS);
    CHECK(near(node.strategy_sums[0], 1.0));
    CHECK(near(node.strategy_sums[1], 0.5));
    CHECK(node.regret_sums[0] == 0.0 && node.regret_sums[1] == 0.0);
    destroy(&node);
}

static void test_deterministic_queries(void) {
    InfoNode node;
    Probability current_first[3] = {0.0, 0.0, 0.0};
    Probability current_second[3] = {0.0, 0.0, 0.0};
    Probability average_first[3] = {0.0, 0.0, 0.0};
    Probability average_second[3] = {0.0, 0.0, 0.0};
    const Probability used[3] = {0.2, 0.3, 0.5};
    Utility regrets_before[3];
    double sums_before[3];

    initialize(&node, 17, 3);
    CHECK(cfr_info_node_add_regret(&node, 0, 2.0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_add_regret(&node, 1, 1.0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_add_regret(&node, 2, -8.0) == CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_accumulate_strategy(&node, used, 3, 0.75) ==
          CFR_STATUS_SUCCESS);
    memcpy(regrets_before, node.regret_sums, sizeof(regrets_before));
    memcpy(sums_before, node.strategy_sums, sizeof(sums_before));

    CHECK(cfr_info_node_current_strategy(&node, current_first, 3) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_current_strategy(&node, current_second, 3) ==
          CFR_STATUS_SUCCESS);
    check_unchanged(current_first, current_second, 3);
    CHECK(cfr_info_node_average_strategy(&node, average_first, 3) ==
          CFR_STATUS_SUCCESS);
    CHECK(cfr_info_node_average_strategy(&node, average_second, 3) ==
          CFR_STATUS_SUCCESS);
    check_unchanged(average_first, average_second, 3);
    check_unchanged(node.regret_sums, regrets_before, 3);
    check_unchanged(node.strategy_sums, sums_before, 3);
    destroy(&node);
}

static void check_composition_for_size(size_t action_count) {
    InfoNode node = {0};
    Probability *strategy;
    Status status;
    long double sum;
    size_t index;

    strategy = malloc(sizeof(*strategy) * action_count);
    CHECK(strategy != NULL);
    if (strategy == NULL) {
        return;
    }

    status = cfr_info_node_init(&node, 18, action_count);
    CHECK(status == CFR_STATUS_SUCCESS);
    if (status != CFR_STATUS_SUCCESS) {
        free(strategy);
        return;
    }

    status = cfr_info_node_current_strategy(&node, strategy, action_count);
    CHECK(status == CFR_STATUS_SUCCESS);
    if (status != CFR_STATUS_SUCCESS) {
        destroy(&node);
        free(strategy);
        return;
    }
    sum = 0.0L;
    for (index = 0; index < action_count; index += 1) {
        CHECK(isfinite(strategy[index]));
        CHECK(strategy[index] >= 0.0);
        CHECK(strategy[index] <= 1.0);
        sum += (long double)strategy[index];
    }
    /* En plataformas donde long double equivale a double, 100 000 sumandos
     * acumulan más de 1e-12 de redondeo. Use la tolerancia del módulo. */
    CHECK(fabsl(sum - 1.0L) <= 1e-8L);
    CHECK(cfr_info_node_accumulate_strategy(&node, strategy, action_count,
                                            1.0) == CFR_STATUS_SUCCESS);

    CHECK(cfr_info_node_add_regret(&node, 0, 3.0) == CFR_STATUS_SUCCESS);
    if (action_count > 1) {
        CHECK(cfr_info_node_add_regret(&node, action_count - 1, -2.0) ==
              CFR_STATUS_SUCCESS);
    }
    if (action_count > 2) {
        CHECK(cfr_info_node_add_regret(&node, action_count / 2, 1.0) ==
              CFR_STATUS_SUCCESS);
    }

    status = cfr_info_node_current_strategy(&node, strategy, action_count);
    CHECK(status == CFR_STATUS_SUCCESS);
    if (status != CFR_STATUS_SUCCESS) {
        destroy(&node);
        free(strategy);
        return;
    }
    CHECK(cfr_info_node_accumulate_strategy(&node, strategy, action_count,
                                            0.5) == CFR_STATUS_SUCCESS);

    destroy(&node);
    free(strategy);
}

static void test_generated_strategy_composition(void) {
    static const size_t action_counts[] = {1, 2, 3, 17, 100000};
    size_t index;

    for (index = 0; index < sizeof(action_counts) / sizeof(action_counts[0]);
         index += 1) {
        check_composition_for_size(action_counts[index]);
    }
}

static void expect_invalid_strategy(const Probability *strategy, size_t count,
                                    Probability weight) {
    InfoNode node;
    const double expected[3] = {2.0, 3.0, 4.0};

    initialize(&node, 13, 3);
    memcpy(node.strategy_sums, expected, sizeof(expected));
    CHECK(cfr_info_node_accumulate_strategy(&node, strategy, count, weight) ==
          CFR_STATUS_INVALID_ARGUMENT);
    check_unchanged(node.strategy_sums, expected, 3);
    destroy(&node);
}

static void test_accumulation_invalid_arguments(void) {
    InfoNode node;
    InfoNode empty = {0};
    const Probability valid[3] = {0.2, 0.3, 0.5};
    const Probability negative[3] = {-0.1, 0.6, 0.5};
    const Probability above_one[3] = {1.1, 0.0, 0.0};
    const Probability wrong_sum[3] = {0.2, 0.3, 0.4};
    const Probability with_nan[3] = {0.2, NAN, 0.8};
    const Probability with_infinity[3] = {0.2, INFINITY, 0.8};

    initialize(&node, 14, 3);
    CHECK(cfr_info_node_accumulate_strategy(NULL, valid, 3, 1.0) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_accumulate_strategy(&empty, valid, 3, 1.0) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_accumulate_strategy(&node, NULL, 3, 1.0) ==
          CFR_STATUS_INVALID_ARGUMENT);
    destroy(&node);

    expect_invalid_strategy(valid, 2, 1.0);
    expect_invalid_strategy(valid, 4, 1.0);
    expect_invalid_strategy(valid, 3, -0.1);
    expect_invalid_strategy(valid, 3, 1.1);
    expect_invalid_strategy(valid, 3, NAN);
    expect_invalid_strategy(valid, 3, INFINITY);
    expect_invalid_strategy(negative, 3, 1.0);
    expect_invalid_strategy(above_one, 3, 1.0);
    expect_invalid_strategy(wrong_sum, 3, 1.0);
    expect_invalid_strategy(with_nan, 3, 1.0);
    expect_invalid_strategy(with_infinity, 3, 1.0);
}

static void test_accumulation_numeric_atomicity(void) {
    InfoNode node;
    const Probability strategy[2] = {0.5, 0.5};
    const double with_infinity[2] = {7.0, INFINITY};
    const double with_negative[2] = {7.0, -1.0};

    initialize(&node, 15, 2);
    memcpy(node.strategy_sums, with_infinity, sizeof(with_infinity));
    CHECK(cfr_info_node_accumulate_strategy(&node, strategy, 2, 1.0) ==
          CFR_STATUS_NUMERIC_ERROR);
    check_unchanged(node.strategy_sums, with_infinity, 2);

    memcpy(node.strategy_sums, with_negative, sizeof(with_negative));
    CHECK(cfr_info_node_accumulate_strategy(&node, strategy, 2, 1.0) ==
          CFR_STATUS_NUMERIC_ERROR);
    check_unchanged(node.strategy_sums, with_negative, 2);
    destroy(&node);
}

static void test_average_strategy_errors_and_limits(void) {
    InfoNode node;
    InfoNode empty = {0};
    Probability output[3] = {51.0, 52.0, 53.0};

    initialize(&node, 16, 2);
    CHECK(cfr_info_node_average_strategy(NULL, output, 2) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_average_strategy(&empty, output, 2) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_average_strategy(&node, NULL, 2) ==
          CFR_STATUS_INVALID_ARGUMENT);
    CHECK(cfr_info_node_average_strategy(&node, output, 0) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(output[0] == 51.0 && output[1] == 52.0 && output[2] == 53.0);
    CHECK(cfr_info_node_average_strategy(&node, output, 1) ==
          CFR_STATUS_BUFFER_TOO_SMALL);
    CHECK(output[0] == 51.0 && output[1] == 52.0 && output[2] == 53.0);

    node.strategy_sums[0] = DBL_MAX;
    node.strategy_sums[1] = DBL_MAX / 2.0;
    CHECK(cfr_info_node_average_strategy(&node, output, 2) ==
          CFR_STATUS_SUCCESS);
    check_distribution(output, 2);
    CHECK(near(output[0], 2.0 / 3.0));
    CHECK(near(output[1], 1.0 / 3.0));

    node.strategy_sums[0] = NAN;
    output[0] = 51.0;
    output[1] = 52.0;
    CHECK(cfr_info_node_average_strategy(&node, output, 2) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(output[0] == 51.0 && output[1] == 52.0);

    node.strategy_sums[0] = INFINITY;
    CHECK(cfr_info_node_average_strategy(&node, output, 2) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(output[0] == 51.0 && output[1] == 52.0);

    node.strategy_sums[0] = -1.0;
    CHECK(cfr_info_node_average_strategy(&node, output, 2) ==
          CFR_STATUS_NUMERIC_ERROR);
    CHECK(output[0] == 51.0 && output[1] == 52.0);
    destroy(&node);
}

int test_info_node(void) {
    failures = 0;

    test_initialization_and_destruction();
#ifdef CFR_TEST_WRAP_ALLOCATOR
    test_allocation_failures();
#endif
    test_single_action();
    test_current_strategy_cases();
    test_current_strategy_numeric_limits();
    test_current_strategy_invalid_arguments();
    test_add_regret();
    test_weighted_average();
    test_deterministic_queries();
    test_generated_strategy_composition();
    test_accumulation_invalid_arguments();
    test_accumulation_numeric_atomicity();
    test_average_strategy_errors_and_limits();

#ifdef CFR_TEST_WRAP_ALLOCATOR
    CHECK(test_allocator_live_allocations() == 0);
#endif

    return failures;
}

#ifdef CFR_TEST_INFO_NODE_STANDALONE
int main(void) {
    const int result = test_info_node();

    if (result != 0) {
        fprintf(stderr, "Fallaron %d comprobaciones de InfoNode.\n", result);
        return 1;
    }

    puts("Todas las pruebas de InfoNode terminaron correctamente.");
    return 0;
}
#endif

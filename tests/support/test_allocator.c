#include "test_allocator.h"

#ifdef CFR_TEST_WRAP_ALLOCATOR

#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>

static atomic_bool failure_enabled;
static atomic_size_t allocations_before_failure;
static atomic_size_t live_allocations;

void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__wrap_malloc(size_t size);
void *__wrap_realloc(void *pointer, size_t size);
void __wrap_free(void *pointer);

static bool allocation_must_fail(void) {
    size_t remaining;

    if (!atomic_load_explicit(&failure_enabled, memory_order_acquire))
        return false;
    remaining = atomic_load_explicit(&allocations_before_failure,
                                     memory_order_relaxed);
    do {
        if (remaining == 0)
            return true;
    } while (!atomic_compare_exchange_weak_explicit(
        &allocations_before_failure, &remaining, remaining - 1,
        memory_order_relaxed, memory_order_relaxed));
    return false;
}

void *__wrap_malloc(size_t size) {
    void *result;

    if (allocation_must_fail())
        return NULL;

    result = __real_malloc(size);
    if (result != NULL)
        atomic_fetch_add_explicit(&live_allocations, 1, memory_order_relaxed);
    return result;
}

void *__wrap_realloc(void *pointer, size_t size) {
    const bool pointer_was_null = pointer == NULL;
    void *result;

    if (allocation_must_fail())
        return NULL;

    result = __real_realloc(pointer, size);
    if (pointer_was_null && result != NULL)
        atomic_fetch_add_explicit(&live_allocations, 1, memory_order_relaxed);
    return result;
}

void __wrap_free(void *pointer) {
    if (pointer != NULL)
        atomic_fetch_sub_explicit(&live_allocations, 1, memory_order_relaxed);
    __real_free(pointer);
}

void test_allocator_fail_after(size_t successful_allocations) {
    atomic_store_explicit(&allocations_before_failure, successful_allocations,
                          memory_order_relaxed);
    atomic_store_explicit(&failure_enabled, true, memory_order_release);
}

void test_allocator_disable_failures(void) {
    atomic_store_explicit(&failure_enabled, false, memory_order_release);
}

size_t test_allocator_live_allocations(void) {
    return atomic_load_explicit(&live_allocations, memory_order_relaxed);
}

#else

void test_allocator_fail_after(size_t successful_allocations) {
    (void)successful_allocations;
}

void test_allocator_disable_failures(void) {}

size_t test_allocator_live_allocations(void) { return 0; }

#endif

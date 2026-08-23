#include "test_allocator.h"

#ifdef CFR_TEST_WRAP_ALLOCATOR

#include <stdbool.h>
#include <stdlib.h>

static bool failure_enabled;
static size_t allocations_before_failure;
static size_t live_allocations;

void *__real_malloc(size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__wrap_malloc(size_t size);
void *__wrap_realloc(void *pointer, size_t size);
void __wrap_free(void *pointer);

void *__wrap_malloc(size_t size) {
    void *result;

    if (failure_enabled && allocations_before_failure == 0) {
        return NULL;
    }
    if (failure_enabled) {
        allocations_before_failure -= 1;
    }

    result = __real_malloc(size);
    if (result != NULL) {
        live_allocations += 1;
    }
    return result;
}

void *__wrap_realloc(void *pointer, size_t size) {
    const bool pointer_was_null = pointer == NULL;
    void *result;

    if (failure_enabled && allocations_before_failure == 0) {
        return NULL;
    }
    if (failure_enabled) {
        allocations_before_failure -= 1;
    }

    result = __real_realloc(pointer, size);
    if (pointer_was_null && result != NULL) {
        live_allocations += 1;
    }
    return result;
}

void __wrap_free(void *pointer) {
    if (pointer != NULL) {
        live_allocations -= 1;
    }
    __real_free(pointer);
}

void test_allocator_fail_after(size_t successful_allocations) {
    failure_enabled = true;
    allocations_before_failure = successful_allocations;
}

void test_allocator_disable_failures(void) { failure_enabled = false; }

size_t test_allocator_live_allocations(void) { return live_allocations; }

#else

void test_allocator_fail_after(size_t successful_allocations) {
    (void)successful_allocations;
}

void test_allocator_disable_failures(void) {}

size_t test_allocator_live_allocations(void) { return 0; }

#endif

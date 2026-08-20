#ifndef CFR_TEST_ALLOCATOR_H
#define CFR_TEST_ALLOCATOR_H

#include <stddef.h>

void test_allocator_fail_after(size_t successful_allocations);
void test_allocator_disable_failures(void);
size_t test_allocator_live_allocations(void);

#endif

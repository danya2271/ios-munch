#ifndef PCIE_TEST_KALLOC_H
#define PCIE_TEST_KALLOC_H

#include <stdbool.h>
#include <stdlib.h>

extern bool pcie_test_fail_allocation;
extern unsigned pcie_test_allocation_calls;
extern unsigned pcie_test_fail_allocation_at;

static inline void *
pcie_test_allocate(size_t count, size_t size)
{
	pcie_test_allocation_calls++;
	if (pcie_test_fail_allocation || pcie_test_allocation_calls == pcie_test_fail_allocation_at) {
		return NULL;
	}
	return calloc(count, size);
}

#define Z_WAITOK 1u
#define Z_ZERO 2u
#define PCIE_TEST_SELECT(first, second, third, selected, ...) selected
#define PCIE_TEST_ALLOC_ONE(type, flags) pcie_test_allocate(1, sizeof(type))
#define PCIE_TEST_ALLOC_ARRAY(type, count, flags) pcie_test_allocate(count, sizeof(type))
#define PCIE_TEST_FREE_ONE(type, memory) free(memory)
#define PCIE_TEST_FREE_ARRAY(type, count, memory) free(memory)
#define kalloc_type(...) PCIE_TEST_SELECT(__VA_ARGS__, PCIE_TEST_ALLOC_ARRAY, PCIE_TEST_ALLOC_ONE)(__VA_ARGS__)
#define kfree_type(...) PCIE_TEST_SELECT(__VA_ARGS__, PCIE_TEST_FREE_ARRAY, PCIE_TEST_FREE_ONE)(__VA_ARGS__)

#endif

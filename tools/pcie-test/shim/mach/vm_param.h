#ifndef PCIE_TEST_VM_PARAM_H
#define PCIE_TEST_VM_PARAM_H

#include <stdint.h>
#include <stddef.h>

#ifndef PCIE_TEST_NATIVE_PAGE_SHIFT
#define PCIE_TEST_NATIVE_PAGE_SHIFT 12
#endif
#define PAGE_SHIFT PCIE_TEST_NATIVE_PAGE_SHIFT
#define PAGE_SIZE ((size_t)1 << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1)
#define round_page(value) (((value) + PAGE_MASK) & ~PAGE_MASK)
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef uint32_t ppnum_t;

#endif

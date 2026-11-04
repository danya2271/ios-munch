#ifndef PCIE_TEST_VM_KERN_H
#define PCIE_TEST_VM_KERN_H

#include <mach/vm_param.h>

#define KERN_SUCCESS 0
#define KMA_KOBJECT 1u
#define KMA_ZERO 2u
#define KMA_DATA 4u
#define VM_KERN_MEMORY_IOKIT 9u
extern void *kernel_map;
int kmem_alloc(void *map, vm_offset_t *address, vm_size_t size, unsigned flags, unsigned tag);
int kernel_memory_allocate(void *map, vm_offset_t *address, vm_size_t size, vm_offset_t mask, unsigned flags, unsigned tag);
void kmem_free(void *map, vm_offset_t address, vm_size_t size);
uint64_t kvtophys(vm_offset_t address);

#endif

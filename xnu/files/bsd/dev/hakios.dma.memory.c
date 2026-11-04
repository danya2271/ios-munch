#include <kern/kalloc.h>
#include <libkern/libkern.h>
#include <mach/vm_param.h>
#include <sys/errno.h>
#include <sys/hakios.dma.memory.h>
#include <vm/memory_types.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>

extern uint64_t kvtophys(vm_offset_t address);
extern unsigned int pmap_cache_attributes(ppnum_t page);
extern void pmap_set_cache_attributes(ppnum_t page, unsigned int attributes);

struct dma_memory_page {
	ppnum_t number;
	unsigned original;
	bool changed;
};

struct dma_memory {
	vm_offset_t address;
	vm_size_t size;
	uint64_t *physical;
	struct dma_memory_page *native;
	uint32_t count, native_count;
	bool coherent, ready;
};

static void
memory_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#else
	__sync_synchronize();
#endif
}

static bool
memory_matches(const struct dma_memory *memory, const struct hakios_dma_backing *backing)
{
	return memory != NULL && backing->cpu == (void *)memory->address && backing->pages == memory->physical &&
	       backing->count == memory->count && backing->coherent == memory->coherent &&
	       backing->cacheable == !memory->coherent;
}

static void
memory_discard(struct dma_memory *memory)
{
	if (memory->address != 0) {
		kmem_free(kernel_map, memory->address, memory->size);
	}
	if (memory->physical != NULL) {
		kfree_type(uint64_t, memory->count, memory->physical);
	}
	if (memory->native != NULL) {
		kfree_type(struct dma_memory_page, memory->native_count, memory->native);
	}
	kfree_type(struct dma_memory, memory);
}

int
hakios_dma_memory_allocate(uint32_t pages, bool coherent, struct hakios_dma_backing *backing)
{
	if (backing == NULL) { return EINVAL; }
	bzero(backing, sizeof(*backing));
	if (pages == 0 || pages > HAKIOS_DMA_PT_PAGES_MAX || PAGE_SIZE < HAKIOS_DMA_PT_PAGE_SIZE ||
	    PAGE_SIZE % HAKIOS_DMA_PT_PAGE_SIZE != 0) {
		return EINVAL;
	}
	struct dma_memory *memory = kalloc_type(struct dma_memory, Z_WAITOK | Z_ZERO);
	if (memory == NULL) { return ENOMEM; }
	memory->count = pages;
	memory->size = round_page((vm_size_t)pages * HAKIOS_DMA_PT_PAGE_SIZE);
	memory->native_count = (uint32_t)(memory->size / PAGE_SIZE);
	memory->coherent = coherent;
	memory->physical = kalloc_type(uint64_t, pages, Z_WAITOK | Z_ZERO);
	memory->native = kalloc_type(struct dma_memory_page, memory->native_count, Z_WAITOK | Z_ZERO);
	if (memory->physical == NULL || memory->native == NULL) {
		memory_discard(memory);
		return ENOMEM;
	}
	vm_size_t alignment = PAGE_SIZE;
	while (coherent && alignment < memory->size) { alignment <<= 1; }
	if (kernel_memory_allocate(kernel_map, &memory->address, memory->size, alignment - 1, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		memory_discard(memory);
		return ENOMEM;
	}
	*backing = (struct hakios_dma_backing) {
		.token = memory, .cpu = (void *)memory->address, .pages = memory->physical, .count = pages,
		.coherent = coherent, .cacheable = !coherent,
	};
	for (uint32_t index = 0; index < memory->native_count; index++) {
		uint64_t physical = kvtophys(memory->address + (vm_offset_t)index * PAGE_SIZE);
		ppnum_t number = (ppnum_t)(physical >> PAGE_SHIFT);
		if (physical == 0 || (physical & PAGE_MASK) != 0 || ((uint64_t)number << PAGE_SHIFT) != physical) {
			return EFAULT;
		}
		unsigned attributes = pmap_cache_attributes(number) & VM_WIMG_MASK;
		if (attributes != VM_WIMG_DEFAULT) { return EPROTO; }
		memory->native[index] = (struct dma_memory_page) {.number = number, .original = attributes};
		for (uint32_t offset = 0; offset < PAGE_SIZE / HAKIOS_DMA_PT_PAGE_SIZE; offset++) {
			uint32_t page = index * (PAGE_SIZE / HAKIOS_DMA_PT_PAGE_SIZE) + offset;
			if (page < pages) { memory->physical[page] = physical + (uint64_t)offset * HAKIOS_DMA_PT_PAGE_SIZE; }
		}
	}
	if (coherent) {
		for (uint32_t index = 0; index < memory->native_count; index++) {
			struct dma_memory_page *page = &memory->native[index];
			page->changed = true;
			pmap_set_cache_attributes(page->number, VM_WIMG_WCOMB);
			if ((pmap_cache_attributes(page->number) & VM_WIMG_MASK) != VM_WIMG_WCOMB) { return EIO; }
		}
	}
	bzero((void *)memory->address, memory->size);
	memory_barrier();
	memory->ready = true;
	return 0;
}

int
hakios_dma_memory_sync(const struct hakios_dma_backing *backing,
    enum hakios_dma_direction direction, bool for_device)
{
	if (backing == NULL || (unsigned)direction > HAKIOS_DMA_FROM_DEVICE) { return EINVAL; }
	struct dma_memory *memory = backing->token;
	if (!memory_matches(memory, backing)) { return EINVAL; }
	if (!memory->ready) { return ESHUTDOWN; }
	unsigned expected = memory->coherent ? VM_WIMG_WCOMB : VM_WIMG_DEFAULT;
	for (uint32_t index = 0; index < memory->native_count; index++) {
		if ((pmap_cache_attributes(memory->native[index].number) & VM_WIMG_MASK) != expected) {
			memory->ready = false;
			return EIO;
		}
	}
	if (!memory->coherent) {
		int error = hakios_dma_cache_streaming(backing->cpu, memory->size, direction, for_device);
		if (error != 0) { memory->ready = false; }
		return error;
	}
	memory_barrier();
	return 0;
}

int
hakios_dma_memory_free(struct hakios_dma_backing *backing)
{
	if (backing == NULL) { return EINVAL; }
	if (backing->token == NULL) {
		return backing->cpu == NULL && backing->pages == NULL && backing->count == 0 ? 0 : EINVAL;
	}
	struct dma_memory *memory = backing->token;
	if (!memory_matches(memory, backing)) { return EINVAL; }
	memory->ready = false;
	memory_barrier();
	for (uint32_t index = 0; index < memory->native_count; index++) {
		struct dma_memory_page *page = &memory->native[index];
		if (!page->changed) {
			if (page->number != 0 && (pmap_cache_attributes(page->number) & VM_WIMG_MASK) != page->original) { return EIO; }
			continue;
		}
		pmap_set_cache_attributes(page->number, page->original);
		if ((pmap_cache_attributes(page->number) & VM_WIMG_MASK) != page->original) { return EIO; }
		page->changed = false;
	}
	memory_barrier();
	memory_discard(memory);
	bzero(backing, sizeof(*backing));
	return 0;
}

static int
memory_allocate_callback(void *owner, uint32_t pages, bool coherent, struct hakios_dma_backing *backing)
{
	(void)owner;
	return hakios_dma_memory_allocate(pages, coherent, backing);
}

static int
memory_sync_callback(void *owner, const struct hakios_dma_backing *backing,
    enum hakios_dma_direction direction, bool for_device)
{
	(void)owner;
	return hakios_dma_memory_sync(backing, direction, for_device);
}

static int
memory_free_callback(void *owner, struct hakios_dma_backing *backing)
{
	(void)owner;
	return hakios_dma_memory_free(backing);
}

int
hakios_dma_memory_create_endpoint(const struct hakios_dma_endpoint_config *config,
    const struct hakios_dma_pt_operations *tables, const struct hakios_dma_endpoint_operations *domain,
    void *owner, struct hakios_dma_endpoint **result)
{
	if (result == NULL) { return EINVAL; }
	*result = NULL;
	if (domain == NULL || domain->allocate != NULL || domain->sync != NULL || domain->free != NULL) { return EINVAL; }
	struct hakios_dma_endpoint_operations operations = *domain;
	operations.allocate = memory_allocate_callback;
	operations.sync = memory_sync_callback;
	operations.free = memory_free_callback;
	return hakios_dma_endpoint_create(config, tables, &operations, owner, result);
}

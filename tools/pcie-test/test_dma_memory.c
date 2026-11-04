#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>
#include <vm/pmap.h>
#include <sys/hakios.dma.memory.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls, pcie_test_fail_allocation_at;
void *kernel_map = (void *)1;

static struct {
	struct { void *cpu; size_t size; uint64_t physical; unsigned attrs[64]; } ranges[16];
	unsigned allocations, frees, sets, ignored_set, bad_read_after_set, syncs, holds;
	bool fail_alloc, bad_physical, bad_attribute, fenced, detached;
	int sync_error;
	void *synced;
	size_t sync_size;
	enum hakios_dma_direction direction;
	bool for_device;
} model;

int
kernel_memory_allocate(void *map, vm_offset_t *address, vm_size_t size, vm_offset_t mask, unsigned flags, unsigned tag)
{
	assert(map == kernel_map && flags == (KMA_KOBJECT | KMA_ZERO | KMA_DATA) && tag == VM_KERN_MEMORY_IOKIT);
	assert(size != 0 && size % PAGE_SIZE == 0 && size / PAGE_SIZE <= 64);
	*address = 0;
	if (model.fail_alloc) { model.fail_alloc = false; return ENOMEM; }
	unsigned index;
	for (index = 0; index < 16 && model.ranges[index].cpu != NULL; index++) {}
	assert(index < 16);
	assert(mask >= PAGE_MASK && ((mask + 1) & mask) == 0);
	model.ranges[index].cpu = aligned_alloc(mask + 1, (size + mask) & ~mask);
	assert(model.ranges[index].cpu != NULL);
	model.ranges[index].size = size;
	model.ranges[index].physical = 0x40000000ull + (uint64_t)index * 0x01000000;
	for (unsigned page = 0; page < size / PAGE_SIZE; page++) { model.ranges[index].attrs[page] = VM_WIMG_DEFAULT; }
	memset(model.ranges[index].cpu, 0, size);
	*address = (vm_offset_t)model.ranges[index].cpu;
	model.allocations++;
	return 0;
}

void
kmem_free(void *map, vm_offset_t address, vm_size_t size)
{
	assert(map == kernel_map);
	for (unsigned index = 0; index < 16; index++) {
		if (model.ranges[index].cpu != (void *)address) { continue; }
		assert(model.ranges[index].size == size);
		for (unsigned page = 0; page < size / PAGE_SIZE; page++) { assert(model.ranges[index].attrs[page] == VM_WIMG_DEFAULT); }
		free(model.ranges[index].cpu);
		model.ranges[index].cpu = NULL;
		model.frees++;
		return;
	}
	assert(false);
}

uint64_t
kvtophys(vm_offset_t address)
{
	if (model.bad_physical) { model.bad_physical = false; return 7; }
	for (unsigned index = 0; index < 16; index++) {
		uintptr_t base = (uintptr_t)model.ranges[index].cpu;
		if (base == 0 || address < base || address - base >= model.ranges[index].size) { continue; }
		uintptr_t offset = address - base;
		return model.ranges[index].physical + (offset / PAGE_SIZE) * 2 * PAGE_SIZE + offset % PAGE_SIZE;
	}
	assert(false);
	return 0;
}

static unsigned *attributes(ppnum_t number)
{
	uint64_t physical = (uint64_t)number << PAGE_SHIFT;
	for (unsigned index = 0; index < 16; index++) {
		if (model.ranges[index].cpu == NULL || physical < model.ranges[index].physical) { continue; }
		uint64_t offset = physical - model.ranges[index].physical;
		if (offset % (2 * PAGE_SIZE) == 0 && offset / (2 * PAGE_SIZE) < model.ranges[index].size / PAGE_SIZE) {
			return &model.ranges[index].attrs[offset / (2 * PAGE_SIZE)];
		}
	}
	assert(false);
	return NULL;
}

unsigned pmap_cache_attributes(ppnum_t number)
{
	if (model.bad_attribute || (model.bad_read_after_set != 0 && model.bad_read_after_set == model.sets)) {
		model.bad_attribute = false;
		model.bad_read_after_set = 0;
		return 0;
	}
	return *attributes(number);
}
void pmap_set_cache_attributes(ppnum_t number, unsigned value)
{
	assert(value == VM_WIMG_WCOMB || value == VM_WIMG_DEFAULT);
	model.sets++;
	if (model.sets != model.ignored_set) { *attributes(number) = value; }
}

int hakios_dma_cache_streaming(void *cpu, size_t size, enum hakios_dma_direction direction, bool for_device)
{
	assert(cpu != NULL && (uintptr_t)cpu % PAGE_SIZE == 0 && size % PAGE_SIZE == 0);
	model.syncs++;
	model.synced = cpu;
	model.sync_size = size;
	model.direction = direction;
	model.for_device = for_device;
	return model.sync_error;
}

int hakios_pcie_endpoint_status(hakios_pcie_lease_t lease) { return lease == 7 ? 0 : ESTALE; }
int hakios_pcie_endpoint_hold(hakios_pcie_lease_t lease) { assert(lease == 7); model.holds++; return 0; }
int hakios_pcie_endpoint_drop(hakios_pcie_lease_t lease) { assert(lease == 7 && model.holds != 0); model.holds--; return 0; }
int hakios_pcie_endpoint_resources(hakios_pcie_lease_t lease, struct hakios_pcie_endpoint_resources *resources)
{
	*resources = (struct hakios_pcie_endpoint_resources) {.domain = 0, .requester_id = 0x100, .stream_id = 0x1c01};
	return hakios_pcie_endpoint_status(lease);
}

static int attach(void *owner, hakios_pcie_lease_t lease, const struct hakios_pcie_endpoint_resources *resources,
    const struct hakios_dma_pt_info *info)
{ assert(owner == &model && lease == 7 && resources->stream_id == 0x1c01 && info->granule == 4096); return 0; }
static int fence(void *owner) { assert(owner == &model); model.fenced = true; return 0; }
static int detach(void *owner) { assert(owner == &model && model.fenced); model.detached = true; return 0; }
static int table_sync(void *owner, const struct hakios_dma_pt_page *page)
{ assert(owner == &model && page->entries != NULL); return 0; }
static int invalidate(void *owner, uint64_t iova, uint64_t size)
{ assert(owner == &model && iova >= 4096 && size != 0); return 0; }
static int quiesce(void *owner) { assert(owner == &model && model.detached); return 0; }

static void test_endpoint(void)
{
	struct hakios_dma_backing tables;
	assert(hakios_dma_memory_allocate(8, false, &tables) == 0);
	struct hakios_dma_pt_page pages[8];
	for (unsigned index = 0; index < 8; index++) {
		pages[index] = (struct hakios_dma_pt_page) {.entries = (void *)((uintptr_t)tables.cpu + index * 4096),
		    .physical = tables.pages[index]};
	}
	struct hakios_dma_endpoint_config config = {.endpoint = 7, .mask = 0xffffffff,
	    .space = {.iova_base = 4096, .iova_limit = 0x10000000,
	    .tables = {.pages = pages, .table_count = 8, .output_bits = 40, .nonsecure = true}}};
	const struct hakios_dma_endpoint_operations domain = {.version = HAKIOS_DMA_ENDPOINT_VERSION,
	    .attach = attach, .fence = fence, .detach = detach};
	const struct hakios_dma_pt_operations operations = {.version = HAKIOS_DMA_PT_VERSION,
	    .sync = table_sync, .invalidate = invalidate, .quiesce = quiesce};
	struct hakios_dma_endpoint *endpoint;
	assert(hakios_dma_memory_create_endpoint(&config, &operations, NULL, &model, &endpoint) == EINVAL && endpoint == NULL);
	assert(hakios_dma_memory_create_endpoint(&config, &operations, &domain, &model, &endpoint) == 0 && model.holds == 1);
	hakios_dma_lease_t lease;
	void *cpu;
	uint64_t iova;
	assert(hakios_dma_endpoint_alloc(endpoint, 8193, &lease, &cpu, &iova) == 0 && iova == 4096);
	memset(cpu, 0xa5, 8193);
	assert(hakios_dma_endpoint_unmap(endpoint, &lease) == 0 && lease == 0);
	assert(hakios_dma_endpoint_alloc_aligned(endpoint, 65536, 65536, &lease, &cpu, &iova) == 0);
	assert((uintptr_t)cpu % 65536 == 0 && iova % 65536 == 0);
	assert(hakios_dma_endpoint_hold(endpoint) == 0);
	assert(hakios_dma_endpoint_destroy(&endpoint) == EBUSY && endpoint != NULL && !model.fenced);
	struct hakios_dma_endpoint_status status;
	assert(hakios_dma_endpoint_status(endpoint, &status) == 0 && !status.closed && status.fault == 0);
	assert(hakios_dma_endpoint_drop(endpoint) == 0);
	assert(hakios_dma_endpoint_drop(endpoint) == EINVAL);
	assert(hakios_dma_endpoint_unmap(endpoint, &lease) == 0);
	uint8_t source[2051];
	memset(source, 0x34, sizeof(source));
	assert(hakios_dma_endpoint_map(endpoint, source, sizeof(source), HAKIOS_DMA_BIDIRECTIONAL, &lease, &iova) == 0);
	assert(model.for_device && memcmp(model.synced, source, sizeof(source)) == 0);
	memset(model.synced, 0x8b, sizeof(source));
	assert(hakios_dma_endpoint_sync(endpoint, lease, false) == 0);
	for (unsigned index = 0; index < sizeof(source); index++) { assert(source[index] == 0x8b); }
	assert(hakios_dma_endpoint_destroy(&endpoint) == 0 && model.holds == 0 && model.detached);
	assert(hakios_dma_memory_free(&tables) == 0);
}

static void test_geometry(void)
{
	for (unsigned coherent = 0; coherent < 2; coherent++) {
		struct hakios_dma_backing backing;
		assert(hakios_dma_memory_allocate(5, coherent != 0, &backing) == 0);
		assert(backing.count == 5 && backing.coherent == (coherent != 0) && backing.cacheable == (coherent == 0));
		for (unsigned index = 0; index < 5; index++) {
			assert(backing.pages[index] == kvtophys((uintptr_t)backing.cpu + index * 4096));
		}
		for (size_t index = 0; index < round_page(5 * 4096); index++) { assert(((uint8_t *)backing.cpu)[index] == 0); }
		struct hakios_dma_backing invalid = backing;
		invalid.count++;
		assert(hakios_dma_memory_sync(&invalid, HAKIOS_DMA_TO_DEVICE, true) == EINVAL);
		assert(hakios_dma_memory_free(&invalid) == EINVAL);
		for (unsigned direction = 0; direction < 3; direction++) {
			assert(hakios_dma_memory_sync(&backing, direction, true) == 0);
			assert(hakios_dma_memory_sync(&backing, direction, false) == 0);
			if (!coherent) {
				assert(model.direction == direction && !model.for_device && model.sync_size == round_page(5 * 4096));
			}
		}
		assert(hakios_dma_memory_free(&backing) == 0 && backing.token == NULL && backing.cpu == NULL);
		assert(hakios_dma_memory_free(&backing) == 0);
	}
}

static void test_failures(void)
{
	struct hakios_dma_backing backing;
	assert(hakios_dma_memory_allocate(0, true, &backing) == EINVAL);
	assert(hakios_dma_memory_allocate(HAKIOS_DMA_PT_PAGES_MAX + 1, true, &backing) == EINVAL);
	for (unsigned allocation = 1; allocation <= 3; allocation++) {
		pcie_test_fail_allocation_at = pcie_test_allocation_calls + allocation;
		assert(hakios_dma_memory_allocate(1, true, &backing) == ENOMEM && backing.token == NULL);
		pcie_test_fail_allocation_at = 0;
	}
	model.fail_alloc = true;
	assert(hakios_dma_memory_allocate(1, true, &backing) == ENOMEM && backing.token == NULL);
	model.bad_physical = true;
	assert(hakios_dma_memory_allocate(1, true, &backing) == EFAULT && backing.token != NULL);
	assert(hakios_dma_memory_sync(&backing, HAKIOS_DMA_TO_DEVICE, true) == ESHUTDOWN);
	assert(hakios_dma_memory_free(&backing) == 0);
	model.bad_attribute = true;
	assert(hakios_dma_memory_allocate(1, true, &backing) == EPROTO && backing.token != NULL);
	assert(hakios_dma_memory_free(&backing) == 0);
	for (unsigned after = 0; after < 2; after++) {
		for (unsigned page = 1; page <= 3; page++) {
			unsigned *failure = after ? &model.bad_read_after_set : &model.ignored_set;
			*failure = model.sets + page;
			assert(hakios_dma_memory_allocate(3 * PAGE_SIZE / 4096, true, &backing) == EIO && backing.token != NULL);
			assert(hakios_dma_memory_free(&backing) == 0);
			assert(hakios_dma_memory_allocate(3 * PAGE_SIZE / 4096, true, &backing) == 0);
			*failure = model.sets + page;
			unsigned before = model.frees;
			assert(hakios_dma_memory_free(&backing) == EIO && backing.token != NULL && model.frees == before);
			assert(hakios_dma_memory_sync(&backing, HAKIOS_DMA_TO_DEVICE, true) == ESHUTDOWN);
			assert(hakios_dma_memory_free(&backing) == 0);
		}
	}
	assert(hakios_dma_memory_allocate(1, true, &backing) == 0);
	model.bad_attribute = true;
	assert(hakios_dma_memory_sync(&backing, HAKIOS_DMA_TO_DEVICE, true) == EIO);
	assert(hakios_dma_memory_free(&backing) == 0);
	assert(hakios_dma_memory_allocate(1, false, &backing) == 0);
	model.sync_error = EIO;
	assert(hakios_dma_memory_sync(&backing, HAKIOS_DMA_FROM_DEVICE, false) == EIO);
	model.sync_error = 0;
	assert(hakios_dma_memory_sync(&backing, HAKIOS_DMA_FROM_DEVICE, false) == ESHUTDOWN);
	assert(hakios_dma_memory_free(&backing) == 0);
	assert(hakios_dma_memory_allocate(1, false, &backing) == 0);
	ppnum_t number = (ppnum_t)(backing.pages[0] >> PAGE_SHIFT);
	pmap_set_cache_attributes(number, VM_WIMG_WCOMB);
	assert(hakios_dma_memory_sync(&backing, HAKIOS_DMA_FROM_DEVICE, false) == EIO);
	assert(hakios_dma_memory_free(&backing) == EIO && backing.token != NULL);
	pmap_set_cache_attributes(number, VM_WIMG_DEFAULT);
	assert(hakios_dma_memory_free(&backing) == 0);
}

int main(void)
{
	test_geometry();
	test_failures();
	test_endpoint();
	assert(model.allocations == model.frees);
	printf("dma-memory-test: PASS (%zu-byte VM pages, wired/coherent backing, retained restoration, real endpoint integration)\n", PAGE_SIZE);
	return 0;
}

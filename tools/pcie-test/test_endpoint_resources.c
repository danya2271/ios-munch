#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/hakios.dma.endpoint.h>
#include <sys/hakios.msi.pool.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;

enum fail_site { NONE, ATTACH, ALLOCATE, SYNC, TABLE_SYNC, INVALIDATE, FREE, FENCE, DETACH, QUIESCE,
	IRQ_CLAIM, IRQ_MAP, IRQ_QUIET, IRQ_UNMAP, IRQ_RELEASE };

struct memory {
	struct memory *next;
	void *cpu;
	void *device;
	uint64_t *pages;
	uint32_t count;
	bool owned;
};

struct model {
	struct hakios_dma_endpoint *dma;
	struct hakios_dma_pt_page tables[8];
	struct hakios_dma_endpoint_config config;
	struct memory *memory;
	uint64_t next_physical;
	unsigned allocated, freed;
	enum fail_site fail;
	int error;
	bool after, attached, fenced, detached;
	bool fake_coherent;
	uint32_t irq_claims;
	uint32_t irq_quiet;
	struct hakios_msi_route routes[64];
	unsigned tokens[64];
	struct hakios_msi_pool *pool;
};

static uint32_t stream_id = 0x1c01;
static unsigned endpoint_holds;

int hakios_pcie_endpoint_hold(hakios_pcie_lease_t lease)
{ assert(lease == 7); endpoint_holds++; return 0; }
int hakios_pcie_endpoint_drop(hakios_pcie_lease_t lease)
{ assert(lease == 7 && endpoint_holds != 0); endpoint_holds--; return 0; }

int
hakios_pcie_endpoint_status(hakios_pcie_lease_t lease)
{
	return lease == 7 ? 0 : ESTALE;
}

int
hakios_pcie_endpoint_resources(hakios_pcie_lease_t lease, struct hakios_pcie_endpoint_resources *resources)
{
	*resources = (struct hakios_pcie_endpoint_resources) {
		.domain = 0, .requester_id = 0x100, .stream_id = stream_id,
		.bar0_address = 0x60300000, .bar0_size = 0x100000,
	};
	return hakios_pcie_endpoint_status(lease);
}

static int
fail(struct model *model, enum fail_site site)
{
	if (model->fail != site) {
		return 0;
	}
	model->fail = NONE;
	return model->error;
}

static int
attach(void *owner, hakios_pcie_lease_t endpoint, const struct hakios_pcie_endpoint_resources *resources,
    const struct hakios_dma_pt_info *tables)
{
	struct model *model = owner;
	assert(endpoint == 7 && resources->stream_id == 0x1c01 && tables->root == model->tables[0].physical);
	assert(tables->input_bits == 39 && tables->granule == 4096 && tables->start_level == 1);
	model->attached = true;
	return fail(model, ATTACH);
}

static int
allocate(void *owner, uint32_t pages, bool coherent, struct hakios_dma_backing *backing)
{
	struct model *model = owner;
	int error = fail(model, ALLOCATE);
	if (error != 0 && !model->after) {
		return error;
	}
	struct memory *memory = calloc(1, sizeof(*memory));
	assert(memory != NULL);
	memory->cpu = aligned_alloc(4096, (size_t)pages * 4096);
	memory->device = aligned_alloc(4096, (size_t)pages * 4096);
	memory->pages = calloc(pages, sizeof(uint64_t));
	assert(memory->cpu != NULL && memory->device != NULL && memory->pages != NULL);
	memory->count = pages;
	memset(memory->cpu, 0x7e, (size_t)pages * 4096);
	for (uint32_t index = 0; index < pages; index++) {
		memory->pages[index] = model->next_physical;
		model->next_physical += 8192;
	}
	memory->next = model->memory;
	model->memory = memory;
	model->allocated++;
	*backing = (struct hakios_dma_backing) {
		.token = memory, .cpu = memory->cpu, .pages = memory->pages, .count = pages,
		.coherent = coherent && !model->fake_coherent, .cacheable = !coherent,
	};
	return error;
}

static int
sync_buffer(void *owner, const struct hakios_dma_backing *backing, enum hakios_dma_direction direction, bool for_device)
{
	struct model *model = owner;
	struct memory *memory = backing->token;
	int error = fail(model, SYNC);
	assert(memory != NULL && (unsigned)direction <= HAKIOS_DMA_FROM_DEVICE);
	if (error == 0 || model->after) {
		if (for_device) {
			memcpy(memory->device, memory->cpu, (size_t)memory->count * 4096);
		} else {
			memcpy(memory->cpu, memory->device, (size_t)memory->count * 4096);
		}
		memory->owned = for_device;
	}
	return error;
}

static int
free_buffer(void *owner, struct hakios_dma_backing *backing)
{
	struct model *model = owner;
	int error = fail(model, FREE);
	if (backing->token == NULL || (error != 0 && !model->after)) {
		return error;
	}
	struct memory *memory = backing->token;
	assert(model->fenced || !memory->owned);
	struct memory **position = &model->memory;
	while (*position != memory) {
		position = &(*position)->next;
	}
	*position = memory->next;
	free(memory->cpu);
	free(memory->device);
	free(memory->pages);
	free(memory);
	memset(backing, 0, sizeof(*backing));
	model->freed++;
	return error;
}

static int
fence(void *owner)
{
	struct model *model = owner;
	int error = fail(model, FENCE);
	if (error == 0 || model->after) {
		model->fenced = true;
	}
	return error;
}

static int
detach(void *owner)
{
	struct model *model = owner;
	assert(model->fenced);
	int error = fail(model, DETACH);
	if (error == 0 || model->after) {
		model->detached = true;
	}
	return error;
}

static int table_sync(void *owner, const struct hakios_dma_pt_page *page)
{ assert(page->entries != NULL); return fail(owner, TABLE_SYNC); }
static int invalidate(void *owner, uint64_t iova, uint64_t size)
{ assert(iova >= 4096 && size != 0); return fail(owner, INVALIDATE); }
static int quiesce(void *owner)
{ struct model *model = owner; assert(model->fenced && model->detached); return fail(owner, QUIESCE); }

static const struct hakios_dma_pt_operations table_ops = {
	.version = HAKIOS_DMA_PT_VERSION, .sync = table_sync, .invalidate = invalidate, .quiesce = quiesce,
};
static const struct hakios_dma_endpoint_operations dma_ops = {
	.version = HAKIOS_DMA_ENDPOINT_VERSION, .attach = attach, .allocate = allocate, .sync = sync_buffer,
	.free = free_buffer, .fence = fence, .detach = detach,
};

static void
model_init(struct model *model)
{
	memset(model, 0, sizeof(*model));
	model->error = EIO;
	model->next_physical = 0x90000000;
	for (unsigned index = 0; index < 8; index++) {
		model->tables[index].entries = aligned_alloc(4096, 4096);
		assert(model->tables[index].entries != NULL);
		model->tables[index].physical = 0x80000000 + index * 4096;
	}
	model->config = (struct hakios_dma_endpoint_config) {
		.endpoint = 7, .mask = 0xffffffff,
		.space = {.iova_base = 0x10000000, .iova_limit = 0x14000000,
			.tables = {.pages = model->tables, .table_count = 8, .output_bits = 40, .nonsecure = true}},
	};
}

static void
model_free(struct model *model)
{
	model->fail = NONE;
	assert(hakios_dma_endpoint_destroy(&model->dma) == 0 && model->dma == NULL);
	assert(model->allocated == model->freed && model->memory == NULL && model->detached);
	assert(endpoint_holds == 0);
	for (unsigned index = 0; index < 8; index++) {
		free(model->tables[index].entries);
	}
}

static uint64_t
leaf(struct model *model, uint64_t iova)
{
	uint64_t *entries = model->tables[0].entries;
	const unsigned shifts[] = {30, 21, 12};
	for (unsigned level = 0; level < 3; level++) {
		uint64_t descriptor = entries[(iova >> shifts[level]) & 511];
		if (level == 2 || descriptor == 0) { return descriptor; }
		uint64_t physical = descriptor & 0x0000fffffffff000ull;
		unsigned index;
		for (index = 0; index < 8; index++) {
			if (model->tables[index].physical == physical) { entries = model->tables[index].entries; break; }
		}
		assert(index < 8);
	}
	abort();
}

static void
test_dma(void)
{
	for (unsigned direction = 0; direction <= HAKIOS_DMA_FROM_DEVICE; direction++) {
		struct model model;
		model_init(&model);
		assert(hakios_dma_endpoint_create(&model.config, &table_ops, &dma_ops, &model, &model.dma) == 0);
		uint8_t cpu[5001];
		memset(cpu, 0x39, sizeof(cpu));
		hakios_dma_lease_t lease;
		uint64_t iova;
		assert(hakios_dma_endpoint_map(model.dma, cpu, sizeof(cpu), direction, &lease, &iova) == 0);
		assert(iova >= 0x10000000 && iova < 0x14000000 && model.memory->count == 2);
		uint64_t pte = leaf(&model, iova);
		assert((pte & 0x0000fffffffff000ull) == model.memory->pages[0] && ((pte >> 2) & 7) == 1);
		assert(((pte & (1ull << 7)) != 0) == (direction == HAKIOS_DMA_TO_DEVICE));
		assert((leaf(&model, iova + 4096) & 0x0000fffffffff000ull) == model.memory->pages[1]);
		uint8_t *device = model.memory->device;
		assert(device[0] == (direction == HAKIOS_DMA_FROM_DEVICE ? 0 : 0x39));
		assert(device[5001] == 0 && device[8191] == 0);
		assert(hakios_dma_endpoint_sync(model.dma, lease, true) == EALREADY);
		memset(device, 0x52, sizeof(cpu));
		assert(hakios_dma_endpoint_sync(model.dma, lease, false) == 0);
		assert(cpu[0] == (direction == HAKIOS_DMA_TO_DEVICE ? 0x39 : 0x52));
		cpu[0] = 0x19;
		assert(hakios_dma_endpoint_sync(model.dma, lease, true) == 0);
		if (direction != HAKIOS_DMA_FROM_DEVICE) {
			assert(device[0] == 0x19);
		}
		hakios_dma_lease_t stale = lease;
		assert(hakios_dma_endpoint_unmap(model.dma, &lease) == 0 && lease == 0);
		assert(leaf(&model, iova) == 0);
		assert(hakios_dma_endpoint_sync(model.dma, stale, false) == ESTALE);
		void *coherent;
		assert(hakios_dma_endpoint_alloc(model.dma, 4096, &lease, &coherent, &iova) == 0);
		assert(coherent != NULL && model.memory->pages[0] != iova);
		assert(((leaf(&model, iova) >> 2) & 7) == 0);
		assert(hakios_dma_endpoint_close(model.dma) == 0);
		assert(hakios_dma_endpoint_sync(model.dma, lease, false) == ESHUTDOWN);
		model_free(&model);
	}
}

static void
test_rejections(void)
{
	struct model model;
	model_init(&model);
	stream_id = 0x1c00;
	assert(hakios_dma_endpoint_create(&model.config, &table_ops, &dma_ops, &model, &model.dma) == ENODEV);
	assert(model.dma == NULL && !model.attached && endpoint_holds == 0);
	stream_id = 0x1c01;
	assert(hakios_dma_endpoint_create(&model.config, &table_ops, &dma_ops, &model, &model.dma) == 0);
	model.fake_coherent = true;
	hakios_dma_lease_t lease;
	uint64_t iova;
	void *cpu;
	assert(hakios_dma_endpoint_alloc(model.dma, 64, &lease, &cpu, &iova) == EPROTO);
	assert(lease == 0 && cpu == NULL && iova == 0);
	model_free(&model);
#if !defined(__aarch64__)
	void *pages = aligned_alloc(4096, 4096);
	assert(pages != NULL);
	assert(hakios_dma_cache_streaming(pages, 4096, HAKIOS_DMA_FROM_DEVICE, true) == ENOTSUP);
	assert(hakios_dma_cache_streaming((uint8_t *)pages + 1, 4096, HAKIOS_DMA_FROM_DEVICE, true) == EINVAL);
	free(pages);
#endif
}

static void
test_dma_failures(void)
{
	unsigned failures = 0;
	for (unsigned after = 0; after < 2; after++) {
		for (unsigned negative = 0; negative < 2; negative++) {
			for (unsigned site = ATTACH; site <= QUIESCE; site++) {
				struct model model;
				model_init(&model);
				model.after = after != 0;
				model.error = negative != 0 ? -EIO : EIO;
				if (site == ATTACH) {
					model.fail = site;
				}
				int error = hakios_dma_endpoint_create(&model.config, &table_ops, &dma_ops, &model, &model.dma);
				if (site == ATTACH) {
					assert(error == EIO && model.dma != NULL);
				} else {
					assert(error == 0);
					uint8_t cpu[64] = {1};
					hakios_dma_lease_t lease;
					uint64_t iova;
					if (site <= INVALIDATE) {
						model.fail = site;
					}
					error = hakios_dma_endpoint_map(model.dma, cpu, sizeof(cpu), HAKIOS_DMA_FROM_DEVICE, &lease, &iova);
					if (site <= INVALIDATE) {
						assert(error == EIO && lease == 0 && iova == 0);
					} else {
						assert(error == 0);
						model.fail = site;
						if (site == FREE) {
							assert(hakios_dma_endpoint_unmap(model.dma, &lease) == EIO && lease != 0);
						} else {
							assert(hakios_dma_endpoint_destroy(&model.dma) == EIO && model.dma != NULL);
						}
					}
				}
				struct hakios_dma_endpoint_status status;
				assert(hakios_dma_endpoint_status(model.dma, &status) == 0 && status.fault == EIO);
				model_free(&model);
				failures++;
			}
		}
	}
	printf("endpoint-dma failures: %u\n", failures);
}

static unsigned route_bank(const struct hakios_msi_route *routes) { return (routes[0].intid - 800) / 32; }
static int irq_claim(void *owner, hakios_pcie_lease_t endpoint, const struct hakios_msi_route *routes, uint32_t count)
{
	struct model *model = owner;
	assert(endpoint == 7 && count == 32);
	unsigned bit = 1u << route_bank(routes);
	assert((model->irq_claims & bit) == 0);
	model->irq_claims |= bit;
	return fail(model, IRQ_CLAIM);
}
static int irq_map(void *owner, hakios_pcie_lease_t endpoint, uint64_t physical, void **mapping, uint64_t *iova)
{
	assert(endpoint == 7 && physical == 0x17a10040);
	*mapping = malloc(1);
	assert(*mapping != NULL);
	*iova = 0x13000040;
	return fail(owner, IRQ_MAP);
}
static int irq_quiet(void *owner, const struct hakios_msi_route *routes, uint32_t count)
{
	struct model *model = owner;
	assert(count == 32);
	model->irq_quiet |= 1u << route_bank(routes);
	return fail(model, IRQ_QUIET);
}
static int irq_unmap(void *owner, void **mapping)
{
	struct model *model = owner;
	assert(model->irq_quiet != 0);
	int error = fail(model, IRQ_UNMAP);
	if (error == 0 || model->after) {
		free(*mapping);
		*mapping = NULL;
	}
	return error;
}
static int irq_release(void *owner, const struct hakios_msi_route *routes, uint32_t count)
{
	struct model *model = owner;
	unsigned bit = 1u << route_bank(routes);
	assert(count == 32 && (model->irq_quiet & bit) != 0);
	model->irq_claims &= ~bit;
	return fail(model, IRQ_RELEASE);
}
static const struct hakios_msi_pool_operations irq_ops = {
	.version = HAKIOS_MSI_POOL_VERSION, .claim = irq_claim, .map = irq_map, .quiesce = irq_quiet,
	.unmap = irq_unmap, .release = irq_release,
};

static void
test_msi_pool(void)
{
	unsigned failures = 0;
	for (unsigned site = NONE; site <= IRQ_RELEASE; site++) {
		if (site != NONE && site < IRQ_CLAIM) {
			continue;
		}
		struct model model = {0};
		model.error = EIO;
		for (unsigned index = 0; index < 64; index++) {
			model.routes[index] = (struct hakios_msi_route) {&model.tokens[index], 800 + index, 832 + index};
		}
		struct hakios_msi_pool_config config = {model.routes, 64, 768, 0x17a10040};
		assert(hakios_msi_pool_create(&config, &irq_ops, &model, &model.pool) == 0);
		struct hakios_msi_allocation *first, *second, *exhausted;
		if (site == IRQ_CLAIM || site == IRQ_MAP) {
			model.fail = site;
		}
		int error = hakios_msi_pool_allocate(model.pool, 7, &first);
		assert(error == ((site == IRQ_CLAIM || site == IRQ_MAP) ? EIO : 0));
		assert(first != NULL);
		assert(hakios_msi_pool_allocate(model.pool, 7, &second) == 0);
		assert(hakios_msi_pool_allocate(model.pool, 7, &exhausted) == ENOSPC && exhausted == NULL);
		assert(hakios_msi_pool_destroy(&model.pool) == EBUSY);
		struct hakios_msi_config msi;
		assert(hakios_msi_allocation_config(second, 8, 0x50, &msi) == ESTALE && msi.doorbell_iova == 0);
		assert(hakios_msi_allocation_config(second, 7, 0x50, &msi) == 0);
		assert(msi.data == 864 && msi.doorbell_iova == 0x13000040 && msi.intids[31] == 863);
		if (site >= IRQ_QUIET) {
			model.fail = site;
			assert(hakios_msi_allocation_destroy(&first) == EIO && first != NULL);
			assert(hakios_msi_pool_allocate(model.pool, 7, &exhausted) == ENOSPC);
		}
		assert(hakios_msi_allocation_destroy(&first) == 0 && first == NULL);
		assert(hakios_msi_allocation_destroy(&second) == 0 && second == NULL);
		assert(model.irq_claims == 0);
		assert(hakios_msi_pool_destroy(&model.pool) == 0);
		assert(endpoint_holds == 0);
		model.routes[0].data = 831;
		assert(hakios_msi_pool_create(&config, &irq_ops, &model, &model.pool) == EINVAL && model.pool == NULL);
		if (site != NONE) {
			failures++;
		}
	}
	printf("msi-pool failures: %u\n", failures);
}

int
main(void)
{
	test_dma();
	test_rejections();
	test_dma_failures();
	test_msi_pool();
	puts("endpoint-resources-test: PASS (bounce ownership, real PTE/IOVA engine, retained failures, granted MSI banks)");
	return 0;
}

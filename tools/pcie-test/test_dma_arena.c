#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/dma-mapping.h>
#include <linux/skbuff.h>
#include <sys/hakios.dma.arena.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls, pcie_test_fail_allocation_at;
struct hakios_dma_endpoint {
	void *memory;
	size_t size;
	unsigned holds, calls;
	bool closed, fenced, fail;
};
static struct device device;
static const uint64_t base = 0x20000000;

int hakios_dma_endpoint_hold(struct hakios_dma_endpoint *endpoint)
{ endpoint->calls++; endpoint->holds++; return 0; }
int hakios_dma_endpoint_drop(struct hakios_dma_endpoint *endpoint)
{ endpoint->calls++; assert(endpoint->holds != 0); endpoint->holds--; return 0; }
int hakios_dma_endpoint_status(struct hakios_dma_endpoint *endpoint, struct hakios_dma_endpoint_status *status)
{
	endpoint->calls++;
	*status = (struct hakios_dma_endpoint_status) {.closed = endpoint->closed, .fenced = endpoint->fenced};
	return 0;
}
int hakios_dma_endpoint_alloc_aligned(struct hakios_dma_endpoint *endpoint, size_t size, uint64_t alignment,
    hakios_dma_lease_t *lease, void **cpu, uint64_t *dma)
{
	endpoint->calls++;
	assert(endpoint->memory == NULL && alignment == size && (alignment & (alignment - 1)) == 0);
	endpoint->memory = aligned_alloc(alignment, size);
	assert(endpoint->memory != NULL);
	endpoint->size = size;
	memset(endpoint->memory, 0, size);
	if (endpoint->fail) { return EIO; }
	*cpu = endpoint->memory; *dma = base; *lease = 1;
	return 0;
}

static struct hakios_dma_arena *create(struct hakios_dma_endpoint *endpoint)
{
	memset(endpoint, 0, sizeof(*endpoint));
	const struct hakios_dma_arena_config config = {.dma_bytes = 65536, .cpu_bytes = 65536};
	struct hakios_dma_arena *arena;
	assert(hakios_dma_arena_create(endpoint, &config, &arena) == 0 && endpoint->holds == 1);
	assert(hakios_dma_arena_bind(arena, &device) == 0);
	assert(hakios_dma_arena_bind(arena, &device) == EBUSY);
	return arena;
}
static void destroy(struct hakios_dma_endpoint *endpoint, struct hakios_dma_arena **arena)
{
	assert(hakios_dma_arena_close(*arena) == 0);
	assert(hakios_dma_arena_destroy(arena) == EBUSY && *arena != NULL);
	endpoint->closed = endpoint->fenced = true;
	assert(hakios_dma_arena_destroy(arena) == 0 && *arena == NULL && endpoint->holds == 0);
	assert(hakios_dma_arena_get(&device) == NULL);
	free(endpoint->memory);
}
static struct hakios_dma_arena_status status(struct hakios_dma_arena *arena)
{ struct hakios_dma_arena_status result; assert(hakios_dma_arena_status(arena, &result) == 0); return result; }

static void test_ranges(void)
{
	struct hakios_dma_endpoint endpoint;
	struct hakios_dma_arena *arena = create(&endpoint);
	unsigned allocations = pcie_test_allocation_calls, calls = endpoint.calls;
	uint8_t source[43];
	memset(source, 0x31, sizeof(source));
	dma_addr_t dma = dma_map_single(&device, source, sizeof(source), DMA_BIDIRECTIONAL);
	assert(!dma_mapping_error(&device, dma));
	uint8_t *hardware = (uint8_t *)endpoint.memory + dma - base;
	assert(memcmp(hardware, source, sizeof(source)) == 0);
	memset(hardware, 0x67, sizeof(source));
	dma_sync_single_range_for_cpu(&device, dma, 5, 17, DMA_BIDIRECTIONAL);
	for (unsigned index = 0; index < sizeof(source); index++) { assert(source[index] == (index >= 5 && index < 22 ? 0x67 : 0x31)); }
	memset(source + 5, 0x91, 17);
	dma_sync_single_range_for_cpu(&device, dma, 5, 17, DMA_BIDIRECTIONAL);
	assert(source[5] == 0x91);
	dma_sync_single_range_for_device(&device, dma, 8, 4, DMA_BIDIRECTIONAL);
	for (unsigned index = 0; index < sizeof(source); index++) { assert(hardware[index] == (index >= 8 && index < 12 ? 0x91 : 0x67)); }
	memset(hardware + 8, 0xa3, 4);
	dma_unmap_single(&device, dma, sizeof(source), DMA_BIDIRECTIONAL);
	for (unsigned index = 0; index < sizeof(source); index++) {
		assert(source[index] == (index >= 8 && index < 12 ? 0xa3 : index >= 5 && index < 22 ? 0x91 : 0x67));
	}
	assert(status(arena).fault == 0 && status(arena).streaming == 0);
	assert(pcie_test_allocation_calls == allocations && endpoint.calls == calls);
	destroy(&endpoint, &arena);
}

static void test_coherent_and_skb(void)
{
	struct hakios_dma_endpoint endpoint;
	struct hakios_dma_arena *arena = create(&endpoint);
	unsigned allocations = pcie_test_allocation_calls, calls = endpoint.calls;
	dma_addr_t first, second;
	void *memory = dma_alloc_coherent(&device, 4097, &first, GFP_ATOMIC);
	assert(memory != NULL && (uintptr_t)memory % 8192 == 0 && first % 8192 == 0);
	memset(memory, 0x4a, 4097);
	dma_free_coherent(&device, 4097, memory, first);
	assert(status(arena).retired == 1 && status(arena).coherent == 0);
	memory = dma_alloc_noncoherent(&device, 4097, &second, DMA_TO_DEVICE, GFP_ATOMIC);
	assert(memory != NULL && second != first);
	dma_sync_single_for_device(&device, second + 3, 4000, DMA_TO_DEVICE);
	dma_free_noncoherent(&device, 4097, memory, second, DMA_TO_DEVICE);
	struct sk_buff *skb = linuxkpi_dev_alloc_skb(2048, GFP_ATOMIC);
	assert(skb != NULL && (uintptr_t)skb % 64 == 0 && skb->data - skb->head == NET_SKB_PAD && skb->end - skb->data == 2048);
	assert(skb->next == skb && skb->prev == skb && status(arena).cpu == 1);
	skb_get(skb);
	linuxkpi_kfree_skb(skb);
	assert(status(arena).cpu == 1);
	memset(skb->data, 0x42, 2048);
	dma_addr_t dma = dma_map_single(&device, skb->data, 2048, DMA_FROM_DEVICE);
	assert(dma != DMA_MAPPING_ERROR);
	memset((uint8_t *)endpoint.memory + dma - base, 0x79, 2048);
	dma_unmap_single(&device, dma, 2048, DMA_FROM_DEVICE);
	assert(skb->data[2047] == 0x79);
	linuxkpi_kfree_skb(skb);
	assert(status(arena).cpu == 0 && status(arena).fault == 0);
	assert(pcie_test_allocation_calls == allocations && endpoint.calls == calls);
	destroy(&endpoint, &arena);
}

static void test_faults_and_lifetime(void)
{
	struct hakios_dma_endpoint endpoint;
	struct hakios_dma_arena *arena = create(&endpoint);
	struct device other;
	uint8_t source[64];
	assert(dma_map_single(&other, source, sizeof(source), DMA_TO_DEVICE) == DMA_MAPPING_ERROR);
	assert(dma_set_mask(&device, 0xffff) == -EIO && dma_set_mask(&device, UINT32_MAX) == 0);
	dma_addr_t dma = dma_map_single(&device, source, sizeof(source), DMA_TO_DEVICE);
	assert(dma != DMA_MAPPING_ERROR);
	dma_sync_single_range_for_cpu(&device, dma, UINT64_MAX, 1, DMA_TO_DEVICE);
	assert(status(arena).fault == EINVAL);
	dma_unmap_single(&device, dma, sizeof(source), DMA_TO_DEVICE);
	assert(status(arena).streaming == 0);
	assert(hakios_dma_arena_get(&device) == arena);
	assert(hakios_dma_arena_close(arena) == EBUSY);
	hakios_dma_arena_put(arena);
	destroy(&endpoint, &arena);

	arena = create(&endpoint);
	struct sk_buff *skb = linuxkpi_alloc_skb(64, GFP_ATOMIC);
	assert(skb != NULL);
	dma = dma_map_single(&device, skb->data, 64, DMA_FROM_DEVICE);
	linuxkpi_kfree_skb(skb);
	assert(status(arena).fault == EBUSY && status(arena).cpu == 1);
	dma_unmap_single(&device, dma, 64, DMA_FROM_DEVICE);
	linuxkpi_kfree_skb(skb);
	assert(status(arena).cpu == 0);
	destroy(&endpoint, &arena);

	arena = create(&endpoint);
	assert(dma_map_sg_attrs(&device, NULL, 1, DMA_TO_DEVICE, 0) == 0);
	assert(status(arena).fault == ENOTSUP);
	destroy(&endpoint, &arena);
}

static void test_bounds_and_exhaustion(void)
{
	struct hakios_dma_endpoint endpoint;
	struct hakios_dma_arena *arena = create(&endpoint);
	uint8_t first[4096], second[4096];
	memset(first, 0x21, sizeof(first));
	memset(second, 0x36, sizeof(second));
	dma_addr_t first_dma = dma_map_single(&device, first, sizeof(first), DMA_BIDIRECTIONAL);
	dma_addr_t second_dma = dma_map_single(&device, second, sizeof(second), DMA_BIDIRECTIONAL);
	assert(first_dma != DMA_MAPPING_ERROR && second_dma == first_dma + 4096);
	memset((uint8_t *)endpoint.memory + second_dma - base, 0x78, sizeof(second));
	assert(hakios_dma_arena_sync_range(arena, first_dma, 4096, 1, HAKIOS_DMA_BIDIRECTIONAL, false) == EINVAL);
	assert(hakios_dma_arena_sync_range(arena, first_dma, SIZE_MAX, 1, HAKIOS_DMA_BIDIRECTIONAL, false) == EINVAL);
	assert(hakios_dma_arena_sync_range(arena, first_dma, 4095, 2, HAKIOS_DMA_BIDIRECTIONAL, false) == EINVAL);
	assert(hakios_dma_arena_sync_range(arena, first_dma, 0, 0, HAKIOS_DMA_BIDIRECTIONAL, false) == EINVAL);
	assert(hakios_dma_arena_unmap(arena, first_dma + 1, 4095, HAKIOS_DMA_BIDIRECTIONAL) == EINVAL);
	assert(hakios_dma_arena_unmap(arena, first_dma, 4096, HAKIOS_DMA_TO_DEVICE) == EINVAL);
	dma_sync_single_range_for_cpu(&device, first_dma, 4096, 1, DMA_BIDIRECTIONAL);
	assert(status(arena).fault == EINVAL && second[0] == 0x36);
	dma_unmap_single(&device, first_dma, sizeof(first), DMA_BIDIRECTIONAL);
	dma_unmap_single(&device, second_dma, sizeof(second), DMA_BIDIRECTIONAL);
	destroy(&endpoint, &arena);

	arena = create(&endpoint);
	assert(dma_max_mapping_size(&device) == 65536);
	dma_addr_t address;
	void *memory = dma_alloc_coherent(&device, 65536, &address, GFP_ATOMIC);
	assert(memory != NULL);
	dma_addr_t unavailable;
	assert(dma_alloc_coherent(&device, 1, &unavailable, GFP_ATOMIC) == NULL);
	assert(dma_map_single(&device, first, sizeof(first), DMA_TO_DEVICE) == DMA_MAPPING_ERROR);
	dma_free_coherent(&device, 65536, memory, address);
	assert(dma_alloc_coherent(&device, 1, &unavailable, GFP_ATOMIC) == NULL && status(arena).fault == 0);
	void *cpu;
	assert(hakios_dma_arena_cpu_alloc(arena, 65536, &cpu) == 0);
	assert(linuxkpi_alloc_skb(1, GFP_ATOMIC) == NULL);
	assert(hakios_dma_arena_close(arena) == 0 && dma_max_mapping_size(&device) == 0);
	assert(linuxkpi_alloc_skb(1, GFP_KERNEL) == NULL);
	endpoint.closed = endpoint.fenced = true;
	assert(hakios_dma_arena_destroy(&arena) == EBUSY && arena != NULL);
	assert(hakios_dma_arena_cpu_free(arena, cpu) == 0);
	assert(hakios_dma_arena_destroy(&arena) == 0);
	free(endpoint.memory);
}

static void test_failure_cleanup(void)
{
	const struct hakios_dma_arena_config config = {.dma_bytes = 65536, .cpu_bytes = 65536};
	for (unsigned failure = 1; failure <= 7; failure++) {
		struct hakios_dma_endpoint endpoint = {0};
		struct hakios_dma_arena *arena;
		pcie_test_fail_allocation_at = pcie_test_allocation_calls + failure;
		assert(hakios_dma_arena_create(&endpoint, &config, &arena) == ENOMEM);
		pcie_test_fail_allocation_at = 0;
		if (arena != NULL) {
			endpoint.closed = endpoint.fenced = true;
			assert(hakios_dma_arena_destroy(&arena) == 0);
		}
		assert(endpoint.holds == 0);
	}
	struct hakios_dma_endpoint endpoint = {.fail = true};
	struct hakios_dma_arena *arena;
	assert(hakios_dma_arena_create(&endpoint, &config, &arena) == EIO && arena != NULL);
	endpoint.closed = endpoint.fenced = true;
	assert(hakios_dma_arena_destroy(&arena) == 0 && endpoint.holds == 0);
	free(endpoint.memory);
}

static void *concurrent(void *unused)
{
	(void)unused;
	for (unsigned iteration = 0; iteration < 200; iteration++) {
		struct sk_buff *skb = linuxkpi_dev_alloc_skb(512, GFP_ATOMIC);
		assert(skb != NULL);
		memset(skb->data, (int)(iteration & 255), 512);
		dma_addr_t dma = dma_map_single(&device, skb->data, 512, DMA_TO_DEVICE);
		assert(dma != DMA_MAPPING_ERROR);
		dma_sync_single_for_cpu(&device, dma, 512, DMA_TO_DEVICE);
		dma_sync_single_range_for_device(&device, dma, 3, 8, DMA_TO_DEVICE);
		dma_unmap_single(&device, dma, 512, DMA_TO_DEVICE);
		linuxkpi_kfree_skb(skb);
	}
	return NULL;
}
static void test_concurrency(void)
{
	struct hakios_dma_endpoint endpoint;
	struct hakios_dma_arena *arena = create(&endpoint);
	unsigned allocations = pcie_test_allocation_calls, calls = endpoint.calls;
	pthread_t threads[4];
	for (unsigned index = 0; index < 4; index++) { assert(pthread_create(&threads[index], NULL, concurrent, NULL) == 0); }
	for (unsigned index = 0; index < 4; index++) { assert(pthread_join(threads[index], NULL) == 0); }
	assert(status(arena).fault == 0 && status(arena).cpu == 0 && status(arena).streaming == 0);
	assert(pcie_test_allocation_calls == allocations && endpoint.calls == calls);
	destroy(&endpoint, &arena);
}
int main(void)
{
	test_ranges(); test_coherent_and_skb(); test_faults_and_lifetime(); test_bounds_and_exhaustion(); test_failure_cleanup(); test_concurrency();
	puts("dma-arena-test: PASS (actual atomic DMA/SKB binding, byte-range ownership, quarantine, retained teardown, 800 concurrent lifetimes)");
	return 0;
}

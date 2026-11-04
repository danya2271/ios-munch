#ifndef _SYS_HAKIOS_DMA_ENDPOINT_H_
#define _SYS_HAKIOS_DMA_ENDPOINT_H_

#include <stddef.h>
#include <sys/hakios.pcie.h>
#include <sys/hakios.dma.space.h>

#define HAKIOS_DMA_ENDPOINT_VERSION 1u

enum hakios_dma_direction {
	HAKIOS_DMA_BIDIRECTIONAL,
	HAKIOS_DMA_TO_DEVICE,
	HAKIOS_DMA_FROM_DEVICE,
};

struct hakios_dma_backing {
	void *token;
	void *cpu;
	const uint64_t *pages;
	uint32_t count;
	bool coherent;
	bool cacheable;
};

struct hakios_dma_endpoint_operations {
	uint32_t version;
	int (*attach)(void *owner, hakios_pcie_lease_t endpoint,
	    const struct hakios_pcie_endpoint_resources *resources, const struct hakios_dma_pt_info *tables);
	int (*allocate)(void *owner, uint32_t pages, bool coherent, struct hakios_dma_backing *backing);
	int (*sync)(void *owner, const struct hakios_dma_backing *backing, enum hakios_dma_direction direction,
	    bool for_device);
	int (*free)(void *owner, struct hakios_dma_backing *backing);
	int (*fence)(void *owner);
	int (*detach)(void *owner);
};

struct hakios_dma_endpoint_config {
	hakios_pcie_lease_t endpoint;
	struct hakios_dma_space_config space;
	uint64_t mask;
};

struct hakios_dma_endpoint_status {
	hakios_pcie_lease_t endpoint;
	uint32_t mappings;
	int fault;
	bool closed;
	bool fenced;
};

struct hakios_dma_endpoint;

int hakios_dma_endpoint_create(const struct hakios_dma_endpoint_config *config,
    const struct hakios_dma_pt_operations *tables, const struct hakios_dma_endpoint_operations *operations,
    void *owner, struct hakios_dma_endpoint **result);
int hakios_dma_endpoint_alloc(struct hakios_dma_endpoint *endpoint, size_t size,
    hakios_dma_lease_t *lease, void **cpu, uint64_t *iova);
int hakios_dma_endpoint_alloc_aligned(struct hakios_dma_endpoint *endpoint, size_t size, uint64_t alignment,
    hakios_dma_lease_t *lease, void **cpu, uint64_t *iova);
int hakios_dma_endpoint_hold(struct hakios_dma_endpoint *endpoint);
int hakios_dma_endpoint_drop(struct hakios_dma_endpoint *endpoint);
int hakios_dma_endpoint_map(struct hakios_dma_endpoint *endpoint, void *cpu, size_t size,
    enum hakios_dma_direction direction, hakios_dma_lease_t *lease, uint64_t *iova);
int hakios_dma_endpoint_sync(struct hakios_dma_endpoint *endpoint, hakios_dma_lease_t lease, bool for_device);
int hakios_dma_endpoint_unmap(struct hakios_dma_endpoint *endpoint, hakios_dma_lease_t *lease);
int hakios_dma_endpoint_close(struct hakios_dma_endpoint *endpoint);
int hakios_dma_endpoint_status(struct hakios_dma_endpoint *endpoint, struct hakios_dma_endpoint_status *status);
int hakios_dma_endpoint_destroy(struct hakios_dma_endpoint **endpoint);
int hakios_dma_cache_streaming(void *cpu, size_t size, enum hakios_dma_direction direction, bool for_device);

#endif

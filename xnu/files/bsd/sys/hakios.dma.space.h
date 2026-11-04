#ifndef _SYS_HAKIOS_DMA_SPACE_H_
#define _SYS_HAKIOS_DMA_SPACE_H_

#include <sys/hakios.dma.pagetable.h>

#define HAKIOS_DMA_SPACE_IOVA_LIMIT (1ull << 39)
#define HAKIOS_DMA_SPACE_RESERVATIONS_MAX 4096u

typedef uint64_t hakios_dma_lease_t;

struct hakios_dma_space_config {
	struct hakios_dma_pt_config tables;
	uint64_t iova_base;
	uint64_t iova_limit;
};

struct hakios_dma_space;

int hakios_dma_space_create(const struct hakios_dma_space_config *config,
    const struct hakios_dma_pt_operations *operations, void *owner, struct hakios_dma_space **result);
int hakios_dma_space_get_info(struct hakios_dma_space *space, struct hakios_dma_pt_info *info);
int hakios_dma_space_status(struct hakios_dma_space *space);
int hakios_dma_space_reserve(struct hakios_dma_space *space, uint32_t page_count,
    uint64_t alignment, uint64_t boundary, hakios_dma_lease_t *lease);
int hakios_dma_space_map(struct hakios_dma_space *space, hakios_dma_lease_t lease,
    const uint64_t *physical_pages, uint32_t page_count, uint32_t protection, uint64_t *iova);
int hakios_dma_space_unmap(struct hakios_dma_space *space, hakios_dma_lease_t lease);
int hakios_dma_space_release(struct hakios_dma_space *space, hakios_dma_lease_t *lease);
int hakios_dma_space_destroy(struct hakios_dma_space **space);

#endif

#ifndef _SYS_HAKIOS_DMA_PAGETABLE_H_
#define _SYS_HAKIOS_DMA_PAGETABLE_H_

#include <stdint.h>
#include <stdbool.h>

#define HAKIOS_DMA_PT_VERSION 1u
#define HAKIOS_DMA_PT_PAGE_SIZE 4096u
#define HAKIOS_DMA_PT_TABLES_MAX 512u
#define HAKIOS_DMA_PT_PAGES_MAX 16384u

enum hakios_dma_pt_protection {
	HAKIOS_DMA_PT_READ = 1u << 0,
	HAKIOS_DMA_PT_WRITE = 1u << 1,
	HAKIOS_DMA_PT_CACHE = 1u << 2,
	HAKIOS_DMA_PT_MMIO = 1u << 3,
};

struct hakios_dma_pt_page {
	uint64_t *entries;
	uint64_t physical;
};

struct hakios_dma_pt_config {
	const struct hakios_dma_pt_page *pages;
	uint32_t table_count;
	uint8_t output_bits;
	bool nonsecure;
};

struct hakios_dma_pt_operations {
	uint32_t version;
	int (*sync)(void *owner, const struct hakios_dma_pt_page *page);
	int (*invalidate)(void *owner, uint64_t iova, uint64_t size);
	int (*quiesce)(void *owner);
};

struct hakios_dma_pt_info {
	uint64_t root;
	uint32_t granule;
	uint32_t mair0;
	uint8_t input_bits;
	uint8_t output_bits;
	uint8_t start_level;
	bool nonsecure;
};

struct hakios_dma_pagetable;

int hakios_dma_pt_create(const struct hakios_dma_pt_config *config,
    const struct hakios_dma_pt_operations *operations, void *owner, struct hakios_dma_pagetable **result);
int hakios_dma_pt_get_info(struct hakios_dma_pagetable *table, struct hakios_dma_pt_info *info);
int hakios_dma_pt_status(struct hakios_dma_pagetable *table);
int hakios_dma_pt_map(struct hakios_dma_pagetable *table, uint64_t iova,
    const uint64_t *physical_pages, uint32_t page_count, uint32_t protection);
int hakios_dma_pt_unmap(struct hakios_dma_pagetable *table, uint64_t iova, uint32_t page_count);
int hakios_dma_pt_destroy(struct hakios_dma_pagetable **table);

#endif

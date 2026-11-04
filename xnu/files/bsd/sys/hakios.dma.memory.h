#ifndef _SYS_HAKIOS_DMA_MEMORY_H_
#define _SYS_HAKIOS_DMA_MEMORY_H_

#include <sys/hakios.dma.endpoint.h>

int hakios_dma_memory_allocate(uint32_t pages, bool coherent, struct hakios_dma_backing *backing);
int hakios_dma_memory_sync(const struct hakios_dma_backing *backing,
    enum hakios_dma_direction direction, bool for_device);
int hakios_dma_memory_free(struct hakios_dma_backing *backing);
int hakios_dma_memory_create_endpoint(const struct hakios_dma_endpoint_config *config,
    const struct hakios_dma_pt_operations *tables, const struct hakios_dma_endpoint_operations *domain,
    void *owner, struct hakios_dma_endpoint **result);

#endif

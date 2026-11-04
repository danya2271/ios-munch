#ifndef PCIE_TEST_DMA_MAPPING_H
#define PCIE_TEST_DMA_MAPPING_H
#include "core.h"
typedef uint64_t dma_addr_t;
typedef unsigned gfp_t;
enum dma_data_direction { DMA_BIDIRECTIONAL, DMA_TO_DEVICE, DMA_FROM_DEVICE, DMA_NONE };
#define GFP_ATOMIC 1u
#define DMA_MAPPING_ERROR UINT64_MAX
#define dma_map_single(device, cpu, size, dir) dma_map_single_attrs(device, cpu, size, dir, 0)
#define dma_unmap_single(device, dma, size, dir) dma_unmap_single_attrs(device, dma, size, dir, 0)
static inline int dma_mapping_error(struct device *device, dma_addr_t dma)
{ (void)device; return dma == 0 || dma == DMA_MAPPING_ERROR; }
#endif

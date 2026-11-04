#ifndef _LX_QCA_DMA_MAPPING_H_
#define _LX_QCA_DMA_MAPPING_H_

#include_next <linux/dma-mapping.h>

#if defined(CONFIG_ATH11K_PCI)
void *lx_qca_dma_allocate(struct device *, size_t, dma_addr_t *, gfp_t, enum dma_data_direction);
void lx_qca_dma_release(struct device *, size_t, void *, dma_addr_t, enum dma_data_direction);
dma_addr_t lx_qca_dma_map(struct device *, void *, size_t, enum dma_data_direction, unsigned long);
void lx_qca_dma_unmap(struct device *, dma_addr_t, size_t, enum dma_data_direction, unsigned long);
void lx_qca_dma_sync_range(struct device *, dma_addr_t, unsigned long, size_t, enum dma_data_direction, bool);
int lx_qca_dma_mask(struct device *, uint64_t);
int lx_qca_dma_unsupported(struct device *);
size_t lx_qca_dma_max_mapping_size(struct device *);

#define dma_alloc_coherent(device, size, dma, flags) lx_qca_dma_allocate(device, size, dma, flags, DMA_BIDIRECTIONAL)
#define dma_zalloc_coherent(device, size, dma, flags) dma_alloc_coherent(device, size, dma, flags)
#define dma_free_coherent(device, size, cpu, dma) lx_qca_dma_release(device, size, cpu, dma, DMA_BIDIRECTIONAL)
#define dma_alloc_noncoherent lx_qca_dma_alloc_noncoherent
#define dma_free_noncoherent lx_qca_dma_free_noncoherent
static inline void *lx_qca_dma_alloc_noncoherent(struct device *device, size_t size, dma_addr_t *dma,
    enum dma_data_direction direction, gfp_t flags)
{ return lx_qca_dma_allocate(device, size, dma, flags, direction); }
static inline void lx_qca_dma_free_noncoherent(struct device *device, size_t size, void *cpu, dma_addr_t dma,
    enum dma_data_direction direction)
{ lx_qca_dma_release(device, size, cpu, dma, direction); }

#undef dma_map_single_attrs
#undef dma_unmap_single_attrs
#define dma_map_single_attrs lx_qca_dma_map
#define dma_unmap_single_attrs lx_qca_dma_unmap
#define dma_sync_single_for_cpu(device, dma, size, direction) lx_qca_dma_sync_range(device, dma, 0, size, direction, false)
#define dma_sync_single_for_device(device, dma, size, direction) lx_qca_dma_sync_range(device, dma, 0, size, direction, true)
#define dma_sync_single_range_for_cpu(device, dma, offset, size, direction) lx_qca_dma_sync_range(device, dma, offset, size, direction, false)
#define dma_sync_single_range_for_device(device, dma, offset, size, direction) lx_qca_dma_sync_range(device, dma, offset, size, direction, true)
#define dma_sync_single dma_sync_single_for_cpu
#define dma_set_mask lx_qca_dma_mask
#define dma_set_coherent_mask lx_qca_dma_mask
#define dma_set_mask_and_coherent lx_qca_dma_mask
#define dma_supported(device, mask) (lx_qca_dma_mask(device, mask) == 0)
#define dma_set_max_seg_size(device, size) lx_qca_dma_unsupported(device)
#define dma_max_mapping_size lx_qca_dma_max_mapping_size
#define dmam_alloc_coherent(device, size, dma, flags) (lx_qca_dma_unsupported(device), (void *)0)
#define dmam_free_coherent(device, size, cpu, dma) ((void)lx_qca_dma_unsupported(device))
#define dma_map_page_attrs(device, page, offset, size, direction, attrs) (lx_qca_dma_unsupported(device), DMA_MAPPING_ERROR)
#define dma_map_page(device, page, offset, size, direction) (lx_qca_dma_unsupported(device), DMA_MAPPING_ERROR)
#define dma_map_resource(device, address, size, direction, attrs) (lx_qca_dma_unsupported(device), DMA_MAPPING_ERROR)
#define dma_unmap_page(device, dma, size, direction) ((void)lx_qca_dma_unsupported(device))
#define dma_unmap_resource(device, dma, size, direction, attrs) ((void)lx_qca_dma_unsupported(device))
#undef dma_map_sg_attrs
#undef dma_unmap_sg_attrs
#define dma_map_sg_attrs(device, list, count, direction, attrs) (lx_qca_dma_unsupported(device), 0)
#define dma_unmap_sg_attrs(device, list, count, direction, attrs) ((void)lx_qca_dma_unsupported(device))
#define dma_map_sgtable(device, table, direction, attrs) lx_qca_dma_unsupported(device)
#define dma_unmap_sgtable(device, table, direction, attrs) ((void)lx_qca_dma_unsupported(device))
#define dma_sync_sg_for_cpu(device, list, count, direction) ((void)lx_qca_dma_unsupported(device))
#define dma_sync_sg_for_device(device, list, count, direction) ((void)lx_qca_dma_unsupported(device))
#endif

#endif

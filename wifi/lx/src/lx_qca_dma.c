#if defined(CONFIG_ATH11K_PCI)

#include <linux/dma-mapping.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <sys/hakios.dma.arena.h>

static int native_direction(enum dma_data_direction direction, enum hakios_dma_direction *native)
{
	switch (direction) {
	case DMA_BIDIRECTIONAL: *native = HAKIOS_DMA_BIDIRECTIONAL; return 0;
	case DMA_TO_DEVICE: *native = HAKIOS_DMA_TO_DEVICE; return 0;
	case DMA_FROM_DEVICE: *native = HAKIOS_DMA_FROM_DEVICE; return 0;
	default: return EINVAL;
	}
}

void *lx_qca_dma_allocate(struct device *device, size_t size, dma_addr_t *dma, gfp_t flags, enum dma_data_direction direction)
{
	(void)flags;
	if (dma == NULL) { return NULL; }
	*dma = DMA_MAPPING_ERROR;
	if (device == NULL) { return NULL; }
	struct hakios_dma_arena *arena = hakios_dma_arena_get(device);
	if (arena == NULL) { return NULL; }
	enum hakios_dma_direction native;
	void *cpu = NULL;
	uint64_t address;
	int error = native_direction(direction, &native);
	if (error == 0) { error = hakios_dma_arena_alloc(arena, size, native, &cpu, &address); }
	if (error == 0) { *dma = address; }
	hakios_dma_arena_put(arena);
	return error == 0 ? cpu : NULL;
}

void lx_qca_dma_release(struct device *device, size_t size, void *cpu, dma_addr_t dma, enum dma_data_direction direction)
{
	if (device == NULL) { return; }
	struct hakios_dma_arena *arena = hakios_dma_arena_get(device);
	if (arena == NULL) { return; }
	enum hakios_dma_direction native;
	int error = native_direction(direction, &native);
	if (error == 0) { error = hakios_dma_arena_free(arena, cpu, dma, size, native); }
	hakios_dma_arena_fault(arena, error);
	hakios_dma_arena_put(arena);
}

dma_addr_t lx_qca_dma_map(struct device *device, void *cpu, size_t size, enum dma_data_direction direction, unsigned long attrs)
{
	if (device == NULL) { return DMA_MAPPING_ERROR; }
	struct hakios_dma_arena *arena = hakios_dma_arena_get(device);
	if (arena == NULL) { return DMA_MAPPING_ERROR; }
	enum hakios_dma_direction native;
	uint64_t dma = DMA_MAPPING_ERROR;
	int error = attrs != 0 ? ENOTSUP : native_direction(direction, &native);
	if (error == 0) { error = hakios_dma_arena_map(arena, cpu, size, native, &dma); }
	hakios_dma_arena_put(arena);
	return error == 0 ? dma : DMA_MAPPING_ERROR;
}

void lx_qca_dma_unmap(struct device *device, dma_addr_t dma, size_t size, enum dma_data_direction direction, unsigned long attrs)
{
	if (device == NULL) { return; }
	struct hakios_dma_arena *arena = hakios_dma_arena_get(device);
	if (arena == NULL) { return; }
	enum hakios_dma_direction native;
	int error = attrs != 0 ? ENOTSUP : native_direction(direction, &native);
	if (error == 0) { error = hakios_dma_arena_unmap(arena, dma, size, native); }
	hakios_dma_arena_fault(arena, error);
	hakios_dma_arena_put(arena);
}

void lx_qca_dma_sync_range(struct device *device, dma_addr_t dma, unsigned long offset, size_t size,
    enum dma_data_direction direction, bool for_device)
{
	if (device == NULL) { return; }
	struct hakios_dma_arena *arena = hakios_dma_arena_get(device);
	if (arena == NULL) { return; }
	enum hakios_dma_direction native;
	int error = dma > UINT64_MAX - offset ? EINVAL : native_direction(direction, &native);
	if (error == 0) { error = hakios_dma_arena_sync_range(arena, dma, offset, size, native, for_device); }
	hakios_dma_arena_fault(arena, error);
	hakios_dma_arena_put(arena);
}

int lx_qca_dma_mask(struct device *device, uint64_t mask)
{
	if (device == NULL || (mask & (mask + 1)) != 0) { return -EINVAL; }
	struct hakios_dma_arena *arena = hakios_dma_arena_get(device);
	if (arena == NULL) { return -ENODEV; }
	struct hakios_dma_arena_status status;
	int error = hakios_dma_arena_status(arena, &status);
	if (error == 0 && (status.closed || status.fault != 0 || status.limit == 0 || status.limit - 1 > mask)) { error = EIO; }
	hakios_dma_arena_put(arena);
	return -error;
}

size_t lx_qca_dma_max_mapping_size(struct device *device)
{
	struct hakios_dma_arena *arena = device != NULL ? hakios_dma_arena_get(device) : NULL;
	if (arena == NULL) { return 0; }
	struct hakios_dma_arena_status status;
	int error = hakios_dma_arena_status(arena, &status);
	size_t size = error == 0 && !status.closed && status.fault == 0 ? status.capacity : 0;
	hakios_dma_arena_put(arena);
	return size;
}

int lx_qca_dma_unsupported(struct device *device)
{
	struct hakios_dma_arena *arena = device != NULL ? hakios_dma_arena_get(device) : NULL;
	if (arena != NULL) { hakios_dma_arena_fault(arena, ENOTSUP); hakios_dma_arena_put(arena); }
	return -EOPNOTSUPP;
}

struct sk_buff *linuxkpi_dev_alloc_skb(size_t size, gfp_t flags)
{
	(void)flags;
	size_t descriptor = (sizeof(struct sk_buff) + 63) & ~(size_t)63;
	size_t shared = (sizeof(struct skb_shared_info) + 63) & ~(size_t)63;
	size_t overhead = descriptor + shared + 64 + NET_SKB_PAD;
	if (size > UINT32_MAX - overhead) { return NULL; }
	struct hakios_dma_arena *arena = hakios_dma_arena_get(NULL);
	if (arena == NULL) { return NULL; }
	void *raw = NULL;
	int error = hakios_dma_arena_cpu_alloc(arena, size + overhead, &raw);
	struct sk_buff *skb = NULL;
	if (error == 0) {
		skb = (void *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
		skb->m = raw;
		skb->prev = skb->next = skb;
		skb->shinfo = (void *)((uint8_t *)skb + descriptor);
		skb->head = skb->data = (uint8_t *)skb->shinfo + shared;
		skb->tail = skb->head;
		skb->end = skb->head + NET_SKB_PAD + size;
		skb->truesize = (uint32_t)(size + overhead);
		refcount_set(&skb->refcnt, 1);
		skb_reserve(skb, NET_SKB_PAD);
	}
	hakios_dma_arena_put(arena);
	return skb;
}
struct sk_buff *linuxkpi_alloc_skb(size_t size, gfp_t flags) { return linuxkpi_dev_alloc_skb(size, flags); }

void linuxkpi_kfree_skb(struct sk_buff *skb)
{
	if (skb == NULL) { return; }
	struct hakios_dma_arena *arena = hakios_dma_arena_get(NULL);
	if (arena == NULL) { return; }
	if (!refcount_dec_and_test(&skb->refcnt)) { hakios_dma_arena_put(arena); return; }
	int error = skb->m_free_func != NULL || skb->shinfo->nr_frags != 0 || skb->shinfo->frag_list != NULL ? ENOTSUP :
	    hakios_dma_arena_cpu_free(arena, skb->m);
	if (error != 0) { refcount_set(&skb->refcnt, 1); hakios_dma_arena_fault(arena, error); }
	hakios_dma_arena_put(arena);
}

#endif

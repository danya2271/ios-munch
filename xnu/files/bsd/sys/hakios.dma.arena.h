#ifndef _SYS_HAKIOS_DMA_ARENA_H_
#define _SYS_HAKIOS_DMA_ARENA_H_

#include <sys/hakios.dma.endpoint.h>

struct hakios_dma_arena;
struct hakios_dma_arena_config { uint32_t dma_bytes, cpu_bytes; };
struct hakios_dma_arena_status {
	struct hakios_dma_endpoint *endpoint;
	uint64_t limit;
	uint32_t capacity;
	uint32_t coherent, streaming, cpu, retired, busy;
	int fault;
	bool closed;
};

int hakios_dma_arena_create(struct hakios_dma_endpoint *endpoint, const struct hakios_dma_arena_config *config,
    struct hakios_dma_arena **result);
int hakios_dma_arena_bind(struct hakios_dma_arena *arena, void *device);
struct hakios_dma_arena *hakios_dma_arena_get(void *device);
void hakios_dma_arena_put(struct hakios_dma_arena *arena);
int hakios_dma_arena_alloc(struct hakios_dma_arena *arena, size_t size, enum hakios_dma_direction direction,
    void **cpu, uint64_t *dma);
int hakios_dma_arena_free(struct hakios_dma_arena *arena, void *cpu, uint64_t dma, size_t size,
    enum hakios_dma_direction direction);
int hakios_dma_arena_map(struct hakios_dma_arena *arena, void *cpu, size_t size,
    enum hakios_dma_direction direction, uint64_t *dma);
int hakios_dma_arena_sync(struct hakios_dma_arena *arena, uint64_t dma, size_t size,
    enum hakios_dma_direction direction, bool for_device);
int hakios_dma_arena_sync_range(struct hakios_dma_arena *arena, uint64_t dma, size_t offset, size_t size,
    enum hakios_dma_direction direction, bool for_device);
int hakios_dma_arena_unmap(struct hakios_dma_arena *arena, uint64_t dma, size_t size,
    enum hakios_dma_direction direction);
int hakios_dma_arena_cpu_alloc(struct hakios_dma_arena *arena, size_t size, void **cpu);
int hakios_dma_arena_cpu_free(struct hakios_dma_arena *arena, void *cpu);
int hakios_dma_arena_status(struct hakios_dma_arena *arena, struct hakios_dma_arena_status *status);
void hakios_dma_arena_fault(struct hakios_dma_arena *arena, int error);
int hakios_dma_arena_close(struct hakios_dma_arena *arena);
int hakios_dma_arena_destroy(struct hakios_dma_arena **arena);

#endif

#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.dma.arena.h>

#define ARENA_PAGE 4096u
#define ARENA_LIMIT (64u << 20)

enum arena_kind { ARENA_EMPTY, ARENA_COHERENT, ARENA_STREAM, ARENA_CPU, ARENA_RETIRED };
struct arena_record {
	void *source;
	size_t size;
	uint32_t pages;
	enum hakios_dma_direction direction;
	enum arena_kind kind;
	bool busy;
};
struct arena_area {
	uint8_t *cpu;
	uint32_t count;
	uint32_t *owners;
	struct arena_record *records;
};
struct hakios_dma_arena {
	lck_spin_t lock;
	struct arena_area dma, cpu;
	struct hakios_dma_arena_status status;
	uint8_t *ownership;
	hakios_dma_lease_t lease;
	uint64_t base;
	void *device;
	uint32_t users;
	bool held;
};
static LCK_GRP_DECLARE(arena_group, "hakios-dma-arena");
static LCK_SPIN_DECLARE(arena_registry_lock, &arena_group);
static struct hakios_dma_arena *arena_registered;

static void arena_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dsb sy" ::: "memory");
#else
	__sync_synchronize();
#endif
}
static bool arena_size(uint32_t size) { return size >= ARENA_PAGE && size <= ARENA_LIMIT && (size & (size - 1)) == 0; }
static bool overlaps(uintptr_t first, size_t length, uintptr_t second, size_t size)
{ return first < second + size && second < first + length; }
static int arena_health(struct hakios_dma_arena *arena)
{ return arena->status.fault != 0 ? arena->status.fault : arena->status.closed ? ESHUTDOWN : 0; }

static int area_init(struct arena_area *area, uint32_t bytes)
{
	area->count = bytes / ARENA_PAGE;
	area->owners = kalloc_type(uint32_t, area->count, Z_WAITOK | Z_ZERO);
	area->records = kalloc_type(struct arena_record, area->count, Z_WAITOK | Z_ZERO);
	return area->owners != NULL && area->records != NULL ? 0 : ENOMEM;
}
static void area_dispose(struct arena_area *area)
{
	if (area->owners != NULL) { kfree_type(uint32_t, area->count, area->owners); }
	if (area->records != NULL) { kfree_type(struct arena_record, area->count, area->records); }
}
static void area_release(struct arena_area *area, uint32_t index)
{
	uint32_t pages = area->records[index].pages;
	for (uint32_t page = index; page < index + pages; page++) { area->owners[page] = 0; }
	bzero(&area->records[index], sizeof(area->records[index]));
}
static int area_reserve(struct arena_area *area, size_t size, enum arena_kind kind, uint32_t *index)
{
	if (size == 0 || size > (size_t)area->count * ARENA_PAGE) { return EINVAL; }
	uint32_t pages = (uint32_t)((size + ARENA_PAGE - 1) / ARENA_PAGE), alignment = 1;
	if (kind == ARENA_COHERENT) { while (alignment < pages) { alignment <<= 1; } }
	for (uint32_t start = 0; start + pages <= area->count;) {
		start = (start + alignment - 1) & ~(alignment - 1);
		if (start + pages > area->count) { break; }
		uint32_t offset;
		for (offset = 0; offset < pages && area->owners[start + offset] == 0; offset++) {}
		if (offset != pages) { start += offset + 1; continue; }
		for (offset = 0; offset < pages; offset++) { area->owners[start + offset] = start + 1; }
		area->records[start] = (struct arena_record) {.size = size, .pages = pages, .kind = kind, .busy = true};
		*index = start;
		return 0;
	}
	return ENOSPC;
}

int
hakios_dma_arena_create(struct hakios_dma_endpoint *endpoint, const struct hakios_dma_arena_config *config,
    struct hakios_dma_arena **result)
{
	if (result == NULL) { return EINVAL; }
	*result = NULL;
	if (endpoint == NULL || config == NULL || !arena_size(config->dma_bytes) || !arena_size(config->cpu_bytes)) { return EINVAL; }
	struct hakios_dma_arena *arena = kalloc_type(struct hakios_dma_arena, Z_WAITOK | Z_ZERO);
	if (arena == NULL) { return ENOMEM; }
	lck_spin_init(&arena->lock, &arena_group, LCK_ATTR_NULL);
	arena->status.endpoint = endpoint;
	*result = arena;
	int error = hakios_dma_endpoint_hold(endpoint);
	if (error != 0) {
		lck_spin_destroy(&arena->lock, &arena_group);
		kfree_type(struct hakios_dma_arena, arena);
		*result = NULL;
		return error;
	}
	arena->held = true;
	if ((error = area_init(&arena->dma, config->dma_bytes)) != 0 ||
	    (error = area_init(&arena->cpu, config->cpu_bytes)) != 0) { goto fail; }
	arena->ownership = kalloc_type(uint8_t, config->dma_bytes / 8, Z_WAITOK | Z_ZERO);
	arena->cpu.cpu = kalloc_type(uint8_t, config->cpu_bytes, Z_WAITOK | Z_ZERO);
	if (arena->ownership == NULL || arena->cpu.cpu == NULL) { error = ENOMEM; goto fail; }
	void *memory = NULL;
	error = hakios_dma_endpoint_alloc_aligned(endpoint, config->dma_bytes, config->dma_bytes,
	    &arena->lease, &memory, &arena->base);
	if (error != 0) { goto fail; }
	arena->dma.cpu = memory;
	arena->status.limit = arena->base + config->dma_bytes;
	arena->status.capacity = config->dma_bytes;
	return 0;
fail:
	arena->status.fault = error;
	arena->status.closed = true;
	return error;
}

int hakios_dma_arena_bind(struct hakios_dma_arena *arena, void *device)
{
	if (arena == NULL || device == NULL) { return EINVAL; }
	lck_spin_lock(&arena_registry_lock);
	lck_spin_lock(&arena->lock);
	int error = arena_health(arena);
	if (error == 0 && (arena_registered != NULL || arena->device != NULL)) { error = EBUSY; }
	if (error == 0) { arena->device = device; arena_registered = arena; }
	lck_spin_unlock(&arena->lock);
	lck_spin_unlock(&arena_registry_lock);
	return error;
}
struct hakios_dma_arena *hakios_dma_arena_get(void *device)
{
	lck_spin_lock(&arena_registry_lock);
	struct hakios_dma_arena *arena = arena_registered;
	if (arena != NULL && (device == NULL || device == arena->device) && arena->users != UINT32_MAX) { arena->users++; }
	else { arena = NULL; }
	lck_spin_unlock(&arena_registry_lock);
	return arena;
}
void hakios_dma_arena_put(struct hakios_dma_arena *arena)
{
	if (arena == NULL) { return; }
	lck_spin_lock(&arena_registry_lock);
	if (arena->users != 0) { arena->users--; }
	lck_spin_unlock(&arena_registry_lock);
}

static int
arena_allocate(struct hakios_dma_arena *arena, void *source, size_t size, enum hakios_dma_direction direction,
    enum arena_kind kind, void **cpu, uint64_t *dma)
{
	if (cpu != NULL) { *cpu = NULL; }
	if (dma != NULL) { *dma = 0; }
	if (arena == NULL || size == 0 || (unsigned)direction > HAKIOS_DMA_FROM_DEVICE ||
	    (kind == ARENA_STREAM && (source == NULL || (uintptr_t)source > UINTPTR_MAX - size))) { return EINVAL; }
	struct arena_area *area = kind == ARENA_CPU ? &arena->cpu : &arena->dma;
	lck_spin_lock(&arena->lock);
	int error = arena_health(arena);
	uint32_t index = 0;
	if (error == 0 && kind == ARENA_STREAM) {
		if (overlaps((uintptr_t)source, size, (uintptr_t)arena->dma.cpu, (size_t)arena->dma.count * ARENA_PAGE)) { error = EINVAL; }
		uintptr_t cpu_base = (uintptr_t)arena->cpu.cpu, address = (uintptr_t)source;
		if (error == 0 && overlaps(address, size, cpu_base, (size_t)arena->cpu.count * ARENA_PAGE)) {
			if (address < cpu_base || address - cpu_base >= (size_t)arena->cpu.count * ARENA_PAGE) { error = EINVAL; }
			else {
				uint32_t owner = arena->cpu.owners[(address - cpu_base) / ARENA_PAGE];
				if (owner == 0) { error = ESTALE; }
				else {
					struct arena_record *allocation = &arena->cpu.records[owner - 1];
					size_t offset = address - cpu_base - (size_t)(owner - 1) * ARENA_PAGE;
					if (allocation->kind != ARENA_CPU || allocation->busy || offset > allocation->size || size > allocation->size - offset) { error = EINVAL; }
				}
			}
		}
		for (uint32_t prior = 0; error == 0 && prior < arena->dma.count; prior++) {
			struct arena_record *record = &arena->dma.records[prior];
			if (record->kind == ARENA_STREAM && overlaps((uintptr_t)source, size, (uintptr_t)record->source, record->size)) { error = EBUSY; }
		}
	}
	if (error == 0) { error = area_reserve(area, size, kind, &index); }
	if (error != 0) { lck_spin_unlock(&arena->lock); return error; }
	struct arena_record *record = &area->records[index];
	record->source = source;
	record->direction = direction;
	arena->status.busy++;
	lck_spin_unlock(&arena->lock);
	uint8_t *memory = area->cpu + (size_t)index * ARENA_PAGE;
	bzero(memory, (size_t)record->pages * ARENA_PAGE);
	if (kind == ARENA_STREAM) {
		memset(arena->ownership + (size_t)index * (ARENA_PAGE / 8), 255, (size_t)record->pages * (ARENA_PAGE / 8));
		if (direction != HAKIOS_DMA_FROM_DEVICE) { memcpy(memory, source, size); }
	}
	arena_barrier();
	lck_spin_lock(&arena->lock);
	arena->status.busy--;
	record->busy = false;
	error = arena_health(arena);
	if (error == 0) {
		if (kind == ARENA_COHERENT) { arena->status.coherent++; }
		if (kind == ARENA_STREAM) { arena->status.streaming++; }
		if (kind == ARENA_CPU) { arena->status.cpu++; }
		if (cpu != NULL) { *cpu = memory; }
		if (dma != NULL) { *dma = arena->base + (size_t)index * ARENA_PAGE; }
	} else { area_release(area, index); }
	lck_spin_unlock(&arena->lock);
	return error;
}
int hakios_dma_arena_alloc(struct hakios_dma_arena *arena, size_t size, enum hakios_dma_direction direction, void **cpu, uint64_t *dma)
{ return cpu == NULL || dma == NULL ? EINVAL : arena_allocate(arena, NULL, size, direction, ARENA_COHERENT, cpu, dma); }
int hakios_dma_arena_map(struct hakios_dma_arena *arena, void *cpu, size_t size, enum hakios_dma_direction direction, uint64_t *dma)
{ return dma == NULL ? EINVAL : arena_allocate(arena, cpu, size, direction, ARENA_STREAM, NULL, dma); }
int hakios_dma_arena_cpu_alloc(struct hakios_dma_arena *arena, size_t size, void **cpu)
{ return cpu == NULL ? EINVAL : arena_allocate(arena, NULL, size, HAKIOS_DMA_BIDIRECTIONAL, ARENA_CPU, cpu, NULL); }

static int arena_lookup(struct hakios_dma_arena *arena, uint64_t dma, size_t size,
    enum hakios_dma_direction direction, uint32_t *index, size_t *offset)
{
	if (dma < arena->base || dma >= arena->status.limit || size == 0 || (unsigned)direction > HAKIOS_DMA_FROM_DEVICE) { return EINVAL; }
	uint32_t owner = arena->dma.owners[(dma - arena->base) / ARENA_PAGE];
	if (owner == 0) { return ESTALE; }
	*index = owner - 1;
	struct arena_record *record = &arena->dma.records[*index];
	*offset = (size_t)(dma - arena->base - (uint64_t)*index * ARENA_PAGE);
	if (record->kind != ARENA_COHERENT && record->kind != ARENA_STREAM) { return ESTALE; }
	if (*offset > record->size || size > record->size - *offset || direction != record->direction) { return EINVAL; }
	return record->busy ? EBUSY : 0;
}
static void arena_copy(struct hakios_dma_arena *arena, uint32_t index, size_t offset, size_t size, bool for_device)
{
	struct arena_record *record = &arena->dma.records[index];
	arena_barrier();
	if (record->kind == ARENA_STREAM) {
		uint8_t *device = arena->dma.cpu + (size_t)index * ARENA_PAGE;
		uint8_t *source = record->source;
		uint8_t *bits = arena->ownership + (size_t)index * (ARENA_PAGE / 8);
		for (size_t position = offset; position < offset + size; position++) {
			uint8_t mask = (uint8_t)(1u << (position & 7));
			bool owned = (bits[position / 8] & mask) != 0;
			if (owned == for_device) { continue; }
			if (for_device) {
				if (record->direction != HAKIOS_DMA_FROM_DEVICE) { device[position] = source[position]; }
				bits[position / 8] |= mask;
			} else {
				if (record->direction != HAKIOS_DMA_TO_DEVICE) { source[position] = device[position]; }
				bits[position / 8] &= (uint8_t)~mask;
			}
		}
	}
	arena_barrier();
}
int hakios_dma_arena_sync_range(struct hakios_dma_arena *arena, uint64_t dma, size_t range_offset, size_t size,
    enum hakios_dma_direction direction, bool for_device)
{
	if (arena == NULL) { return EINVAL; }
	lck_spin_lock(&arena->lock);
	uint32_t index = 0;
	size_t offset = 0;
	int error = arena_health(arena);
	if (error == 0) { error = arena_lookup(arena, dma, 1, direction, &index, &offset); }
	if (error == 0) {
		size_t available = arena->dma.records[index].size - offset;
		if (size == 0 || range_offset > available || size > available - range_offset) { error = EINVAL; }
		else { offset += range_offset; }
	}
	if (error != 0) { lck_spin_unlock(&arena->lock); return error; }
	arena->dma.records[index].busy = true;
	arena->status.busy++;
	lck_spin_unlock(&arena->lock);
	arena_copy(arena, index, offset, size, for_device);
	lck_spin_lock(&arena->lock);
	arena->dma.records[index].busy = false;
	arena->status.busy--;
	lck_spin_unlock(&arena->lock);
	return 0;
}
int hakios_dma_arena_sync(struct hakios_dma_arena *arena, uint64_t dma, size_t size, enum hakios_dma_direction direction, bool for_device)
{ return hakios_dma_arena_sync_range(arena, dma, 0, size, direction, for_device); }
int hakios_dma_arena_unmap(struct hakios_dma_arena *arena, uint64_t dma, size_t size, enum hakios_dma_direction direction)
{
	if (arena == NULL || arena->dma.records == NULL) { return EINVAL; }
	lck_spin_lock(&arena->lock);
	uint32_t index = 0;
	size_t offset = 0;
	int error = arena_lookup(arena, dma, size, direction, &index, &offset);
	struct arena_record *record = &arena->dma.records[index];
	if (error == 0 && (offset != 0 || size != record->size || record->kind != ARENA_STREAM)) { error = EINVAL; }
	if (error != 0) { lck_spin_unlock(&arena->lock); return error; }
	record->busy = true;
	arena->status.busy++;
	bool copy = arena_health(arena) == 0;
	lck_spin_unlock(&arena->lock);
	if (copy) { arena_copy(arena, index, 0, size, false); }
	lck_spin_lock(&arena->lock);
	arena->status.busy--;
	arena->status.streaming--;
	area_release(&arena->dma, index);
	lck_spin_unlock(&arena->lock);
	return 0;
}
int hakios_dma_arena_free(struct hakios_dma_arena *arena, void *cpu, uint64_t dma, size_t size, enum hakios_dma_direction direction)
{
	if (arena == NULL || arena->dma.records == NULL) { return EINVAL; }
	lck_spin_lock(&arena->lock);
	uint32_t index = 0;
	size_t offset = 0;
	int error = arena_lookup(arena, dma, size, direction, &index, &offset);
	struct arena_record *record = &arena->dma.records[index];
	if (error == 0 && (offset != 0 || size != record->size || record->kind != ARENA_COHERENT ||
	    cpu != arena->dma.cpu + (size_t)index * ARENA_PAGE)) { error = EINVAL; }
	if (error == 0) { record->kind = ARENA_RETIRED; arena->status.coherent--; arena->status.retired++; }
	lck_spin_unlock(&arena->lock);
	return error;
}
int hakios_dma_arena_cpu_free(struct hakios_dma_arena *arena, void *cpu)
{
	if (arena == NULL || cpu == NULL || arena->cpu.records == NULL) { return EINVAL; }
	lck_spin_lock(&arena->lock);
	uintptr_t address = (uintptr_t)cpu, base = (uintptr_t)arena->cpu.cpu;
	int error = address < base || address - base >= (size_t)arena->cpu.count * ARENA_PAGE || (address - base) % ARENA_PAGE != 0 ? EINVAL : 0;
	uint32_t index = error == 0 ? (uint32_t)((address - base) / ARENA_PAGE) : 0;
	struct arena_record *record = &arena->cpu.records[index];
	if (error == 0 && (record->kind != ARENA_CPU || record->busy)) { error = ESTALE; }
	for (uint32_t slot = 0; error == 0 && slot < arena->dma.count; slot++) {
		struct arena_record *mapped = &arena->dma.records[slot];
		if (mapped->kind == ARENA_STREAM && overlaps(address, record->size, (uintptr_t)mapped->source, mapped->size)) { error = EBUSY; }
	}
	if (error == 0) { arena->status.cpu--; area_release(&arena->cpu, index); }
	lck_spin_unlock(&arena->lock);
	return error;
}
int hakios_dma_arena_status(struct hakios_dma_arena *arena, struct hakios_dma_arena_status *status)
{
	if (arena == NULL || status == NULL) { return EINVAL; }
	lck_spin_lock(&arena->lock); *status = arena->status; lck_spin_unlock(&arena->lock);
	return 0;
}
void hakios_dma_arena_fault(struct hakios_dma_arena *arena, int error)
{
	if (arena == NULL || error == 0) { return; }
	lck_spin_lock(&arena->lock);
	if (arena->status.fault == 0) { arena->status.fault = error < 0 ? EIO : error; }
	lck_spin_unlock(&arena->lock);
}
int hakios_dma_arena_close(struct hakios_dma_arena *arena)
{
	if (arena == NULL) { return EINVAL; }
	lck_spin_lock(&arena_registry_lock);
	lck_spin_lock(&arena->lock);
	arena->status.closed = true;
	int error = arena->users != 0 || arena->status.busy != 0 ? EBUSY : 0;
	lck_spin_unlock(&arena->lock);
	lck_spin_unlock(&arena_registry_lock);
	return error;
}
int hakios_dma_arena_destroy(struct hakios_dma_arena **arena)
{
	if (arena == NULL || *arena == NULL) { return EINVAL; }
	struct hakios_dma_arena *state = *arena;
	int error = hakios_dma_arena_close(state);
	if (error != 0) { return error; }
	struct hakios_dma_endpoint_status endpoint;
	error = hakios_dma_endpoint_status(state->status.endpoint, &endpoint);
	if (error != 0 || !endpoint.closed || !endpoint.fenced) { return error != 0 ? error : EBUSY; }
	lck_spin_lock(&arena_registry_lock);
	lck_spin_lock(&state->lock);
	error = state->users != 0 || state->status.busy != 0 || state->status.coherent != 0 ||
	    state->status.streaming != 0 || state->status.cpu != 0 ? EBUSY : 0;
	if (error == 0 && arena_registered == state) { arena_registered = NULL; }
	lck_spin_unlock(&state->lock);
	lck_spin_unlock(&arena_registry_lock);
	if (error != 0) { return error; }
	if (state->held && (error = hakios_dma_endpoint_drop(state->status.endpoint)) != 0) { return error; }
	if (state->ownership != NULL) { kfree_type(uint8_t, (size_t)state->dma.count * ARENA_PAGE / 8, state->ownership); }
	if (state->cpu.cpu != NULL) { kfree_type(uint8_t, (size_t)state->cpu.count * ARENA_PAGE, state->cpu.cpu); }
	area_dispose(&state->dma);
	area_dispose(&state->cpu);
	lck_spin_destroy(&state->lock, &arena_group);
	kfree_type(struct hakios_dma_arena, state);
	*arena = NULL;
	return 0;
}

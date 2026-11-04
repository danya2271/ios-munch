#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.dma.space.h>

struct dma_reservation {
	struct dma_reservation *next;
	hakios_dma_lease_t lease;
	uint64_t base;
	uint32_t page_count;
	bool mapped;
};

struct hakios_dma_space {
	lck_mtx_t lock;
	struct hakios_dma_pagetable *table;
	struct dma_reservation *reservations;
	uint64_t base;
	uint64_t limit;
	uint32_t count;
	bool faulted;
};

static LCK_GRP_DECLARE(dma_space_lock_group, "hakios-dma-space");
static LCK_MTX_DECLARE(dma_lease_lock, &dma_space_lock_group);
static hakios_dma_lease_t dma_lease_generation;

static int
dma_next_lease(hakios_dma_lease_t *lease)
{
	int error = 0;

	lck_mtx_lock(&dma_lease_lock);
	if (dma_lease_generation == UINT64_MAX) {
		error = EOVERFLOW;
	} else {
		*lease = ++dma_lease_generation;
	}
	lck_mtx_unlock(&dma_lease_lock);
	return error;
}

static int
dma_space_health(struct hakios_dma_space *space)
{
	if (!space->faulted && hakios_dma_pt_status(space->table) != 0) {
		space->faulted = true;
	}
	return space->faulted ? EIO : 0;
}

static bool
dma_power_of_two(uint64_t value)
{
	return value >= HAKIOS_DMA_PT_PAGE_SIZE && value <= HAKIOS_DMA_SPACE_IOVA_LIMIT &&
	       (value & (value - 1)) == 0;
}

static bool
dma_gap_fit(uint64_t cursor, uint64_t limit, uint64_t size,
    uint64_t alignment, uint64_t boundary, uint64_t *base)
{
	uint64_t candidate = (cursor + alignment - 1) & ~(alignment - 1);

	if (boundary != 0 && (candidate / boundary) != ((candidate + size - 1) / boundary)) {
		candidate = (candidate + boundary - 1) & ~(boundary - 1);
		candidate = (candidate + alignment - 1) & ~(alignment - 1);
	}
	if (candidate > limit || size > limit - candidate) {
		return false;
	}
	*base = candidate;
	return true;
}

static struct dma_reservation *
dma_find_lease(struct hakios_dma_space *space, hakios_dma_lease_t lease)
{
	for (struct dma_reservation *entry = space->reservations; entry != NULL; entry = entry->next) {
		if (entry->lease == lease) {
			return entry;
		}
	}
	return NULL;
}

int
hakios_dma_space_create(const struct hakios_dma_space_config *config,
    const struct hakios_dma_pt_operations *operations, void *owner, struct hakios_dma_space **result)
{
	struct hakios_dma_space *space;
	int error;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || config->iova_base < HAKIOS_DMA_PT_PAGE_SIZE ||
	    config->iova_base >= config->iova_limit || config->iova_limit > HAKIOS_DMA_SPACE_IOVA_LIMIT ||
	    ((config->iova_base | config->iova_limit) & (HAKIOS_DMA_PT_PAGE_SIZE - 1)) != 0) {
		return EINVAL;
	}
	space = kalloc_type(struct hakios_dma_space, Z_WAITOK | Z_ZERO);
	if (space == NULL) {
		return ENOMEM;
	}
	error = hakios_dma_pt_create(&config->tables, operations, owner, &space->table);
	if (error != 0) {
		kfree_type(struct hakios_dma_space, space);
		return error;
	}
	lck_mtx_init(&space->lock, &dma_space_lock_group, LCK_ATTR_NULL);
	space->base = config->iova_base;
	space->limit = config->iova_limit;
	*result = space;
	return 0;
}

int
hakios_dma_space_get_info(struct hakios_dma_space *space, struct hakios_dma_pt_info *info)
{
	int error;

	if (info == NULL) {
		return EINVAL;
	}
	bzero(info, sizeof(*info));
	if (space == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&space->lock);
	error = dma_space_health(space);
	if (error == 0) {
		error = hakios_dma_pt_get_info(space->table, info);
		(void)dma_space_health(space);
	}
	lck_mtx_unlock(&space->lock);
	return error;
}

int
hakios_dma_space_status(struct hakios_dma_space *space)
{
	int error;

	if (space == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&space->lock);
	error = dma_space_health(space);
	lck_mtx_unlock(&space->lock);
	return error;
}

int
hakios_dma_space_reserve(struct hakios_dma_space *space, uint32_t page_count,
    uint64_t alignment, uint64_t boundary, hakios_dma_lease_t *lease)
{
	uint64_t size = (uint64_t)page_count * HAKIOS_DMA_PT_PAGE_SIZE;
	struct dma_reservation **position, *entry;
	uint64_t cursor, base;
	int error;

	if (lease == NULL) {
		return EINVAL;
	}
	*lease = 0;
	if (space == NULL || page_count == 0 || page_count > HAKIOS_DMA_PT_PAGES_MAX ||
	    !dma_power_of_two(alignment) ||
	    (boundary != 0 && (!dma_power_of_two(boundary) || size > boundary))) {
		return EINVAL;
	}
	lck_mtx_lock(&space->lock);
	error = dma_space_health(space);
	if (error != 0) {
		goto out;
	}
	if (space->count == HAKIOS_DMA_SPACE_RESERVATIONS_MAX) {
		error = ENOSPC;
		goto out;
	}
	position = &space->reservations;
	cursor = space->base;
	while (!dma_gap_fit(cursor, *position == NULL ? space->limit : (*position)->base,
	    size, alignment, boundary, &base)) {
		if (*position == NULL) {
			error = ENOSPC;
			goto out;
		}
		cursor = (*position)->base + (uint64_t)(*position)->page_count * HAKIOS_DMA_PT_PAGE_SIZE;
		position = &(*position)->next;
	}
	entry = kalloc_type(struct dma_reservation, Z_WAITOK | Z_ZERO);
	if (entry == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = dma_next_lease(&entry->lease);
	if (error != 0) {
		kfree_type(struct dma_reservation, entry);
		goto out;
	}
	entry->base = base;
	entry->page_count = page_count;
	entry->next = *position;
	*position = entry;
	space->count++;
	*lease = entry->lease;
out:
	lck_mtx_unlock(&space->lock);
	return error;
}

int
hakios_dma_space_map(struct hakios_dma_space *space, hakios_dma_lease_t lease,
    const uint64_t *physical_pages, uint32_t page_count, uint32_t protection, uint64_t *iova)
{
	struct dma_reservation *entry;
	int error;

	if (iova == NULL) {
		return EINVAL;
	}
	*iova = 0;
	if (space == NULL || physical_pages == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&space->lock);
	error = dma_space_health(space);
	if (error != 0) {
		goto out;
	}
	entry = dma_find_lease(space, lease);
	if (entry == NULL) {
		error = ESTALE;
	} else if (entry->mapped) {
		error = EBUSY;
	} else if (page_count != entry->page_count) {
		error = EINVAL;
	} else {
		error = hakios_dma_pt_map(space->table, entry->base, physical_pages, page_count, protection);
		if (error == EEXIST) {
			space->faulted = true;
		}
		(void)dma_space_health(space);
		if (error == 0) {
			entry->mapped = true;
			*iova = entry->base;
		}
	}
out:
	lck_mtx_unlock(&space->lock);
	return error;
}

int
hakios_dma_space_unmap(struct hakios_dma_space *space, hakios_dma_lease_t lease)
{
	struct dma_reservation *entry;
	int error;

	if (space == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&space->lock);
	error = dma_space_health(space);
	if (error != 0) {
		goto out;
	}
	entry = dma_find_lease(space, lease);
	if (entry == NULL) {
		error = ESTALE;
	} else if (!entry->mapped) {
		error = ENOENT;
	} else {
		error = hakios_dma_pt_unmap(space->table, entry->base, entry->page_count);
		if (error == ENOENT) {
			space->faulted = true;
		}
		(void)dma_space_health(space);
		if (error == 0) {
			entry->mapped = false;
		}
	}
out:
	lck_mtx_unlock(&space->lock);
	return error;
}

int
hakios_dma_space_release(struct hakios_dma_space *space, hakios_dma_lease_t *lease)
{
	struct dma_reservation **position, *entry;
	int error;

	if (space == NULL || lease == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&space->lock);
	error = dma_space_health(space);
	if (error != 0) {
		goto out;
	}
	position = &space->reservations;
	while (*position != NULL && (*position)->lease != *lease) {
		position = &(*position)->next;
	}
	entry = *position;
	if (entry == NULL) {
		error = ESTALE;
	} else if (entry->mapped) {
		error = EBUSY;
	} else {
		*position = entry->next;
		space->count--;
		kfree_type(struct dma_reservation, entry);
		*lease = 0;
	}
out:
	lck_mtx_unlock(&space->lock);
	return error;
}

int
hakios_dma_space_destroy(struct hakios_dma_space **space)
{
	struct hakios_dma_space *state;
	int error;

	if (space == NULL || *space == NULL) {
		return EINVAL;
	}
	state = *space;
	lck_mtx_lock(&state->lock);
	error = hakios_dma_pt_destroy(&state->table);
	if (error != 0) {
		state->faulted = true;
		lck_mtx_unlock(&state->lock);
		return error;
	}
	while (state->reservations != NULL) {
		struct dma_reservation *entry = state->reservations;
		state->reservations = entry->next;
		kfree_type(struct dma_reservation, entry);
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &dma_space_lock_group);
	kfree_type(struct hakios_dma_space, state);
	*space = NULL;
	return 0;
}

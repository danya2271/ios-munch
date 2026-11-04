#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.dma.endpoint.h>

struct endpoint_mapping {
	struct endpoint_mapping *next;
	struct hakios_dma_backing backing;
	hakios_dma_lease_t lease;
	void *source;
	size_t size;
	enum hakios_dma_direction direction;
	bool device_owned;
	bool coherent;
	bool mapped;
	bool released;
};

struct hakios_dma_endpoint {
	lck_mtx_t lock;
	struct hakios_dma_endpoint_operations operations;
	struct hakios_dma_endpoint_status status;
	struct hakios_dma_space *space;
	struct endpoint_mapping *mappings;
	void *owner;
	uint32_t dependents;
	bool attach_attempted;
	bool detached;
	bool held;
};

static LCK_GRP_DECLARE(endpoint_dma_group, "hakios-endpoint-dma");

static int
endpoint_error(int error)
{
	return error < 0 ? EIO : error;
}

static int
endpoint_fault(struct hakios_dma_endpoint *endpoint, int error)
{
	if (endpoint->status.fault == 0) {
		endpoint->status.fault = endpoint_error(error);
	}
	return endpoint->status.fault;
}

static int
endpoint_health(struct hakios_dma_endpoint *endpoint)
{
	if (endpoint->status.fault != 0) {
		return endpoint->status.fault;
	}
	if (endpoint->status.closed) {
		return ESHUTDOWN;
	}
	int error = hakios_pcie_endpoint_status(endpoint->status.endpoint);
	if (error == 0) {
		error = hakios_dma_space_status(endpoint->space);
	}
	return error == 0 ? 0 : endpoint_fault(endpoint, error);
}

static int
mapping_sync(struct hakios_dma_endpoint *endpoint, struct endpoint_mapping *mapping, bool for_device)
{
	if (!mapping->coherent && for_device == mapping->device_owned) {
		return EALREADY;
	}
	if (for_device && !mapping->coherent && mapping->direction != HAKIOS_DMA_FROM_DEVICE) {
		memcpy(mapping->backing.cpu, mapping->source, mapping->size);
	}
	int error = endpoint->operations.sync(endpoint->owner, &mapping->backing, mapping->direction, for_device);
	if (error != 0) {
		return endpoint_fault(endpoint, error);
	}
	if (!for_device && !mapping->coherent && mapping->direction != HAKIOS_DMA_TO_DEVICE) {
		memcpy(mapping->source, mapping->backing.cpu, mapping->size);
	}
	mapping->device_owned = for_device;
	return 0;
}

static struct endpoint_mapping *
mapping_find(struct hakios_dma_endpoint *endpoint, hakios_dma_lease_t lease)
{
	for (struct endpoint_mapping *mapping = endpoint->mappings; mapping != NULL; mapping = mapping->next) {
		if (mapping->lease == lease && lease != 0) {
			return mapping;
		}
	}
	return NULL;
}

static int
mapping_free(struct hakios_dma_endpoint *endpoint, struct endpoint_mapping *mapping)
{
	if (mapping->released) {
		return 0;
	}
	int error = endpoint_error(endpoint->operations.free(endpoint->owner, &mapping->backing));
	if (error == 0) {
		mapping->released = true;
	}
	return error;
}

int
hakios_dma_endpoint_create(const struct hakios_dma_endpoint_config *config,
    const struct hakios_dma_pt_operations *tables, const struct hakios_dma_endpoint_operations *operations,
    void *owner, struct hakios_dma_endpoint **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || config->endpoint == 0 || config->mask == UINT64_MAX ||
	    (config->mask & (config->mask + 1)) != 0 || config->space.iova_limit == 0 ||
	    config->space.iova_limit - 1 > config->mask || operations == NULL || owner == NULL ||
	    operations->version != HAKIOS_DMA_ENDPOINT_VERSION || operations->attach == NULL ||
	    operations->allocate == NULL || operations->sync == NULL || operations->free == NULL ||
	    operations->fence == NULL || operations->detach == NULL) {
		return EINVAL;
	}
	struct hakios_pcie_endpoint_resources resources;
	int error = hakios_pcie_endpoint_resources(config->endpoint, &resources);
	if (error != 0) {
		return error;
	}
	if (resources.domain != 0 || resources.requester_id != 0x100 || resources.stream_id != 0x1c01) {
		return ENODEV;
	}
	struct hakios_dma_endpoint *endpoint = kalloc_type(struct hakios_dma_endpoint, Z_WAITOK | Z_ZERO);
	if (endpoint == NULL) {
		return ENOMEM;
	}
	endpoint->owner = owner;
	endpoint->operations = *operations;
	endpoint->status.endpoint = config->endpoint;
	lck_mtx_init(&endpoint->lock, &endpoint_dma_group, LCK_ATTR_NULL);
	error = hakios_pcie_endpoint_hold(config->endpoint);
	if (error != 0) {
		lck_mtx_destroy(&endpoint->lock, &endpoint_dma_group);
		kfree_type(struct hakios_dma_endpoint, endpoint);
		return error;
	}
	endpoint->held = true;
	*result = endpoint;
	error = hakios_dma_space_create(&config->space, tables, owner, &endpoint->space);
	if (error != 0) {
		int drop = hakios_pcie_endpoint_drop(config->endpoint);
		if (drop != 0) {
			return endpoint_fault(endpoint, drop);
		}
		lck_mtx_destroy(&endpoint->lock, &endpoint_dma_group);
		kfree_type(struct hakios_dma_endpoint, endpoint);
		*result = NULL;
		return error;
	}
	struct hakios_dma_pt_info info;
	error = hakios_dma_space_get_info(endpoint->space, &info);
	for (uint32_t index = 0; error == 0 && index < config->space.tables.table_count; index++) {
		error = endpoint_error(tables->sync(owner, &config->space.tables.pages[index]));
	}
	if (error == 0) {
		endpoint->attach_attempted = true;
		error = endpoint_error(operations->attach(owner, config->endpoint, &resources, &info));
	}
	if (error != 0) {
		endpoint_fault(endpoint, error);
	}
	return error;
}

static int
endpoint_map(struct hakios_dma_endpoint *endpoint, void *source, size_t size, enum hakios_dma_direction direction,
    bool coherent, uint64_t alignment, hakios_dma_lease_t *lease, void **cpu, uint64_t *iova)
{
	if (lease != NULL) {
		*lease = 0;
	}
	if (cpu != NULL) {
		*cpu = NULL;
	}
	if (iova != NULL) {
		*iova = 0;
	}
	if (endpoint == NULL || lease == NULL || iova == NULL || size == 0 || alignment < HAKIOS_DMA_PT_PAGE_SIZE ||
	    (alignment & (alignment - 1)) != 0 || alignment > (uint64_t)HAKIOS_DMA_PT_PAGES_MAX * HAKIOS_DMA_PT_PAGE_SIZE ||
	    size > (uint64_t)HAKIOS_DMA_PT_PAGES_MAX * HAKIOS_DMA_PT_PAGE_SIZE ||
	    (!coherent && (source == NULL || (uintptr_t)source > UINTPTR_MAX - size)) ||
	    (unsigned)direction > HAKIOS_DMA_FROM_DEVICE || (coherent && cpu == NULL)) {
		return EINVAL;
	}
	lck_mtx_lock(&endpoint->lock);
	int error = endpoint_health(endpoint);
	if (error != 0) {
		goto out;
	}
	struct endpoint_mapping *mapping = kalloc_type(struct endpoint_mapping, Z_WAITOK | Z_ZERO);
	if (mapping == NULL) {
		error = ENOMEM;
		goto out;
	}
	mapping->source = source;
	mapping->size = size;
	mapping->direction = direction;
	mapping->coherent = coherent;
	mapping->next = endpoint->mappings;
	endpoint->mappings = mapping;
	endpoint->status.mappings++;
	uint32_t pages = (uint32_t)((size + HAKIOS_DMA_PT_PAGE_SIZE - 1) / HAKIOS_DMA_PT_PAGE_SIZE);
	error = endpoint_error(endpoint->operations.allocate(endpoint->owner, pages, coherent, &mapping->backing));
	if (error == 0 && (mapping->backing.token == NULL || mapping->backing.cpu == NULL ||
	    ((uintptr_t)mapping->backing.cpu & (alignment - 1)) != 0 ||
	    (uintptr_t)mapping->backing.cpu > UINTPTR_MAX - (uint64_t)pages * HAKIOS_DMA_PT_PAGE_SIZE ||
	    mapping->backing.count != pages || mapping->backing.pages == NULL ||
	    (coherent && !mapping->backing.coherent))) {
		error = EPROTO;
	}
	if (error != 0) {
		goto fault;
	}
	if (!coherent && (uintptr_t)source < (uintptr_t)mapping->backing.cpu + (size_t)pages * HAKIOS_DMA_PT_PAGE_SIZE &&
	    (uintptr_t)mapping->backing.cpu < (uintptr_t)source + size) {
		error = EINVAL;
		goto fault;
	}
	bzero(mapping->backing.cpu, (size_t)pages * HAKIOS_DMA_PT_PAGE_SIZE);
	error = mapping_sync(endpoint, mapping, true);
	if (error != 0) {
		goto fault;
	}
	error = hakios_dma_space_reserve(endpoint->space, pages, alignment, 0, &mapping->lease);
	if (error != 0) {
		goto fault;
	}
	uint32_t protection = HAKIOS_DMA_PT_READ | (mapping->backing.cacheable ? HAKIOS_DMA_PT_CACHE : 0);
	if (direction != HAKIOS_DMA_TO_DEVICE) {
		protection |= HAKIOS_DMA_PT_WRITE;
	}
	error = hakios_dma_space_map(endpoint->space, mapping->lease, mapping->backing.pages, pages, protection, iova);
	if (error != 0) {
		goto fault;
	}
	mapping->mapped = true;
	*lease = mapping->lease;
	if (cpu != NULL) {
		*cpu = mapping->backing.cpu;
	}
	goto out;
fault:
	error = endpoint_fault(endpoint, error);
out:
	lck_mtx_unlock(&endpoint->lock);
	return error;
}

int
hakios_dma_endpoint_alloc(struct hakios_dma_endpoint *endpoint, size_t size,
    hakios_dma_lease_t *lease, void **cpu, uint64_t *iova)
{
	return endpoint_map(endpoint, NULL, size, HAKIOS_DMA_BIDIRECTIONAL, true, HAKIOS_DMA_PT_PAGE_SIZE, lease, cpu, iova);
}

int
hakios_dma_endpoint_alloc_aligned(struct hakios_dma_endpoint *endpoint, size_t size, uint64_t alignment,
    hakios_dma_lease_t *lease, void **cpu, uint64_t *iova)
{
	return endpoint_map(endpoint, NULL, size, HAKIOS_DMA_BIDIRECTIONAL, true, alignment, lease, cpu, iova);
}

int
hakios_dma_endpoint_hold(struct hakios_dma_endpoint *endpoint)
{
	if (endpoint == NULL) { return EINVAL; }
	lck_mtx_lock(&endpoint->lock);
	int error = endpoint_health(endpoint);
	if (error == 0 && endpoint->dependents == UINT32_MAX) { error = EOVERFLOW; }
	if (error == 0) { endpoint->dependents++; }
	lck_mtx_unlock(&endpoint->lock);
	return error;
}

int
hakios_dma_endpoint_drop(struct hakios_dma_endpoint *endpoint)
{
	if (endpoint == NULL) { return EINVAL; }
	lck_mtx_lock(&endpoint->lock);
	int error = endpoint->dependents == 0 ? EINVAL : 0;
	if (error == 0) { endpoint->dependents--; }
	lck_mtx_unlock(&endpoint->lock);
	return error;
}

int
hakios_dma_endpoint_map(struct hakios_dma_endpoint *endpoint, void *cpu, size_t size,
    enum hakios_dma_direction direction, hakios_dma_lease_t *lease, uint64_t *iova)
{
	return endpoint_map(endpoint, cpu, size, direction, false, HAKIOS_DMA_PT_PAGE_SIZE, lease, NULL, iova);
}

int
hakios_dma_endpoint_sync(struct hakios_dma_endpoint *endpoint, hakios_dma_lease_t lease, bool for_device)
{
	if (endpoint == NULL || lease == 0) {
		return EINVAL;
	}
	lck_mtx_lock(&endpoint->lock);
	int error = endpoint_health(endpoint);
	struct endpoint_mapping *mapping = mapping_find(endpoint, lease);
	if (error == 0) {
		error = mapping == NULL ? ESTALE : mapping_sync(endpoint, mapping, for_device);
	}
	lck_mtx_unlock(&endpoint->lock);
	return error;
}

int
hakios_dma_endpoint_unmap(struct hakios_dma_endpoint *endpoint, hakios_dma_lease_t *lease)
{
	if (endpoint == NULL || lease == NULL || *lease == 0) {
		return EINVAL;
	}
	lck_mtx_lock(&endpoint->lock);
	int error = endpoint_health(endpoint);
	struct endpoint_mapping *mapping = mapping_find(endpoint, *lease);
	if (error != 0 || mapping == NULL) {
		error = error != 0 ? error : ESTALE;
		goto out;
	}
	if (mapping->device_owned && (error = mapping_sync(endpoint, mapping, false)) != 0) {
		goto out;
	}
	if ((error = hakios_dma_space_unmap(endpoint->space, mapping->lease)) != 0) {
		goto fault;
	}
	mapping->mapped = false;
	if ((error = mapping_free(endpoint, mapping)) != 0) {
		goto fault;
	}
	if ((error = hakios_dma_space_release(endpoint->space, &mapping->lease)) != 0) {
		goto fault;
	}
	struct endpoint_mapping **position = &endpoint->mappings;
	while (*position != mapping) {
		position = &(*position)->next;
	}
	*position = mapping->next;
	endpoint->status.mappings--;
	kfree_type(struct endpoint_mapping, mapping);
	*lease = 0;
	goto out;
fault:
	error = endpoint_fault(endpoint, error);
out:
	lck_mtx_unlock(&endpoint->lock);
	return error;
}

int
hakios_dma_endpoint_close(struct hakios_dma_endpoint *endpoint)
{
	if (endpoint == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&endpoint->lock);
	endpoint->status.closed = true;
	int error = 0;
	if (!endpoint->status.fenced) {
		error = endpoint_error(endpoint->operations.fence(endpoint->owner));
		if (error == 0) {
			endpoint->status.fenced = true;
		} else {
			endpoint_fault(endpoint, error);
		}
	}
	lck_mtx_unlock(&endpoint->lock);
	return error;
}

int
hakios_dma_endpoint_status(struct hakios_dma_endpoint *endpoint, struct hakios_dma_endpoint_status *status)
{
	if (endpoint == NULL || status == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&endpoint->lock);
	*status = endpoint->status;
	lck_mtx_unlock(&endpoint->lock);
	return 0;
}

int
hakios_dma_endpoint_destroy(struct hakios_dma_endpoint **endpoint)
{
	if (endpoint == NULL || *endpoint == NULL) {
		return EINVAL;
	}
	struct hakios_dma_endpoint *state = *endpoint;
	lck_mtx_lock(&state->lock);
	bool retained = state->dependents != 0;
	lck_mtx_unlock(&state->lock);
	if (retained) { return EBUSY; }
	int error = hakios_dma_endpoint_close(state);
	if (error != 0) {
		return error;
	}
	lck_mtx_lock(&state->lock);
	if (state->dependents != 0) { lck_mtx_unlock(&state->lock); return EBUSY; }
	if (state->attach_attempted && !state->detached) {
		error = endpoint_error(state->operations.detach(state->owner));
		if (error != 0) {
			goto out;
		}
		state->detached = true;
	}
	if (state->space != NULL && (error = hakios_dma_space_destroy(&state->space)) != 0) {
		goto out;
	}
	while (state->mappings != NULL) {
		struct endpoint_mapping *mapping = state->mappings;
		error = mapping_free(state, mapping);
		if (error != 0) {
			goto out;
		}
		state->mappings = mapping->next;
		state->status.mappings--;
		kfree_type(struct endpoint_mapping, mapping);
	}
	if (state->held) {
		error = hakios_pcie_endpoint_drop(state->status.endpoint);
		if (error == 0) { state->held = false; }
	}
out:
	if (error != 0) {
		endpoint_fault(state, error);
	}
	lck_mtx_unlock(&state->lock);
	if (error == 0) {
		lck_mtx_destroy(&state->lock, &endpoint_dma_group);
		kfree_type(struct hakios_dma_endpoint, state);
		*endpoint = NULL;
	}
	return error;
}

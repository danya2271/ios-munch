#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.msi.pool.h>

struct hakios_msi_pool {
	lck_mtx_t lock;
	struct hakios_msi_pool_operations operations;
	struct hakios_msi_route routes[HAKIOS_MSI_POOL_ROUTES_MAX];
	uint64_t doorbell;
	uint32_t count;
	uint32_t used;
	void *owner;
};

struct hakios_msi_allocation {
	struct hakios_msi_pool *pool;
	hakios_pcie_lease_t endpoint;
	void *mapping;
	uint64_t iova;
	uint32_t bank;
	int fault;
	bool quiet;
	bool released;
	bool closing;
	bool held;
};

static LCK_GRP_DECLARE(msi_pool_group, "hakios-msi-pool");

static int
route_error(int error)
{
	return error < 0 ? EIO : error;
}

int
hakios_msi_pool_create(const struct hakios_msi_pool_config *config,
    const struct hakios_msi_pool_operations *operations, void *owner, struct hakios_msi_pool **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || operations == NULL || owner == NULL || config->routes == NULL ||
	    config->count == 0 || config->count > HAKIOS_MSI_POOL_ROUTES_MAX || (config->count & 31) != 0 ||
	    config->dt_spi_base > 988 - config->count || (config->dt_spi_base & 31) != 0 ||
	    config->doorbell_physical == 0 || (config->doorbell_physical & 3) != 0 ||
	    operations->version != HAKIOS_MSI_POOL_VERSION || operations->claim == NULL || operations->map == NULL ||
	    operations->quiesce == NULL || operations->unmap == NULL || operations->release == NULL) {
		return EINVAL;
	}
	for (uint32_t index = 0; index < config->count; index++) {
		const struct hakios_msi_route *route = &config->routes[index];
		uint32_t vector = index & 31;
		const struct hakios_msi_route *bank = &config->routes[index - vector];
		if (route->token == NULL || route->intid != config->dt_spi_base + 32 + index ||
		    bank->data > UINT16_MAX - 31 || (bank->data & 31) != 0 ||
		    route->data != bank->data + vector) {
			return EINVAL;
		}
		for (uint32_t prior = 0; prior < index; prior++) {
			if (route->token == config->routes[prior].token || route->data == config->routes[prior].data) {
				return EINVAL;
			}
		}
	}
	struct hakios_msi_pool *pool = kalloc_type(struct hakios_msi_pool, Z_WAITOK | Z_ZERO);
	if (pool == NULL) {
		return ENOMEM;
	}
	pool->operations = *operations;
	pool->owner = owner;
	pool->doorbell = config->doorbell_physical;
	pool->count = config->count;
	memcpy(pool->routes, config->routes, config->count * sizeof(*pool->routes));
	lck_mtx_init(&pool->lock, &msi_pool_group, LCK_ATTR_NULL);
	*result = pool;
	return 0;
}

int
hakios_msi_pool_allocate(struct hakios_msi_pool *pool, hakios_pcie_lease_t endpoint,
    struct hakios_msi_allocation **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (pool == NULL || endpoint == 0) {
		return EINVAL;
	}
	struct hakios_pcie_endpoint_resources resources;
	int error = hakios_pcie_endpoint_resources(endpoint, &resources);
	if (error != 0) {
		return error;
	}
	if (resources.domain != 0 || resources.requester_id != 0x100 || resources.stream_id != 0x1c01) {
		return ENODEV;
	}
	lck_mtx_lock(&pool->lock);
	uint32_t bank;
	for (bank = 0; bank < pool->count / 32; bank++) {
		if ((pool->used & (1u << bank)) == 0) {
			break;
		}
	}
	if (bank == pool->count / 32) {
		error = ENOSPC;
		goto out;
	}
	struct hakios_msi_allocation *allocation = kalloc_type(struct hakios_msi_allocation, Z_WAITOK | Z_ZERO);
	if (allocation == NULL) {
		error = ENOMEM;
		goto out;
	}
	allocation->pool = pool;
	allocation->endpoint = endpoint;
	allocation->bank = bank;
	error = hakios_pcie_endpoint_hold(endpoint);
	if (error != 0) {
		kfree_type(struct hakios_msi_allocation, allocation);
		goto out;
	}
	allocation->held = true;
	pool->used |= 1u << bank;
	*result = allocation;
	error = route_error(pool->operations.claim(pool->owner, endpoint, &pool->routes[bank * 32], 32));
	if (error == 0) {
		error = route_error(pool->operations.map(pool->owner, endpoint, pool->doorbell, &allocation->mapping, &allocation->iova));
		if (error == 0 && (allocation->mapping == NULL || allocation->iova == 0 ||
		    (allocation->iova & 4095) != (pool->doorbell & 4095))) {
			error = EPROTO;
		}
	}
	allocation->fault = error;
out:
	lck_mtx_unlock(&pool->lock);
	return error;
}

int
hakios_msi_allocation_config(struct hakios_msi_allocation *allocation, hakios_pcie_lease_t endpoint,
    uint16_t capability, struct hakios_msi_config *config)
{
	if (config == NULL) {
		return EINVAL;
	}
	bzero(config, sizeof(*config));
	if (allocation == NULL) {
		return EINVAL;
	}
	struct hakios_msi_pool *pool = allocation->pool;
	lck_mtx_lock(&pool->lock);
	int error = allocation->endpoint != endpoint ? ESTALE : allocation->fault != 0 ? allocation->fault :
	    allocation->closing ? ESHUTDOWN : hakios_pcie_endpoint_status(endpoint);
	if (error == 0) {
		config->doorbell_iova = allocation->iova;
		config->data = (uint16_t)pool->routes[allocation->bank * 32].data;
		config->capability = capability;
		for (uint32_t index = 0; index < 32; index++) {
			config->intids[index] = pool->routes[allocation->bank * 32 + index].intid;
		}
	}
	lck_mtx_unlock(&pool->lock);
	return error;
}

int
hakios_msi_allocation_route(struct hakios_msi_allocation *allocation, uint32_t vector, struct hakios_msi_route *route)
{
	if (route == NULL) {
		return EINVAL;
	}
	bzero(route, sizeof(*route));
	if (allocation == NULL || vector >= 32) {
		return EINVAL;
	}
	struct hakios_msi_pool *pool = allocation->pool;
	lck_mtx_lock(&pool->lock);
	int error = allocation->fault != 0 ? allocation->fault : allocation->closing ? ESHUTDOWN :
	    hakios_pcie_endpoint_status(allocation->endpoint);
	if (error == 0) {
		*route = pool->routes[allocation->bank * 32 + vector];
	}
	lck_mtx_unlock(&pool->lock);
	return error;
}

int
hakios_msi_allocation_destroy(struct hakios_msi_allocation **allocation)
{
	if (allocation == NULL || *allocation == NULL) {
		return EINVAL;
	}
	struct hakios_msi_allocation *state = *allocation;
	struct hakios_msi_pool *pool = state->pool;
	const struct hakios_msi_route *routes = &pool->routes[state->bank * 32];
	lck_mtx_lock(&pool->lock);
	state->closing = true;
	int error = 0;
	if (!state->quiet) {
		error = route_error(pool->operations.quiesce(pool->owner, routes, 32));
		if (error != 0) {
			goto out;
		}
		state->quiet = true;
	}
	if (state->mapping != NULL) {
		error = route_error(pool->operations.unmap(pool->owner, &state->mapping));
		if (error != 0) {
			goto out;
		}
		if (state->mapping != NULL) {
			error = EPROTO;
			goto out;
		}
	}
	if (!state->released) {
		error = route_error(pool->operations.release(pool->owner, routes, 32));
		if (error != 0) {
			goto out;
		}
		state->released = true;
	}
	if (state->held) {
		error = hakios_pcie_endpoint_drop(state->endpoint);
		if (error != 0) { goto out; }
		state->held = false;
	}
	pool->used &= ~(1u << state->bank);
out:
	if (error != 0) {
		state->fault = error;
	}
	lck_mtx_unlock(&pool->lock);
	if (error == 0) {
		kfree_type(struct hakios_msi_allocation, state);
		*allocation = NULL;
	}
	return error;
}

int
hakios_msi_pool_destroy(struct hakios_msi_pool **pool)
{
	if (pool == NULL || *pool == NULL) {
		return EINVAL;
	}
	struct hakios_msi_pool *state = *pool;
	lck_mtx_lock(&state->lock);
	int error = state->used != 0 ? EBUSY : 0;
	lck_mtx_unlock(&state->lock);
	if (error == 0) {
		lck_mtx_destroy(&state->lock, &msi_pool_group);
		kfree_type(struct hakios_msi_pool, state);
		*pool = NULL;
	}
	return error;
}

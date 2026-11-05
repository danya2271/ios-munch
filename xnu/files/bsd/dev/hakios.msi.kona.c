#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.gic.h>
#include <sys/hakios.msi.kona.h>
#include <sys/hakios.msi.pool.h>
#include <sys/hakios.qca6390.h>

#define KONA_MSI_DTS_SPI_BASE 768u
#define KONA_GIC_SPI_TRANSLATION 32u
#define KONA_MSI_FIRST_INTID (KONA_MSI_DTS_SPI_BASE + KONA_GIC_SPI_TRANSLATION)
#define KONA_MSI_DOORBELL 0x17a10040ull
#define KONA_MSI_PAGE_SIZE 4096u

struct kona_msi_handle;
struct kona_msi_route {
	struct hakios_msi_kona *owner;
	uint32_t vector;
	struct kona_msi_handle *handle;
	hakios_qca6390_irq_handler_t handler;
	void *context;
};

struct kona_msi_handle {
	struct hakios_msi_kona *owner;
	hakios_pcie_lease_t endpoint;
	struct hakios_msi_allocation *allocation;
	struct hakios_msi *msi;
	bool attached[HAKIOS_MSI_VECTORS];
};

struct hakios_msi_kona {
	lck_mtx_t lock;
	struct hakios_msi_kona_operations operations;
	struct hakios_msi_pool *pool;
	struct kona_msi_route routes[HAKIOS_MSI_VECTORS];
	gic_v3_spi_lease_t gic;
	struct kona_msi_handle *active;
	void *context;
	bool registered;
	bool claimed;
	bool registration_busy;
};

static LCK_GRP_DECLARE(kona_msi_group, "hakios-msi-kona");

static void kona_msi_interrupt(void *context, uint32_t intid);

static int
kona_gic_error(kern_return_t error)
{
	switch (error) {
	case KERN_SUCCESS:
		return 0;
	case KERN_INVALID_ARGUMENT:
		return EINVAL;
	case KERN_NAME_EXISTS:
		return EBUSY;
	case KERN_RESOURCE_SHORTAGE:
		return ENOSPC;
	case KERN_OPERATION_TIMED_OUT:
		return ETIMEDOUT;
	default:
		return EIO;
	}
}

static int
kona_pool_claim(void *context, hakios_pcie_lease_t endpoint, const struct hakios_msi_route *routes,
    uint32_t count)
{
	struct hakios_msi_kona *owner = context;
	struct hakios_pcie_endpoint_resources resources;
	int error;

	if (routes == NULL || count != HAKIOS_MSI_VECTORS || owner->claimed || owner->active == NULL) {
		return EINVAL;
	}
	error = hakios_pcie_endpoint_resources(endpoint, &resources);
	if (error != 0) {
		return error;
	}
	if (resources.domain != 0 || resources.requester_id != 0x100 || resources.stream_id != 0x1c01) {
		return ENODEV;
	}
	for (uint32_t vector = 0; vector < count; vector++) {
		if (routes[vector].token != &owner->routes[vector] ||
		    routes[vector].intid != KONA_MSI_FIRST_INTID + vector ||
		    routes[vector].data != KONA_MSI_FIRST_INTID + vector) {
			return EPROTO;
		}
	}
	error = kona_gic_error(gic_v3_spi_bank_reserve(KONA_MSI_FIRST_INTID, HAKIOS_MSI_VECTORS,
	    &owner->gic));
	if (error != 0) {
		return error;
	}
	owner->claimed = true;
	for (uint32_t vector = 0; vector < count; vector++) {
		owner->routes[vector].handle = owner->active;
		error = kona_gic_error(gic_v3_spi_bank_attach(owner->gic, vector, true,
		    kona_msi_interrupt, &owner->routes[vector]));
		if (error != 0) {
			return error;
		}
	}
	return 0;
}

static int
kona_pool_map(void *context, hakios_pcie_lease_t endpoint, uint64_t physical,
    void **mapping, uint64_t *iova)
{
	struct hakios_msi_kona *owner = context;

	if (mapping == NULL || iova == NULL || physical != KONA_MSI_DOORBELL) {
		return EINVAL;
	}
	*mapping = NULL;
	*iova = 0;
	return owner->operations.map_doorbell(owner->context, endpoint, physical,
	    KONA_MSI_PAGE_SIZE, mapping, iova);
}

static int
kona_pool_quiesce(void *context, const struct hakios_msi_route *routes, uint32_t count)
{
	struct hakios_msi_kona *owner = context;

	if (routes == NULL || count != HAKIOS_MSI_VECTORS) {
		return EINVAL;
	}
	if (!owner->claimed) { return 0; }
	return kona_gic_error(gic_v3_spi_bank_quiesce(owner->gic));
}

static int
kona_pool_unmap(void *context, void **mapping)
{
	struct hakios_msi_kona *owner = context;

	if (owner->active == NULL) { return ESTALE; }
	return owner->operations.unmap_doorbell(owner->context, owner->active->endpoint, mapping);
}

static int
kona_pool_release(void *context, const struct hakios_msi_route *routes, uint32_t count)
{
	struct hakios_msi_kona *owner = context;
	int error;

	if (routes == NULL || count != HAKIOS_MSI_VECTORS) {
		return EINVAL;
	}
	if (!owner->claimed) { return 0; }
	error = kona_gic_error(gic_v3_spi_bank_release(&owner->gic));
	if (error == 0) {
		owner->claimed = false;
		for (uint32_t vector = 0; vector < HAKIOS_MSI_VECTORS; vector++) {
			owner->routes[vector].handle = NULL;
			owner->routes[vector].handler = NULL;
			owner->routes[vector].context = NULL;
		}
	}
	return error;
}

static const struct hakios_msi_pool_operations kona_pool_operations = {
	.version = HAKIOS_MSI_POOL_VERSION,
	.claim = kona_pool_claim,
	.map = kona_pool_map,
	.quiesce = kona_pool_quiesce,
	.unmap = kona_pool_unmap,
	.release = kona_pool_release,
};

static int
kona_msi_read(void *context, uint16_t offset, uint8_t width, uint32_t *value)
{
	struct kona_msi_handle *handle = context;

	return hakios_pcie_config_read(handle->endpoint, offset, width, value);
}

static int
kona_msi_write(void *context, uint16_t offset, uint8_t width, uint32_t value)
{
	struct kona_msi_handle *handle = context;

	return hakios_pcie_config_write(handle->endpoint, offset, width, value);
}

static int
kona_msi_arm(void *context)
{
	struct kona_msi_handle *handle = context;

	return handle->owner->claimed && handle->allocation != NULL ? 0 : ENXIO;
}

static int
kona_msi_quiesce(void *context)
{
	struct kona_msi_handle *handle = context;

	return kona_gic_error(gic_v3_spi_bank_quiesce(handle->owner->gic));
}

static const struct hakios_msi_operations kona_msi_operations = {
	.version = HAKIOS_MSI_VERSION,
	.read = kona_msi_read,
	.write = kona_msi_write,
	.arm = kona_msi_arm,
	.quiesce = kona_msi_quiesce,
};

static void
kona_msi_interrupt(void *context, uint32_t intid)
{
	struct kona_msi_route *route = context;
	hakios_qca6390_irq_handler_t handler;

	if (route == NULL) { return; }
	handler = __atomic_load_n(&route->handler, __ATOMIC_ACQUIRE);
	if (handler != NULL &&
	    intid == KONA_MSI_FIRST_INTID + route->vector) {
		handler(__atomic_load_n(&route->context, __ATOMIC_RELAXED), intid);
	}
}

static int
kona_msi_allocate(void *context, hakios_pcie_lease_t endpoint, uint16_t capability,
    struct hakios_qca6390_msi_info *info, void **result)
{
	struct hakios_msi_kona *owner = context;
	struct kona_msi_handle *handle;
	struct hakios_msi_config config;
	int error;

	if (info == NULL || result == NULL) {
		return EINVAL;
	}
	bzero(info, sizeof(*info));
	*result = NULL;
	lck_mtx_lock(&owner->lock);
	if (!owner->registered || owner->active != NULL) {
		error = owner->registered ? EBUSY : ENXIO;
		goto out;
	}
	handle = kalloc_type(struct kona_msi_handle, Z_WAITOK | Z_ZERO);
	if (handle == NULL) {
		error = ENOMEM;
		goto out;
	}
	handle->owner = owner;
	handle->endpoint = endpoint;
	owner->active = handle;
	*result = handle;
	error = hakios_msi_pool_allocate(owner->pool, endpoint, &handle->allocation);
	if (error != 0) {
		goto out;
	}
	error = hakios_msi_allocation_config(handle->allocation, endpoint, capability, &config);
	if (error != 0) {
		goto out;
	}
	error = hakios_msi_create(&config, &kona_msi_operations, handle, &handle->msi);
	if (error != 0) {
		goto out;
	}
	error = hakios_msi_start(handle->msi);
	if (error != 0) {
		goto out;
	}
	info->address = config.doorbell_iova;
	info->data = config.data;
	info->first_intid = config.intids[0];
out:
	lck_mtx_unlock(&owner->lock);
	return error;
}

static int
kona_msi_attach(void *context, void *opaque, uint32_t vector,
    hakios_qca6390_irq_handler_t handler, void *handler_context)
{
	struct hakios_msi_kona *owner = context;
	struct kona_msi_handle *handle = opaque;
	int error = 0;

	if (handle == NULL || handler == NULL || vector >= HAKIOS_MSI_VECTORS) {
		return EINVAL;
	}
	lck_mtx_lock(&owner->lock);
	struct kona_msi_route *route = &owner->routes[vector];
	if (owner->active != handle || !owner->claimed || handle->attached[vector] ||
	    route->handle != handle) {
		error = ESTALE;
	} else {
		__atomic_store_n(&route->context, handler_context, __ATOMIC_RELAXED);
		__atomic_store_n(&route->handler, handler, __ATOMIC_RELEASE);
		handle->attached[vector] = true;
	}
	lck_mtx_unlock(&owner->lock);
	return error;
}

static int
kona_msi_enable(void *context, void *opaque, uint32_t vector, bool enable)
{
	struct hakios_msi_kona *owner = context;
	struct kona_msi_handle *handle = opaque;

	if (handle == NULL || vector >= HAKIOS_MSI_VECTORS) {
		return EINVAL;
	}
	lck_mtx_lock(&owner->lock);
	int error = owner->active != handle || !handle->attached[vector] ? ESTALE :
	    kona_gic_error(gic_v3_spi_bank_enable(owner->gic, vector, enable));
	lck_mtx_unlock(&owner->lock);
	return error;
}

static int
kona_msi_detach_all(void *context, void *opaque)
{
	struct hakios_msi_kona *owner = context;
	struct kona_msi_handle *handle = opaque;

	if (handle == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&owner->lock);
	int error = owner->active != handle ? ESTALE :
	    kona_gic_error(gic_v3_spi_bank_quiesce(owner->gic));
	lck_mtx_unlock(&owner->lock);
	return error;
}

static uint64_t
kona_msi_count(void *context, void *opaque, uint32_t vector)
{
	struct hakios_msi_kona *owner = context;
	struct kona_msi_handle *handle = opaque;
	uint64_t count = 0;

	if (handle == NULL || vector >= HAKIOS_MSI_VECTORS) {
		return 0;
	}
	lck_mtx_lock(&owner->lock);
	if (owner->active == handle) {
		(void)gic_v3_spi_bank_count(owner->gic, vector, &count);
	}
	lck_mtx_unlock(&owner->lock);
	return count;
}

static int
kona_msi_release(void *context, void **opaque)
{
	struct hakios_msi_kona *owner = context;
	struct kona_msi_handle *handle;
	int error;

	if (opaque == NULL || *opaque == NULL) {
		return EINVAL;
	}
	handle = *opaque;
	lck_mtx_lock(&owner->lock);
	if (owner->active != handle) {
		error = ESTALE;
		goto out;
	}
	if (handle->msi != NULL && (error = hakios_msi_destroy(&handle->msi)) != 0) {
		goto out;
	}
	if (handle->allocation != NULL &&
	    (error = hakios_msi_allocation_destroy(&handle->allocation)) != 0) {
		goto out;
	}
	owner->active = NULL;
	kfree_type(struct kona_msi_handle, handle);
	*opaque = NULL;
	error = 0;
out:
	lck_mtx_unlock(&owner->lock);
	return error;
}

static const struct hakios_qca6390_msi_provider kona_msi_provider = {
	.version = HAKIOS_QCA6390_MSI_PROVIDER_VERSION,
	.allocate = kona_msi_allocate,
	.attach = kona_msi_attach,
	.enable = kona_msi_enable,
	.detach_all = kona_msi_detach_all,
	.count = kona_msi_count,
	.release = kona_msi_release,
};

int
hakios_msi_kona_create(const struct hakios_msi_kona_operations *operations, void *context,
    struct hakios_msi_kona **result)
{
	struct hakios_msi_kona *owner;
	struct hakios_msi_pool_config config;
	struct hakios_msi_route routes[HAKIOS_MSI_VECTORS];
	int error;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (operations == NULL || context == NULL || operations->version != HAKIOS_MSI_KONA_VERSION ||
	    operations->map_doorbell == NULL || operations->unmap_doorbell == NULL) {
		return EINVAL;
	}
	owner = kalloc_type(struct hakios_msi_kona, Z_WAITOK | Z_ZERO);
	if (owner == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&owner->lock, &kona_msi_group, LCK_ATTR_NULL);
	owner->operations = *operations;
	owner->context = context;
	for (uint32_t vector = 0; vector < HAKIOS_MSI_VECTORS; vector++) {
		owner->routes[vector] = (struct kona_msi_route) { .owner = owner, .vector = vector };
		routes[vector] = (struct hakios_msi_route) {
			.token = &owner->routes[vector],
			.intid = KONA_MSI_FIRST_INTID + vector,
			.data = KONA_MSI_FIRST_INTID + vector,
		};
	}
	config.routes = routes;
	config.count = HAKIOS_MSI_VECTORS;
	config.dt_spi_base = KONA_MSI_DTS_SPI_BASE;
	config.doorbell_physical = KONA_MSI_DOORBELL;
	error = hakios_msi_pool_create(&config, &kona_pool_operations, owner, &owner->pool);
	if (error != 0) {
		lck_mtx_destroy(&owner->lock, &kona_msi_group);
		kfree_type(struct hakios_msi_kona, owner);
		return error;
	}
	*result = owner;
	return 0;
}

int
hakios_msi_kona_activate(struct hakios_msi_kona *owner)
{
	int error;

	if (owner == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&owner->lock);
	if (owner->registered || owner->registration_busy) {
		lck_mtx_unlock(&owner->lock);
		return EBUSY;
	}
	owner->registration_busy = true;
	lck_mtx_unlock(&owner->lock);
	error = hakios_qca6390_msi_register(&kona_msi_provider, owner);
	lck_mtx_lock(&owner->lock);
	if (error == 0) {
		owner->registered = true;
	}
	owner->registration_busy = false;
	lck_mtx_unlock(&owner->lock);
	return error;
}

int
hakios_msi_kona_deactivate(struct hakios_msi_kona *owner)
{
	int error;

	if (owner == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&owner->lock);
	if (!owner->registered) {
		lck_mtx_unlock(&owner->lock);
		return 0;
	}
	if (owner->registration_busy) {
		lck_mtx_unlock(&owner->lock);
		return EBUSY;
	}
	owner->registration_busy = true;
	lck_mtx_unlock(&owner->lock);
	error = hakios_qca6390_msi_unregister(owner);
	lck_mtx_lock(&owner->lock);
	if (error == 0) {
		owner->registered = false;
	}
	owner->registration_busy = false;
	lck_mtx_unlock(&owner->lock);
	return error;
}

int
hakios_msi_kona_destroy(struct hakios_msi_kona **owner)
{
	if (owner == NULL || *owner == NULL) {
		return EINVAL;
	}
	struct hakios_msi_kona *state = *owner;
	int error = hakios_msi_kona_deactivate(state);
	if (error == 0) {
		error = hakios_msi_pool_destroy(&state->pool);
	}
	if (error != 0) {
		return error;
	}
	lck_mtx_destroy(&state->lock, &kona_msi_group);
	kfree_type(struct hakios_msi_kona, state);
	*owner = NULL;
	return 0;
}

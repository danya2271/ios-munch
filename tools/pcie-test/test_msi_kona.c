#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/hakios.msi.kona.h>
#include <sys/hakios.qca6390.h>
#include <arm64/gic_v3_spi.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls, pcie_test_fail_allocation_at;

static uint8_t config_space[256];
static const struct hakios_qca6390_msi_provider *registered_provider;
static void *registered_context;
static gic_v3_spi_handler_t gic_handlers[32];
static void *gic_contexts[32];
static bool gic_enabled[32];
static uint64_t gic_counts[32];
static uint64_t active_gic_lease;
static void *doorbell_mapping;
static unsigned holds, maps, unmaps, route_attaches;
static int map_error, unmap_error, attach_error;
static unsigned attach_error_vector = UINT32_MAX;

int
hakios_pcie_endpoint_status(hakios_pcie_lease_t endpoint)
{
	return endpoint == 7 ? 0 : ESTALE;
}

int
hakios_pcie_endpoint_resources(hakios_pcie_lease_t endpoint,
    struct hakios_pcie_endpoint_resources *resources)
{
	if (resources == NULL || endpoint != 7) {
		return EINVAL;
	}
	*resources = (struct hakios_pcie_endpoint_resources) {
		.domain = 0,
		.requester_id = 0x100,
		.stream_id = 0x1c01,
		.bar0_address = 0x60300000,
		.bar0_size = 0x100000,
	};
	return 0;
}

int hakios_pcie_endpoint_hold(hakios_pcie_lease_t endpoint)
{ assert(endpoint == 7); holds++; return 0; }
int hakios_pcie_endpoint_drop(hakios_pcie_lease_t endpoint)
{ assert(endpoint == 7 && holds != 0); holds--; return 0; }

int
hakios_pcie_config_read(hakios_pcie_lease_t endpoint, uint16_t offset, uint8_t width, uint32_t *value)
{
	if (endpoint != 7 || value == NULL || (width != 1 && width != 2 && width != 4) ||
	    offset > sizeof(config_space) - width) {
		return EINVAL;
	}
	*value = 0;
	for (unsigned byte = 0; byte < width; byte++) {
		*value |= (uint32_t)config_space[offset + byte] << (8 * byte);
	}
	return 0;
}

int
hakios_pcie_config_write(hakios_pcie_lease_t endpoint, uint16_t offset, uint8_t width, uint32_t value)
{
	if (endpoint != 7 || (width != 1 && width != 2 && width != 4) ||
	    offset > sizeof(config_space) - width) {
		return EINVAL;
	}
	for (unsigned byte = 0; byte < width; byte++) {
		config_space[offset + byte] = (uint8_t)(value >> (8 * byte));
	}
	return 0;
}

int
hakios_qca6390_msi_register(const struct hakios_qca6390_msi_provider *provider, void *context)
{
	if (registered_provider != NULL || provider == NULL || context == NULL) {
		return EBUSY;
	}
	registered_provider = provider;
	registered_context = context;
	return 0;
}

int
hakios_qca6390_msi_unregister(void *context)
{
	if (registered_provider == NULL || registered_context != context || active_gic_lease != 0) {
		return EBUSY;
	}
	registered_provider = NULL;
	registered_context = NULL;
	return 0;
}

kern_return_t
gic_v3_spi_bank_reserve(uint32_t first, uint32_t count, gic_v3_spi_lease_t *lease)
{
	if (first != 800 || count != 32 || lease == NULL || active_gic_lease != 0) {
		return KERN_NAME_EXISTS;
	}
	active_gic_lease = 19;
	*lease = active_gic_lease;
	return KERN_SUCCESS;
}

kern_return_t
gic_v3_spi_bank_attach(gic_v3_spi_lease_t lease, uint32_t vector, bool edge,
    gic_v3_spi_handler_t handler, void *context)
{
	if (lease != active_gic_lease || vector >= 32 || !edge || handler == NULL) {
		return KERN_INVALID_ARGUMENT;
	}
	route_attaches++;
	if (vector == attach_error_vector) {
		return attach_error == 0 ? KERN_FAILURE : attach_error;
	}
	if (gic_handlers[vector] != NULL) {
		return KERN_NAME_EXISTS;
	}
	gic_handlers[vector] = handler;
	gic_contexts[vector] = context;
	return KERN_SUCCESS;
}

kern_return_t
gic_v3_spi_bank_intid(gic_v3_spi_lease_t lease, uint32_t vector, uint32_t *intid)
{
	if (lease != active_gic_lease || vector >= 32 || intid == NULL) {
		return KERN_INVALID_ARGUMENT;
	}
	*intid = 800 + vector;
	return KERN_SUCCESS;
}

kern_return_t
gic_v3_spi_bank_count(gic_v3_spi_lease_t lease, uint32_t vector, uint64_t *count)
{
	if (lease != active_gic_lease || vector >= 32 || count == NULL) {
		return KERN_INVALID_ARGUMENT;
	}
	*count = gic_counts[vector];
	return KERN_SUCCESS;
}

kern_return_t
gic_v3_spi_bank_enable(gic_v3_spi_lease_t lease, uint32_t vector, bool enable)
{
	if (lease != active_gic_lease || vector >= 32 || gic_handlers[vector] == NULL) {
		return KERN_INVALID_ARGUMENT;
	}
	gic_enabled[vector] = enable;
	return KERN_SUCCESS;
}

kern_return_t
gic_v3_spi_bank_quiesce(gic_v3_spi_lease_t lease)
{
	if (lease != active_gic_lease) {
		return KERN_INVALID_ARGUMENT;
	}
	for (unsigned vector = 0; vector < 32; vector++) {
		gic_enabled[vector] = false;
	}
	return KERN_SUCCESS;
}

kern_return_t
gic_v3_spi_bank_release(gic_v3_spi_lease_t *lease)
{
	if (lease == NULL || *lease != active_gic_lease) {
		return KERN_INVALID_ARGUMENT;
	}
	memset(gic_handlers, 0, sizeof(gic_handlers));
	memset(gic_contexts, 0, sizeof(gic_contexts));
	memset(gic_enabled, 0, sizeof(gic_enabled));
	active_gic_lease = 0;
	*lease = 0;
	return KERN_SUCCESS;
}

static int
map_doorbell(void *context, hakios_pcie_lease_t endpoint, uint64_t physical, size_t size,
    void **mapping, uint64_t *iova)
{
	assert(context == &doorbell_mapping && endpoint == 7 && physical == 0x17a10040 && size == 4096);
	maps++;
	if (map_error != 0) {
		return map_error;
	}
	doorbell_mapping = (void *)(uintptr_t)1;
	*mapping = doorbell_mapping;
	*iova = 0x90000040;
	return 0;
}

static int
unmap_doorbell(void *context, hakios_pcie_lease_t endpoint, void **mapping)
{
	assert(context == &doorbell_mapping && endpoint == 7 && mapping != NULL);
	unmaps++;
	if (unmap_error != 0) {
		return unmap_error;
	}
	assert(*mapping == doorbell_mapping);
	*mapping = NULL;
	doorbell_mapping = NULL;
	return 0;
}

static unsigned delivered[32];
static void
irq(void *context, uint32_t intid)
{
	unsigned vector = (unsigned)(uintptr_t)context;
	assert(vector < 32 && intid == 800 + vector);
	delivered[vector]++;
}

static void
fire(unsigned vector)
{
	assert(vector < 32 && gic_enabled[vector] && gic_handlers[vector] != NULL);
	gic_counts[vector]++;
	gic_handlers[vector](gic_contexts[vector], 800 + vector);
}

static void
reset(void)
{
	assert(active_gic_lease == 0 && registered_provider == NULL && holds == 0 && doorbell_mapping == NULL);
	memset(config_space, 0, sizeof(config_space));
	config_space[0x40] = 5;
	config_space[0x42] = 0x0a;
	memset(delivered, 0, sizeof(delivered));
	memset(gic_handlers, 0, sizeof(gic_handlers));
	memset(gic_contexts, 0, sizeof(gic_contexts));
	memset(gic_enabled, 0, sizeof(gic_enabled));
	memset(gic_counts, 0, sizeof(gic_counts));
	registered_context = NULL;
	maps = unmaps = route_attaches = 0;
	map_error = unmap_error = attach_error = 0;
	attach_error_vector = UINT32_MAX;
}

static struct hakios_msi_kona *
create_owner(void)
{
	const struct hakios_msi_kona_operations operations = {
		.version = HAKIOS_MSI_KONA_VERSION,
		.map_doorbell = map_doorbell,
		.unmap_doorbell = unmap_doorbell,
	};
	struct hakios_msi_kona *owner = NULL;
	assert(hakios_msi_kona_create(&operations, &doorbell_mapping, &owner) == 0);
	assert(owner != NULL && hakios_msi_kona_activate(owner) == 0);
	assert(registered_provider != NULL && registered_context == owner);
	return owner;
}

static void
test_lifecycle(void)
{
	reset();
	struct hakios_msi_kona *owner = create_owner();
	struct hakios_qca6390_msi_info info;
	void *handle = NULL;
	assert(registered_provider->allocate(registered_context, 7, 0x40, &info, &handle) == 0);
	assert(handle != NULL && info.address == 0x90000040 && info.data == 800 && info.first_intid == 800);
	assert(route_attaches == 32 && maps == 1 && holds == 1 && config_space[0x42] == 0x5b);
	for (uint32_t vector = 0; vector < 32; vector++) {
		assert(!gic_enabled[vector]);
		assert(registered_provider->attach(registered_context, handle, vector, irq,
		    (void *)(uintptr_t)vector) == 0);
	}
	for (uint32_t vector = 0; vector < 32; vector++) {
		assert(registered_provider->enable(registered_context, handle, vector, true) == 0);
		fire(vector);
		assert(delivered[vector] == 1 && registered_provider->count(registered_context, handle, vector) == 1);
	}
	assert(registered_provider->detach_all(registered_context, handle) == 0);
	for (uint32_t vector = 0; vector < 32; vector++) {
		assert(!gic_enabled[vector]);
	}
	assert(registered_provider->release(registered_context, &handle) == 0 && handle == NULL);
	assert(config_space[0x42] == 0x0a && doorbell_mapping == NULL && holds == 0 && unmaps == 1);
	assert(hakios_msi_kona_deactivate(owner) == 0 && registered_provider == NULL);
	assert(hakios_msi_kona_destroy(&owner) == 0 && owner == NULL);
}

static void
test_partial_cleanup(void)
{
	reset();
	struct hakios_msi_kona *owner = create_owner();
	struct hakios_qca6390_msi_info info;
	void *handle = NULL;
	map_error = EIO;
	assert(registered_provider->allocate(registered_context, 7, 0x40, &info, &handle) == EIO);
	assert(handle != NULL && active_gic_lease != 0 && holds == 1 && doorbell_mapping == NULL);
	assert(registered_provider->release(registered_context, &handle) == 0 && handle == NULL);
	assert(active_gic_lease == 0 && holds == 0 && unmaps == 0);
	map_error = 0;
	attach_error_vector = 13;
	attach_error = KERN_OPERATION_TIMED_OUT;
	handle = NULL;
	assert(registered_provider->allocate(registered_context, 7, 0x40, &info, &handle) == ETIMEDOUT);
	assert(handle != NULL && active_gic_lease != 0 && holds == 1 && maps == 1);
	assert(registered_provider->release(registered_context, &handle) == 0 && handle == NULL);
	assert(active_gic_lease == 0 && holds == 0 && unmaps == 0 && doorbell_mapping == NULL);
	attach_error_vector = UINT32_MAX;
	unmap_error = EIO;
	handle = NULL;
	assert(registered_provider->allocate(registered_context, 7, 0x40, &info, &handle) == 0);
	assert(handle != NULL && active_gic_lease != 0 && holds == 1 && doorbell_mapping != NULL);
	assert(registered_provider->release(registered_context, &handle) == EIO && handle != NULL);
	assert(active_gic_lease != 0 && holds == 1 && doorbell_mapping != NULL && config_space[0x42] == 0x0a);
	unmap_error = 0;
	assert(registered_provider->release(registered_context, &handle) == 0 && handle == NULL);
	assert(active_gic_lease == 0 && holds == 0 && doorbell_mapping == NULL);
	assert(hakios_msi_kona_deactivate(owner) == 0);
	assert(hakios_msi_kona_destroy(&owner) == 0);
}

int
main(void)
{
	test_lifecycle();
	test_partial_cleanup();
	puts("kona-msi-test: PASS (32 GIC routes, PCI capability, endpoint doorbell mapping, rollback and teardown)");
	return 0;
}

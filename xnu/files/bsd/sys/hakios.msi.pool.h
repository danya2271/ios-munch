#ifndef _SYS_HAKIOS_MSI_POOL_H_
#define _SYS_HAKIOS_MSI_POOL_H_

#include <sys/hakios.pcie.h>
#include <sys/hakios.msi.h>

#define HAKIOS_MSI_POOL_VERSION 1u
#define HAKIOS_MSI_POOL_ROUTES_MAX 256u

struct hakios_msi_route {
	void *token;
	uint32_t intid;
	uint32_t data;
};

struct hakios_msi_pool_config {
	const struct hakios_msi_route *routes;
	uint32_t count;
	uint32_t dt_spi_base;
	uint64_t doorbell_physical;
};

struct hakios_msi_pool_operations {
	uint32_t version;
	int (*claim)(void *owner, hakios_pcie_lease_t endpoint, const struct hakios_msi_route *routes, uint32_t count);
	int (*map)(void *owner, hakios_pcie_lease_t endpoint, uint64_t physical, void **mapping, uint64_t *iova);
	int (*quiesce)(void *owner, const struct hakios_msi_route *routes, uint32_t count);
	int (*unmap)(void *owner, void **mapping);
	int (*release)(void *owner, const struct hakios_msi_route *routes, uint32_t count);
};

struct hakios_msi_pool;
struct hakios_msi_allocation;

int hakios_msi_pool_create(const struct hakios_msi_pool_config *config,
    const struct hakios_msi_pool_operations *operations, void *owner, struct hakios_msi_pool **result);
int hakios_msi_pool_allocate(struct hakios_msi_pool *pool, hakios_pcie_lease_t endpoint,
    struct hakios_msi_allocation **result);
int hakios_msi_allocation_config(struct hakios_msi_allocation *allocation, hakios_pcie_lease_t endpoint,
    uint16_t capability, struct hakios_msi_config *config);
int hakios_msi_allocation_route(struct hakios_msi_allocation *allocation, uint32_t vector, struct hakios_msi_route *route);
int hakios_msi_allocation_destroy(struct hakios_msi_allocation **allocation);
int hakios_msi_pool_destroy(struct hakios_msi_pool **pool);

#endif

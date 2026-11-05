#ifndef _SYS_HAKIOS_PCIE_H_
#define _SYS_HAKIOS_PCIE_H_

#include <stdint.h>
#include <stdbool.h>

#define HAKIOS_PCIE_PROVIDER_VERSION 1u
#define HAKIOS_PCIE_BAR_OPERATIONS_MAX 8u

typedef uint64_t hakios_pcie_lease_t;

struct hakios_pcie_endpoint_request {
	uint16_t domain;
	uint16_t requester_id;
	uint32_t stream_id;
	uint16_t vendor;
	uint16_t device;
	uint32_t class_code;
	uint8_t revision;
	uint64_t bar0_address;
	uint64_t bar0_size;
};

struct hakios_pcie_endpoint_resources {
	uint16_t domain;
	uint16_t requester_id;
	uint32_t stream_id;
	uint64_t bar0_address;
	uint64_t bar0_size;
};

struct hakios_pcie_rc0_provider {
	uint32_t version;
	int (*acquire)(void *context, const struct hakios_pcie_endpoint_request *request,
	    struct hakios_pcie_endpoint_resources *resources, void **owner);
	int (*release)(void *context, void *owner);
	int (*config_read)(void *context, void *owner, uint16_t offset, uint8_t width, uint32_t *value);
	int (*config_write)(void *context, void *owner, uint16_t offset, uint8_t width, uint32_t value);
	int (*access_begin)(void *context, void *owner, bool wake_required);
	int (*access_end)(void *context, void *owner);
	int (*bar0_read32)(void *context, void *owner, uint32_t offset, uint32_t *value);
	int (*bar0_write32)(void *context, void *owner, uint32_t offset, uint32_t value);
};

enum hakios_pcie_bar_operation_type {
	HAKIOS_PCIE_BAR_READ32,
	HAKIOS_PCIE_BAR_WRITE32,
	HAKIOS_PCIE_BAR_VERIFY32,
};

struct hakios_pcie_bar_operation {
	enum hakios_pcie_bar_operation_type type;
	uint32_t offset;
	uint32_t value;
	uint32_t mask;
};

int hakios_pcie_rc0_register(const struct hakios_pcie_rc0_provider *provider, void *context);
int hakios_pcie_rc0_unregister(void *context);
int hakios_pcie_rc0_recover(void *context);
int hakios_pcie_endpoint_acquire(const struct hakios_pcie_endpoint_request *request, hakios_pcie_lease_t *lease);
int hakios_pcie_endpoint_release(hakios_pcie_lease_t lease);
int hakios_pcie_endpoint_hold(hakios_pcie_lease_t lease);
int hakios_pcie_endpoint_drop(hakios_pcie_lease_t lease);
int hakios_pcie_endpoint_status(hakios_pcie_lease_t lease);
int hakios_pcie_endpoint_resources(hakios_pcie_lease_t lease, struct hakios_pcie_endpoint_resources *resources);
int hakios_pcie_config_read(hakios_pcie_lease_t lease, uint16_t offset, uint8_t width, uint32_t *value);
int hakios_pcie_config_write(hakios_pcie_lease_t lease, uint16_t offset, uint8_t width, uint32_t value);
int hakios_pcie_bar0_transfer(hakios_pcie_lease_t lease, struct hakios_pcie_bar_operation *operations,
    uint32_t count, bool wake_required);

#endif

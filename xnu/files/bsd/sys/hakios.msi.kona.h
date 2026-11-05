#ifndef _SYS_HAKIOS_MSI_KONA_H_
#define _SYS_HAKIOS_MSI_KONA_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/hakios.pcie.h>

#define HAKIOS_MSI_KONA_VERSION 1u

struct hakios_msi_kona_operations {
	uint32_t version;
	int (*map_doorbell)(void *context, hakios_pcie_lease_t endpoint, uint64_t physical,
	    size_t size, void **mapping, uint64_t *iova);
	int (*unmap_doorbell)(void *context, hakios_pcie_lease_t endpoint, void **mapping);
};

struct hakios_msi_kona;

int hakios_msi_kona_create(const struct hakios_msi_kona_operations *operations, void *context,
    struct hakios_msi_kona **result);
int hakios_msi_kona_activate(struct hakios_msi_kona *owner);
int hakios_msi_kona_deactivate(struct hakios_msi_kona *owner);
int hakios_msi_kona_destroy(struct hakios_msi_kona **owner);

#endif

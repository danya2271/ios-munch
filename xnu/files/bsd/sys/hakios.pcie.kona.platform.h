#ifndef _SYS_HAKIOS_PCIE_KONA_PLATFORM_H_
#define _SYS_HAKIOS_PCIE_KONA_PLATFORM_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/hakios.pcie.kona.h>

#define HAKIOS_PCIE_KONA_PLATFORM_VERSION 2u

struct hakios_pcie_kona_platform_operations {
	uint32_t version;
	void *cold_context;
	void *config_context;
	void *bar_context;
	struct hakios_pcie_kona_cold_owner_operations cold;
	struct hakios_pcie_kona_operations config;
	int (*bar_begin)(void *context, void *owner, bool wake_required);
	int (*bar_end)(void *context, void *owner);
	int (*bar_read32)(void *context, void *owner, uint32_t offset, uint32_t *value);
	int (*bar_write32)(void *context, void *owner, uint32_t offset, uint32_t value);
};

struct hakios_pcie_kona_platform;

int hakios_pcie_kona_platform_create(
    const struct hakios_pcie_kona_platform_operations *operations, void *context,
    struct hakios_pcie_kona_platform **result);
int hakios_pcie_kona_platform_activate(struct hakios_pcie_kona_platform *platform);
int hakios_pcie_kona_platform_deactivate(struct hakios_pcie_kona_platform *platform);
int hakios_pcie_kona_platform_destroy(struct hakios_pcie_kona_platform **platform);
bool hakios_pcie_kona_platform_active(struct hakios_pcie_kona_platform *platform);

#endif

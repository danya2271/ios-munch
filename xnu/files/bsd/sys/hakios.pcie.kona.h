#ifndef _SYS_HAKIOS_PCIE_KONA_H_
#define _SYS_HAKIOS_PCIE_KONA_H_

#include <stdbool.h>
#include <stdint.h>

#define HAKIOS_PCIE_KONA_OPERATIONS_VERSION 1u
#define HAKIOS_PCIE_KONA_COLD_OWNER_VERSION 1u
#define HAKIOS_PCIE_KONA_COLD_STAGE_COUNT 10u

enum hakios_pcie_kona_space {
	HAKIOS_PCIE_KONA_DBI,
	HAKIOS_PCIE_KONA_IATU,
	HAKIOS_PCIE_KONA_CONFIG,
	HAKIOS_PCIE_KONA_ELBI,
	HAKIOS_PCIE_KONA_PARF,
	HAKIOS_PCIE_KONA_PHY,
};

enum hakios_pcie_kona_cold_stage {
	HAKIOS_PCIE_KONA_COLD_PERST_ASSERT,
	HAKIOS_PCIE_KONA_COLD_POWER,
	HAKIOS_PCIE_KONA_COLD_CLOCKS_RESETS,
	HAKIOS_PCIE_KONA_COLD_PARF,
	HAKIOS_PCIE_KONA_COLD_PHY_PIPE,
	HAKIOS_PCIE_KONA_COLD_PERST_RELEASE,
	HAKIOS_PCIE_KONA_COLD_LINK_TRAIN,
	HAKIOS_PCIE_KONA_COLD_SID,
	HAKIOS_PCIE_KONA_COLD_CONTROLLER,
	HAKIOS_PCIE_KONA_COLD_ENDPOINT_CHECK,
};

struct hakios_pcie_kona_resource {
	uint64_t address;
	uint64_t size;
};

struct hakios_pcie_kona_resources {
	struct hakios_pcie_kona_resource dbi;
	struct hakios_pcie_kona_resource iatu;
	struct hakios_pcie_kona_resource config;
};

struct hakios_pcie_kona_operations {
	uint32_t version;
	int (*begin)(void *context, void **guard);
	int (*end)(void *context, void *guard);
	int (*read)(void *context, void *guard, enum hakios_pcie_kona_space space,
	    uint32_t offset, uint8_t width, uint32_t *value);
	int (*write)(void *context, void *guard, enum hakios_pcie_kona_space space,
	    uint32_t offset, uint8_t width, uint32_t value);
	int (*config_drain)(void *context, void *guard);
	int (*delay_us)(void *context, void *guard, uint32_t microseconds);
};

struct hakios_pcie_kona_cold_owner_operations {
	uint32_t version;
	int (*authorize)(void *context, void **ownership);
	int (*revoke)(void *context, void **ownership);
	int (*stage)(void *context, void *ownership, enum hakios_pcie_kona_cold_stage stage,
	    bool acquire, void **cookie);
};

struct hakios_pcie_kona;
struct hakios_pcie_kona_cold_owner;

int hakios_pcie_kona_create(const struct hakios_pcie_kona_operations *operations, void *context,
    const struct hakios_pcie_kona_resources *resources, struct hakios_pcie_kona **result);
int hakios_pcie_kona_destroy(struct hakios_pcie_kona **controller);
int hakios_pcie_kona_recover(struct hakios_pcie_kona *controller);
int hakios_pcie_kona_config_read(struct hakios_pcie_kona *controller, uint16_t requester_id,
    uint16_t offset, uint8_t width, uint32_t *value);
int hakios_pcie_kona_config_write(struct hakios_pcie_kona *controller, uint16_t requester_id,
    uint16_t offset, uint8_t width, uint32_t value);
int hakios_pcie_kona_cold_owner_create(const struct hakios_pcie_kona_cold_owner_operations *operations,
    void *context, struct hakios_pcie_kona_cold_owner **result);
int hakios_pcie_kona_cold_owner_acquire(struct hakios_pcie_kona_cold_owner *owner);
int hakios_pcie_kona_cold_owner_release(struct hakios_pcie_kona_cold_owner *owner);
int hakios_pcie_kona_cold_owner_recover(struct hakios_pcie_kona_cold_owner *owner);
int hakios_pcie_kona_cold_owner_destroy(struct hakios_pcie_kona_cold_owner **owner);

#endif

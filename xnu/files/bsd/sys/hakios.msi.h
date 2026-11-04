#ifndef _SYS_HAKIOS_MSI_H_
#define _SYS_HAKIOS_MSI_H_

#include <stdint.h>

#define HAKIOS_MSI_VERSION 1u
#define HAKIOS_MSI_VECTORS 32u

enum hakios_msi_user {
	HAKIOS_MSI_MHI,
	HAKIOS_MSI_CE,
	HAKIOS_MSI_WAKE,
	HAKIOS_MSI_DP,
};

struct hakios_msi_config {
	uint64_t doorbell_iova;
	uint32_t intids[HAKIOS_MSI_VECTORS];
	uint16_t data;
	uint16_t capability;
};

struct hakios_msi_operations {
	uint32_t version;
	int (*read)(void *owner, uint16_t offset, uint8_t width, uint32_t *value);
	int (*write)(void *owner, uint16_t offset, uint8_t width, uint32_t value);
	int (*arm)(void *owner);
	int (*quiesce)(void *owner);
};

struct hakios_msi_message {
	uint64_t address;
	uint32_t data;
	uint32_t intid;
};

struct hakios_msi;

int hakios_msi_create(const struct hakios_msi_config *config,
    const struct hakios_msi_operations *operations, void *owner, struct hakios_msi **result);
int hakios_msi_start(struct hakios_msi *msi);
int hakios_msi_message(struct hakios_msi *msi, enum hakios_msi_user user,
    uint32_t index, struct hakios_msi_message *message);
int hakios_msi_destroy(struct hakios_msi **msi);

#endif

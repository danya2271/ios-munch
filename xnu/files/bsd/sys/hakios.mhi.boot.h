#ifndef _SYS_HAKIOS_MHI_BOOT_H_
#define _SYS_HAKIOS_MHI_BOOT_H_

#include <stdint.h>

#define HAKIOS_MHI_BOOT_VERSION 1u
#define HAKIOS_MHI_FW_SEGMENT 524288u
#define HAKIOS_MHI_FW_SEGMENTS 32u

struct hakios_mhi_boot_buffer {
	uint8_t *bytes;
	uint64_t iova;
	uint32_t capacity;
};

struct hakios_mhi_boot_config {
	const uint8_t *image;
	uint32_t image_size;
	const struct hakios_mhi_boot_buffer *segments;
	uint32_t segment_count;
	struct hakios_mhi_boot_buffer vectors;
	uint64_t channel_context;
	uint64_t event_context;
	uint64_t command_context;
	uint64_t aperture_start;
	uint64_t aperture_end;
	uint32_t sequence;
	uint32_t timeout_ms;
};

struct hakios_mhi_boot_operations {
	uint32_t version;
	int (*activate)(void *owner);
	int (*read)(void *owner, uint32_t offset, uint32_t *value);
	int (*write)(void *owner, uint32_t offset, uint32_t value);
	int (*sync)(void *owner, uint64_t iova, uint32_t length);
	int (*ready)(void *owner);
	int (*quiesce)(void *owner);
};

enum hakios_mhi_boot_phase {
	HAKIOS_MHI_BOOT_NEW,
	HAKIOS_MHI_BOOT_BHI,
	HAKIOS_MHI_BOOT_READY,
	HAKIOS_MHI_BOOT_SBL,
	HAKIOS_MHI_BOOT_BHIE,
	HAKIOS_MHI_BOOT_AMSS,
	HAKIOS_MHI_BOOT_ONLINE,
	HAKIOS_MHI_BOOT_RUNTIME,
	HAKIOS_MHI_BOOT_FAULT,
};

struct hakios_mhi_boot;

int hakios_mhi_boot_create(const struct hakios_mhi_boot_config *config,
    const struct hakios_mhi_boot_operations *operations, void *owner, struct hakios_mhi_boot **result);
int hakios_mhi_boot_start(struct hakios_mhi_boot *boot, uint64_t now_ms);
int hakios_mhi_boot_step(struct hakios_mhi_boot *boot, uint64_t now_ms);
int hakios_mhi_boot_status(struct hakios_mhi_boot *boot, enum hakios_mhi_boot_phase *phase);
int hakios_mhi_boot_take_runtime(struct hakios_mhi_boot *boot);
int hakios_mhi_boot_retire_runtime(struct hakios_mhi_boot *boot);
int hakios_mhi_boot_destroy(struct hakios_mhi_boot **boot);

#endif

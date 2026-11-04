#ifndef _SYS_HAKIOS_MHI_H_
#define _SYS_HAKIOS_MHI_H_

#include <stdint.h>
#include <stdbool.h>

#define HAKIOS_MHI_VERSION 1u
#define HAKIOS_MHI_ELEMENTS 64u
#define HAKIOS_MHI_TRE_SIZE 16u
#define HAKIOS_MHI_RING_SIZE 1024u
#define HAKIOS_MHI_BUFFER_MAX 8192u
#define HAKIOS_MHI_CONTEXT_SIZE 44u
#define HAKIOS_MHI_COMMAND_ELEMENTS 128u
#define HAKIOS_MHI_COMMAND_SIZE 2048u
#define HAKIOS_MHI_CONTROL_ELEMENTS 32u
#define HAKIOS_MHI_DATA_ELEMENTS 256u

struct hakios_mhi_ring_config {
	uint8_t *elements;
	uint64_t iova;
	uint8_t channel;
};

struct hakios_mhi_ring_operations {
	uint32_t version;
	int (*sync_buffer)(void *owner, uint64_t cookie, bool for_device, uint32_t length);
	int (*sync_ring)(void *owner, uint32_t offset, uint32_t length);
	int (*publish)(void *owner, uint8_t channel, uint64_t next_wp);
	int (*quiesce)(void *owner);
};

struct hakios_mhi_completion {
	uint64_t cookie;
	uint64_t iova;
	uint32_t length;
	int error;
};

struct hakios_mhi_ring;

int hakios_mhi_channel_context(uint8_t channel, uint64_t base, uint8_t context[HAKIOS_MHI_CONTEXT_SIZE]);
int hakios_mhi_command(uint8_t channel, uint8_t command, uint8_t tre[HAKIOS_MHI_TRE_SIZE]);
int hakios_mhi_ring_create(const struct hakios_mhi_ring_config *config,
    const struct hakios_mhi_ring_operations *operations, void *owner, struct hakios_mhi_ring **result);
int hakios_mhi_ring_status(struct hakios_mhi_ring *ring);
int hakios_mhi_ring_queue(struct hakios_mhi_ring *ring, uint64_t cookie, uint64_t iova, uint32_t length);
int hakios_mhi_ring_complete(struct hakios_mhi_ring *ring, const uint8_t event[HAKIOS_MHI_TRE_SIZE],
    struct hakios_mhi_completion *completions, uint32_t capacity, uint32_t *count);
int hakios_mhi_ring_destroy(struct hakios_mhi_ring **ring);

struct hakios_mhi_command_config {
	uint8_t *elements;
	uint64_t iova;
};

struct hakios_mhi_command_operations {
	uint32_t version;
	int (*sync_ring)(void *owner, uint32_t offset, uint32_t length);
	int (*publish)(void *owner, uint64_t next_wp);
	int (*quiesce)(void *owner);
};

struct hakios_mhi_command_completion {
	uint64_t cookie;
	uint8_t channel;
	uint8_t command;
};

struct hakios_mhi_command_ring;

int hakios_mhi_command_context(uint64_t base, uint8_t context[HAKIOS_MHI_CONTEXT_SIZE]);
int hakios_mhi_command_create(const struct hakios_mhi_command_config *config,
    const struct hakios_mhi_command_operations *operations, void *owner, struct hakios_mhi_command_ring **result);
int hakios_mhi_command_status(struct hakios_mhi_command_ring *ring);
int hakios_mhi_command_submit(struct hakios_mhi_command_ring *ring, uint64_t cookie, uint8_t channel, uint8_t command);
int hakios_mhi_command_complete(struct hakios_mhi_command_ring *ring, const uint8_t event[HAKIOS_MHI_TRE_SIZE],
    struct hakios_mhi_command_completion *completion);
int hakios_mhi_command_destroy(struct hakios_mhi_command_ring **ring);

struct hakios_mhi_event_config {
	uint8_t *elements;
	uint64_t iova;
	uint8_t index;
};

struct hakios_mhi_event_operations {
	uint32_t version;
	int (*sync_ring)(void *owner, uint32_t offset, uint32_t length, bool for_device);
	int (*read_pointer)(void *owner, uint64_t *device_rp);
	int (*publish)(void *owner, uint64_t credit_wp);
	int (*dispatch)(void *owner, const uint8_t event[HAKIOS_MHI_TRE_SIZE]);
	int (*quiesce)(void *owner);
};

struct hakios_mhi_event_ring;

int hakios_mhi_event_context(uint8_t index, uint64_t base, uint8_t context[HAKIOS_MHI_CONTEXT_SIZE]);
int hakios_mhi_event_create(const struct hakios_mhi_event_config *config,
    const struct hakios_mhi_event_operations *operations, void *owner, struct hakios_mhi_event_ring **result);
int hakios_mhi_event_start(struct hakios_mhi_event_ring *ring);
int hakios_mhi_event_status(struct hakios_mhi_event_ring *ring);
int hakios_mhi_event_poll(struct hakios_mhi_event_ring *ring, uint32_t budget, uint32_t *dispatched);
int hakios_mhi_event_destroy(struct hakios_mhi_event_ring **ring);

#endif

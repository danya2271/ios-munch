#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.mhi.h>
#include <sys/hakios.wire.h>

struct mhi_buffer {
	uint64_t cookie;
	uint64_t iova;
	uint32_t length;
};

struct hakios_mhi_ring {
	lck_mtx_t lock;
	struct hakios_mhi_ring_config config;
	struct hakios_mhi_ring_operations operations;
	struct mhi_buffer buffers[HAKIOS_MHI_ELEMENTS];
	void *owner;
	uint32_t read;
	uint32_t write;
	uint32_t used;
	bool faulted;
};

static LCK_GRP_DECLARE(mhi_lock_group, "hakios-mhi-ring");

static int
mhi_error(int error)
{
	return error < 0 ? EIO : error;
}

static bool
mhi_base_valid(uint64_t base)
{
	return base != 0 && (base & (HAKIOS_MHI_RING_SIZE - 1)) == 0 && base <= UINT64_MAX - HAKIOS_MHI_RING_SIZE;
}

int
hakios_mhi_channel_context(uint8_t channel, uint64_t base, uint8_t context[HAKIOS_MHI_CONTEXT_SIZE])
{
	if (context == NULL) {
		return EINVAL;
	}
	bzero(context, HAKIOS_MHI_CONTEXT_SIZE);
	if ((channel != 20 && channel != 21) || !mhi_base_valid(base)) {
		return EINVAL;
	}
	hakios_wire_put32(context, 2u << 8);
	hakios_wire_put32(context + 4, channel == 20 ? 1 : 2);
	hakios_wire_put32(context + 8, 1);
	hakios_wire_put64(context + 12, base);
	hakios_wire_put64(context + 20, HAKIOS_MHI_RING_SIZE);
	hakios_wire_put64(context + 28, base);
	hakios_wire_put64(context + 36, base);
	return 0;
}

int
hakios_mhi_command(uint8_t channel, uint8_t command, uint8_t tre[HAKIOS_MHI_TRE_SIZE])
{
	if (tre == NULL) {
		return EINVAL;
	}
	bzero(tre, HAKIOS_MHI_TRE_SIZE);
	if ((channel != 20 && channel != 21) || (command != 0x10 && command != 0x11 && command != 0x12)) {
		return ENOTSUP;
	}
	hakios_wire_put32(tre + 12, (uint32_t)channel << 24 | (uint32_t)command << 16);
	return 0;
}

int
hakios_mhi_ring_create(const struct hakios_mhi_ring_config *config,
    const struct hakios_mhi_ring_operations *operations, void *owner, struct hakios_mhi_ring **result)
{
	struct hakios_mhi_ring *ring;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || operations == NULL || owner == NULL || config->elements == NULL ||
	    ((uintptr_t)config->elements & 15) != 0 || !mhi_base_valid(config->iova) ||
	    (config->channel != 20 && config->channel != 21) || operations->version != HAKIOS_MHI_VERSION ||
	    operations->sync_buffer == NULL || operations->sync_ring == NULL ||
	    operations->publish == NULL || operations->quiesce == NULL) {
		return EINVAL;
	}
	ring = kalloc_type(struct hakios_mhi_ring, Z_WAITOK | Z_ZERO);
	if (ring == NULL) {
		return ENOMEM;
	}
	ring->config = *config;
	ring->operations = *operations;
	ring->owner = owner;
	lck_mtx_init(&ring->lock, &mhi_lock_group, LCK_ATTR_NULL);
	bzero(config->elements, HAKIOS_MHI_RING_SIZE);
	*result = ring;
	return 0;
}

int
hakios_mhi_ring_status(struct hakios_mhi_ring *ring)
{
	int error;
	if (ring == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&ring->lock);
	error = ring->faulted ? EIO : 0;
	lck_mtx_unlock(&ring->lock);
	return error;
}

int
hakios_mhi_ring_queue(struct hakios_mhi_ring *ring, uint64_t cookie, uint64_t iova, uint32_t length)
{
	int error = 0;

	if (ring == NULL || cookie == 0 || iova == 0 || length == 0 || length > HAKIOS_MHI_BUFFER_MAX ||
	    iova > UINT64_MAX - length) {
		return EINVAL;
	}
	lck_mtx_lock(&ring->lock);
	if (ring->faulted || ring->used == HAKIOS_MHI_ELEMENTS - 1) {
		error = ring->faulted ? EIO : ENOSPC;
		goto out;
	}
	if (iova < ring->config.iova + HAKIOS_MHI_RING_SIZE && iova + length > ring->config.iova) {
		error = EINVAL;
		goto out;
	}
	for (unsigned index = 0; index < ring->used; index++) {
		struct mhi_buffer *buffer = &ring->buffers[(ring->read + index) % HAKIOS_MHI_ELEMENTS];
		if (buffer->cookie == cookie || (iova < buffer->iova + buffer->length && iova + length > buffer->iova)) {
			error = EEXIST;
			goto out;
		}
	}
	uint32_t slot = ring->write;
	ring->buffers[slot] = (struct mhi_buffer) {.cookie = cookie, .iova = iova, .length = length};
	ring->write = (slot + 1) % HAKIOS_MHI_ELEMENTS;
	ring->used++;
	error = mhi_error(ring->operations.sync_buffer(ring->owner, cookie, true, length));
	if (error == 0) {
		uint8_t *tre = ring->config.elements + slot * HAKIOS_MHI_TRE_SIZE;
		hakios_wire_put64(tre, iova);
		hakios_wire_put32(tre + 8, length);
		hakios_wire_put32(tre + 12, (2u << 16) | (1u << 9));
		error = mhi_error(ring->operations.sync_ring(ring->owner, slot * HAKIOS_MHI_TRE_SIZE, HAKIOS_MHI_TRE_SIZE));
	}
	if (error == 0) {
		error = mhi_error(ring->operations.publish(ring->owner, ring->config.channel,
		    ring->config.iova + ring->write * HAKIOS_MHI_TRE_SIZE));
	}
	if (error != 0) {
		ring->faulted = true;
	}
out:
	lck_mtx_unlock(&ring->lock);
	return error;
}

int
hakios_mhi_ring_complete(struct hakios_mhi_ring *ring, const uint8_t event[HAKIOS_MHI_TRE_SIZE],
    struct hakios_mhi_completion *completions, uint32_t capacity, uint32_t *count)
{
	int error = 0;

	if (count == NULL) {
		return EINVAL;
	}
	*count = 0;
	if (completions != NULL && capacity < HAKIOS_MHI_ELEMENTS) {
		bzero(completions, capacity * sizeof(*completions));
	}
	if (ring == NULL || event == NULL || completions == NULL || capacity == 0 || capacity >= HAKIOS_MHI_ELEMENTS) {
		return EINVAL;
	}
	lck_mtx_lock(&ring->lock);
	if (ring->faulted) {
		error = EIO;
		goto out;
	}
	uint64_t pointer = hakios_wire_get64(event);
	uint32_t word0 = hakios_wire_get32(event + 8), word1 = hakios_wire_get32(event + 12);
	uint32_t code = word0 >> 24, length = word0 & 0xffff;
	if ((word1 >> 24) != ring->config.channel || ((word1 >> 16) & 255) != 0x22 ||
	    (code != 2 && code != 3) || pointer < ring->config.iova ||
	    pointer - ring->config.iova >= HAKIOS_MHI_RING_SIZE || ((pointer - ring->config.iova) & 15) != 0) {
		error = EPROTO;
		goto fault;
	}
	uint32_t slot = (uint32_t)((pointer - ring->config.iova) / HAKIOS_MHI_TRE_SIZE);
	uint32_t distance = (slot + HAKIOS_MHI_ELEMENTS - ring->read) % HAKIOS_MHI_ELEMENTS;
	if (distance >= ring->used || length > ring->buffers[slot].length) {
		error = EPROTO;
		goto fault;
	}
	if (capacity < distance + 1) {
		error = ENOSPC;
		goto out;
	}
	for (unsigned index = 0; index <= distance; index++) {
		struct mhi_buffer *buffer = &ring->buffers[(ring->read + index) % HAKIOS_MHI_ELEMENTS];
		error = mhi_error(ring->operations.sync_buffer(ring->owner, buffer->cookie, false, buffer->length));
		if (error != 0) {
			goto fault;
		}
	}
	for (unsigned index = 0; index <= distance; index++) {
		struct mhi_buffer *buffer = &ring->buffers[ring->read];
		completions[index] = (struct hakios_mhi_completion) {
			.cookie = buffer->cookie, .iova = buffer->iova,
			.length = index == distance ? length : buffer->length,
			.error = code == 3 ? EOVERFLOW : 0,
		};
		bzero(buffer, sizeof(*buffer));
		ring->read = (ring->read + 1) % HAKIOS_MHI_ELEMENTS;
		ring->used--;
	}
	*count = distance + 1;
	goto out;
fault:
	ring->faulted = true;
out:
	lck_mtx_unlock(&ring->lock);
	return error;
}

int
hakios_mhi_ring_destroy(struct hakios_mhi_ring **ring)
{
	if (ring == NULL || *ring == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_ring *state = *ring;
	lck_mtx_lock(&state->lock);
	int error = mhi_error(state->operations.quiesce(state->owner));
	if (error != 0) {
		state->faulted = true;
		lck_mtx_unlock(&state->lock);
		return error;
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &mhi_lock_group);
	kfree_type(struct hakios_mhi_ring, state);
	*ring = NULL;
	return 0;
}

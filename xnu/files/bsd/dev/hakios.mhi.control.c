#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.mhi.h>
#include <sys/hakios.wire.h>

struct hakios_mhi_command_ring {
	lck_mtx_t lock;
	struct hakios_mhi_command_config config;
	struct hakios_mhi_command_operations operations;
	struct hakios_mhi_command_completion pending[HAKIOS_MHI_COMMAND_ELEMENTS];
	void *owner;
	uint32_t read;
	uint32_t write;
	uint32_t used;
	bool faulted;
};

struct hakios_mhi_event_ring {
	lck_mtx_t lock;
	struct hakios_mhi_event_config config;
	struct hakios_mhi_event_operations operations;
	void *owner;
	uint32_t elements;
	uint32_t read;
	uint32_t producer;
	uint32_t pending;
	bool started;
	bool faulted;
};

static LCK_GRP_DECLARE(mhi_control_lock_group, "hakios-mhi-control");

static int
mhi_control_error(int error)
{
	return error < 0 ? EIO : error;
}

static bool
mhi_control_base_valid(uint64_t base, uint32_t size)
{
	return base != 0 && (base & (size - 1)) == 0 && base <= UINT64_MAX - size;
}

static uint32_t
mhi_event_elements(uint8_t index)
{
	return index == 0 ? HAKIOS_MHI_CONTROL_ELEMENTS : HAKIOS_MHI_DATA_ELEMENTS;
}

static void
mhi_context_ring(uint8_t *context, uint64_t base, uint32_t size)
{
	hakios_wire_put64(context + 12, base);
	hakios_wire_put64(context + 20, size);
	hakios_wire_put64(context + 28, base);
	hakios_wire_put64(context + 36, base);
}

int
hakios_mhi_command_context(uint64_t base, uint8_t context[HAKIOS_MHI_CONTEXT_SIZE])
{
	if (context == NULL) {
		return EINVAL;
	}
	bzero(context, HAKIOS_MHI_CONTEXT_SIZE);
	if (!mhi_control_base_valid(base, HAKIOS_MHI_COMMAND_SIZE)) {
		return EINVAL;
	}
	mhi_context_ring(context, base, HAKIOS_MHI_COMMAND_SIZE);
	return 0;
}

int
hakios_mhi_event_context(uint8_t index, uint64_t base, uint8_t context[HAKIOS_MHI_CONTEXT_SIZE])
{
	uint32_t size = mhi_event_elements(index) * HAKIOS_MHI_TRE_SIZE;
	if (context == NULL) {
		return EINVAL;
	}
	bzero(context, HAKIOS_MHI_CONTEXT_SIZE);
	if (index > 1 || !mhi_control_base_valid(base, size)) {
		return EINVAL;
	}
	hakios_wire_put32(context, index == 0 ? 0 : 1u << 16);
	hakios_wire_put32(context + 4, 1);
	hakios_wire_put32(context + 8, index + 1);
	mhi_context_ring(context, base, size);
	return 0;
}

int
hakios_mhi_command_create(const struct hakios_mhi_command_config *config,
    const struct hakios_mhi_command_operations *operations, void *owner, struct hakios_mhi_command_ring **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || operations == NULL || owner == NULL || config->elements == NULL ||
	    ((uintptr_t)config->elements & 15) != 0 || !mhi_control_base_valid(config->iova, HAKIOS_MHI_COMMAND_SIZE) ||
	    operations->version != HAKIOS_MHI_VERSION || operations->sync_ring == NULL ||
	    operations->publish == NULL || operations->quiesce == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_command_ring *ring = kalloc_type(struct hakios_mhi_command_ring, Z_WAITOK | Z_ZERO);
	if (ring == NULL) {
		return ENOMEM;
	}
	ring->config = *config;
	ring->operations = *operations;
	ring->owner = owner;
	lck_mtx_init(&ring->lock, &mhi_control_lock_group, LCK_ATTR_NULL);
	bzero(config->elements, HAKIOS_MHI_COMMAND_SIZE);
	*result = ring;
	return 0;
}

int
hakios_mhi_command_status(struct hakios_mhi_command_ring *ring)
{
	if (ring == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&ring->lock);
	int error = ring->faulted ? EIO : 0;
	lck_mtx_unlock(&ring->lock);
	return error;
}

int
hakios_mhi_command_submit(struct hakios_mhi_command_ring *ring, uint64_t cookie, uint8_t channel, uint8_t command)
{
	uint8_t tre[HAKIOS_MHI_TRE_SIZE];
	if (ring == NULL || cookie == 0) {
		return EINVAL;
	}
	int error = hakios_mhi_command(channel, command, tre);
	if (error != 0) {
		return error;
	}
	lck_mtx_lock(&ring->lock);
	if (ring->faulted || ring->used == HAKIOS_MHI_COMMAND_ELEMENTS - 1) {
		error = ring->faulted ? EIO : ENOSPC;
		goto out;
	}
	for (unsigned index = 0; index < ring->used; index++) {
		struct hakios_mhi_command_completion *pending = &ring->pending[(ring->read + index) % HAKIOS_MHI_COMMAND_ELEMENTS];
		if (pending->cookie == cookie || pending->channel == channel) {
			error = pending->cookie == cookie ? EEXIST : EBUSY;
			goto out;
		}
	}
	uint32_t slot = ring->write;
	ring->pending[slot] = (struct hakios_mhi_command_completion) {
		.cookie = cookie, .channel = channel, .command = command,
	};
	ring->write = (slot + 1) % HAKIOS_MHI_COMMAND_ELEMENTS;
	ring->used++;
	memcpy(ring->config.elements + slot * HAKIOS_MHI_TRE_SIZE, tre, sizeof(tre));
	error = mhi_control_error(ring->operations.sync_ring(ring->owner, slot * HAKIOS_MHI_TRE_SIZE, sizeof(tre)));
	if (error == 0) {
		error = mhi_control_error(ring->operations.publish(ring->owner,
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
hakios_mhi_command_complete(struct hakios_mhi_command_ring *ring, const uint8_t event[HAKIOS_MHI_TRE_SIZE],
    struct hakios_mhi_command_completion *completion)
{
	if (completion == NULL) {
		return EINVAL;
	}
	bzero(completion, sizeof(*completion));
	if (ring == NULL || event == NULL) {
		return EINVAL;
	}
	int error = 0;
	lck_mtx_lock(&ring->lock);
	if (ring->faulted) {
		error = EIO;
		goto out;
	}
	uint32_t type = (hakios_wire_get32(event + 12) >> 16) & 255;
	uint32_t code = hakios_wire_get32(event + 8) >> 24;
	if (type != 0x21 || ring->used == 0 ||
	    hakios_wire_get64(event) != ring->config.iova + ring->read * HAKIOS_MHI_TRE_SIZE) {
		error = EPROTO;
	} else if (code != 1) {
		error = EIO;
	}
	if (error != 0) {
		ring->faulted = true;
		goto out;
	}
	*completion = ring->pending[ring->read];
	bzero(&ring->pending[ring->read], sizeof(*completion));
	ring->read = (ring->read + 1) % HAKIOS_MHI_COMMAND_ELEMENTS;
	ring->used--;
out:
	lck_mtx_unlock(&ring->lock);
	return error;
}

int
hakios_mhi_command_destroy(struct hakios_mhi_command_ring **ring)
{
	if (ring == NULL || *ring == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_command_ring *state = *ring;
	lck_mtx_lock(&state->lock);
	int error = mhi_control_error(state->operations.quiesce(state->owner));
	if (error != 0) {
		state->faulted = true;
		lck_mtx_unlock(&state->lock);
		return error;
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &mhi_control_lock_group);
	kfree_type(struct hakios_mhi_command_ring, state);
	*ring = NULL;
	return 0;
}

int
hakios_mhi_event_create(const struct hakios_mhi_event_config *config,
    const struct hakios_mhi_event_operations *operations, void *owner, struct hakios_mhi_event_ring **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || operations == NULL || owner == NULL || config->elements == NULL || config->index > 1 ||
	    ((uintptr_t)config->elements & 15) != 0 ||
	    !mhi_control_base_valid(config->iova, mhi_event_elements(config->index) * HAKIOS_MHI_TRE_SIZE) ||
	    operations->version != HAKIOS_MHI_VERSION || operations->sync_ring == NULL ||
	    operations->read_pointer == NULL || operations->publish == NULL ||
	    operations->dispatch == NULL || operations->quiesce == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_event_ring *ring = kalloc_type(struct hakios_mhi_event_ring, Z_WAITOK | Z_ZERO);
	if (ring == NULL) {
		return ENOMEM;
	}
	ring->config = *config;
	ring->operations = *operations;
	ring->owner = owner;
	ring->elements = mhi_event_elements(config->index);
	lck_mtx_init(&ring->lock, &mhi_control_lock_group, LCK_ATTR_NULL);
	bzero(config->elements, ring->elements * HAKIOS_MHI_TRE_SIZE);
	*result = ring;
	return 0;
}

int
hakios_mhi_event_start(struct hakios_mhi_event_ring *ring)
{
	if (ring == NULL) {
		return EINVAL;
	}
	int error = 0;
	uint64_t pointer = 0;
	lck_mtx_lock(&ring->lock);
	if (ring->faulted || ring->started) {
		error = ring->faulted ? EIO : EALREADY;
		goto out;
	}
	error = mhi_control_error(ring->operations.sync_ring(ring->owner, 0, ring->elements * HAKIOS_MHI_TRE_SIZE, true));
	if (error == 0) {
		error = mhi_control_error(ring->operations.read_pointer(ring->owner, &pointer));
	}
	if (error == 0 && pointer != ring->config.iova) {
		error = EPROTO;
	}
	if (error == 0) {
		error = mhi_control_error(ring->operations.publish(ring->owner,
		    ring->config.iova + (ring->elements - 1) * HAKIOS_MHI_TRE_SIZE));
	}
	if (error != 0) {
		ring->faulted = true;
	} else {
		ring->started = true;
	}
out:
	lck_mtx_unlock(&ring->lock);
	return error;
}

int
hakios_mhi_event_status(struct hakios_mhi_event_ring *ring)
{
	if (ring == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&ring->lock);
	int error = ring->faulted ? EIO : ring->started ? 0 : EAGAIN;
	lck_mtx_unlock(&ring->lock);
	return error;
}

static int
mhi_event_route(uint8_t index, const uint8_t *event)
{
	uint32_t word = hakios_wire_get32(event + 12);
	uint32_t type = (word >> 16) & 255, channel = word >> 24;
	if (index == 0) {
		return type == 0x20 || type == 0x21 || type == 0x40 ? 0 : ENOTSUP;
	}
	if (type != 0x22) {
		return ENOTSUP;
	}
	return channel == 20 || channel == 21 ? 0 : EPROTO;
}

int
hakios_mhi_event_poll(struct hakios_mhi_event_ring *ring, uint32_t budget, uint32_t *dispatched)
{
	if (dispatched == NULL) {
		return EINVAL;
	}
	*dispatched = 0;
	if (ring == NULL || budget == 0 || budget >= ring->elements) {
		return EINVAL;
	}
	int error = 0;
	uint64_t pointer = 0;
	lck_mtx_lock(&ring->lock);
	if (ring->faulted || !ring->started) {
		error = ring->faulted ? EIO : EAGAIN;
		goto out;
	}
	error = mhi_control_error(ring->operations.read_pointer(ring->owner, &pointer));
	if (error != 0) {
		goto fault;
	}
	if (pointer < ring->config.iova || pointer - ring->config.iova >= ring->elements * HAKIOS_MHI_TRE_SIZE ||
	    ((pointer - ring->config.iova) & (HAKIOS_MHI_TRE_SIZE - 1)) != 0) {
		error = EPROTO;
		goto fault;
	}
	uint32_t producer = (uint32_t)((pointer - ring->config.iova) / HAKIOS_MHI_TRE_SIZE);
	uint32_t advance = (producer + ring->elements - ring->producer) % ring->elements;
	if (advance > ring->elements - 1 - ring->pending) {
		error = EPROTO;
		goto fault;
	}
	ring->producer = producer;
	ring->pending += advance;
	while (ring->pending != 0 && *dispatched < budget) {
		uint8_t event[HAKIOS_MHI_TRE_SIZE];
		uint32_t offset = ring->read * HAKIOS_MHI_TRE_SIZE;
		error = mhi_control_error(ring->operations.sync_ring(ring->owner, offset, sizeof(event), false));
		if (error != 0) {
			goto fault;
		}
		memcpy(event, ring->config.elements + offset, sizeof(event));
		error = mhi_event_route(ring->config.index, event);
		if (error != 0) {
			goto fault;
		}
		error = mhi_control_error(ring->operations.dispatch(ring->owner, event));
		if (error == EAGAIN || error == ENOSPC) {
			goto out;
		}
		if (error != 0) {
			goto fault;
		}
		(*dispatched)++;
		ring->read = (ring->read + 1) % ring->elements;
		ring->pending--;
		error = mhi_control_error(ring->operations.sync_ring(ring->owner, offset, sizeof(event), true));
		if (error == 0) {
			error = mhi_control_error(ring->operations.publish(ring->owner, ring->config.iova + offset));
		}
		if (error != 0) {
			goto fault;
		}
	}
	goto out;
fault:
	ring->faulted = true;
out:
	lck_mtx_unlock(&ring->lock);
	return error;
}

int
hakios_mhi_event_destroy(struct hakios_mhi_event_ring **ring)
{
	if (ring == NULL || *ring == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_event_ring *state = *ring;
	lck_mtx_lock(&state->lock);
	int error = mhi_control_error(state->operations.quiesce(state->owner));
	if (error != 0) {
		state->faulted = true;
		lck_mtx_unlock(&state->lock);
		return error;
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &mhi_control_lock_group);
	kfree_type(struct hakios_mhi_event_ring, state);
	*ring = NULL;
	return 0;
}

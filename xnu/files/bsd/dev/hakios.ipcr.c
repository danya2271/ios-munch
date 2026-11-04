#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.ipcr.h>
#include <sys/hakios.qrtr.wire.h>
#include <sys/hakios.wire.h>

enum ipcr_buffer_state { IPCR_FREE, IPCR_POSTED, IPCR_READY };

struct ipcr_slot {
	struct hakios_ipcr_buffer buffer;
	enum ipcr_buffer_state state;
	uint32_t length;
};

struct ipcr_channel {
	struct hakios_ipcr *bridge;
	struct hakios_mhi_ring *ring;
	struct ipcr_slot slots[HAKIOS_IPCR_BUFFERS];
	uint32_t count;
	uint8_t number;
};

struct hakios_ipcr {
	lck_mtx_t lock;
	struct hakios_ipcr_operations operations;
	struct hakios_ipcr_status status;
	struct ipcr_channel tx;
	struct ipcr_channel rx;
	struct hakios_qrtr_mhi *router;
	struct hakios_ipcr *binding_next;
	struct qmi_handle *qmi;
	void *owner;
	uint32_t ready[HAKIOS_IPCR_BUFFERS];
	uint32_t read;
	uint32_t write;
	bool working;
	bool router_started;
};

static LCK_GRP_DECLARE(ipcr_group, "hakios-ipcr");
static LCK_MTX_DECLARE(ipcr_bind_lock, &ipcr_group);
static struct hakios_ipcr *ipcr_bindings;

static int
ipcr_fault(struct hakios_ipcr *bridge, int error)
{
	if (error <= 0 || error == EAGAIN || error == ENOSPC) {
		error = EIO;
	}
	if (bridge->status.fault == 0) {
		bridge->status.fault = error;
	}
	return bridge->status.fault;
}

static int
ipcr_available(struct hakios_ipcr *bridge)
{
	return bridge->status.fault != 0 ? bridge->status.fault : bridge->status.stopped ? ENOTCONN :
	    !bridge->status.started || bridge->status.paused ? EAGAIN : 0;
}

static int
ipcr_sync_buffer(void *owner, uint64_t cookie, bool for_device, uint32_t length)
{
	struct ipcr_channel *channel = owner;
	struct hakios_ipcr *bridge = channel->bridge;
	if (cookie == 0 || cookie > channel->count) {
		return EINVAL;
	}
	return bridge->operations.sync_buffer(bridge->owner, channel->number, (uint32_t)cookie - 1, for_device, length);
}

static int
ipcr_sync_ring(void *owner, uint32_t offset, uint32_t length)
{
	struct ipcr_channel *channel = owner;
	return channel->bridge->operations.sync_ring(channel->bridge->owner, channel->number, offset, length);
}

static int
ipcr_publish(void *owner, uint8_t number, uint64_t next_wp)
{
	struct ipcr_channel *channel = owner;
	return channel->bridge->operations.publish(channel->bridge->owner, number, next_wp);
}

static int
ipcr_ring_quiesce(void *owner)
{
	struct ipcr_channel *channel = owner;
	return channel->bridge->status.quiesced ? 0 : EBUSY;
}

static int
ipcr_router_quiesce(void *owner)
{
	struct hakios_ipcr *bridge = owner;
	return bridge->status.quiesced ? 0 : EBUSY;
}

static void
ipcr_router_schedule(void *owner)
{
	struct hakios_ipcr *bridge = owner;
	lck_mtx_lock(&bridge->lock);
	if (!bridge->status.stopped) {
		bridge->status.quiesced = false;
		bridge->operations.schedule(bridge->owner);
	}
	lck_mtx_unlock(&bridge->lock);
}

static int
ipcr_queue(struct ipcr_channel *channel, uint32_t index, uint32_t length)
{
	struct ipcr_slot *slot = &channel->slots[index];
	int error = hakios_mhi_ring_queue(channel->ring, index + 1, slot->buffer.iova, length);
	if (error == 0) {
		slot->state = IPCR_POSTED;
	} else if ((error != EAGAIN && error != ENOSPC) || hakios_mhi_ring_status(channel->ring) != 0) {
		error = ipcr_fault(channel->bridge, error);
	}
	return error;
}

static int
ipcr_transmit(void *owner, const void *frame, size_t size)
{
	struct hakios_ipcr *bridge = owner;
	if (frame == NULL || size == 0 || size > HAKIOS_MHI_BUFFER_MAX) {
		return EMSGSIZE;
	}
	lck_mtx_lock(&bridge->lock);
	int error = ipcr_available(bridge);
	if (error == 0) {
		error = EAGAIN;
		for (uint32_t index = 0; index < bridge->tx.count; index++) {
			struct ipcr_slot *slot = &bridge->tx.slots[index];
			if (slot->state != IPCR_FREE) {
				continue;
			}
			memcpy(slot->buffer.bytes, frame, size);
			error = ipcr_queue(&bridge->tx, index, (uint32_t)size);
			if (error == 0) {
				bridge->status.tx_pending++;
			}
			break;
		}
	}
	if (bridge->status.fault != 0 && !bridge->status.stopped) {
		bridge->operations.schedule(bridge->owner);
	}
	lck_mtx_unlock(&bridge->lock);
	return error;
}

static bool
ipcr_overlap(uint64_t first, uint32_t first_size, uint64_t second, uint32_t second_size)
{
	return first < second + second_size && second < first + first_size;
}

static int
ipcr_validate(const struct hakios_ipcr_config *config)
{
	struct { uint64_t cpu, iova; uint32_t size; } regions[2 + 2 * HAKIOS_IPCR_BUFFERS];
	if (config == NULL || config->epoch == 0 || config->tx_buffers == NULL || config->rx_buffers == NULL ||
	    config->tx_count == 0 || config->tx_count > HAKIOS_IPCR_BUFFERS || config->rx_count == 0 ||
	    config->rx_count > HAKIOS_IPCR_BUFFERS || config->tx_ring.channel != 20 || config->rx_ring.channel != 21 ||
	    (config->tx_ring.iova & (HAKIOS_MHI_RING_SIZE - 1)) != 0 ||
	    (config->rx_ring.iova & (HAKIOS_MHI_RING_SIZE - 1)) != 0 ||
	    ((uintptr_t)config->tx_ring.elements & 15) != 0 || ((uintptr_t)config->rx_ring.elements & 15) != 0 ||
	    config->router.local_node == 0 || config->router.local_node >= HAKIOS_QRTR_CONTROL_PORT ||
	    config->router.local_port == 0 || config->router.local_port >= HAKIOS_QRTR_CONTROL_PORT) {
		return EINVAL;
	}
	regions[0].cpu = (uintptr_t)config->tx_ring.elements;
	regions[0].iova = config->tx_ring.iova;
	regions[0].size = HAKIOS_MHI_RING_SIZE;
	regions[1].cpu = (uintptr_t)config->rx_ring.elements;
	regions[1].iova = config->rx_ring.iova;
	regions[1].size = HAKIOS_MHI_RING_SIZE;
	uint32_t count = 2 + config->tx_count + config->rx_count;
	for (uint32_t index = 2; index < count; index++) {
		const struct hakios_ipcr_buffer *buffer = index - 2 < config->tx_count ?
		    &config->tx_buffers[index - 2] : &config->rx_buffers[index - 2 - config->tx_count];
		regions[index].cpu = (uintptr_t)buffer->bytes;
		regions[index].iova = buffer->iova;
		regions[index].size = HAKIOS_MHI_BUFFER_MAX;
	}
	for (uint32_t index = 0; index < count; index++) {
		if (regions[index].cpu == 0 || regions[index].iova == 0 ||
		    regions[index].cpu > UINTPTR_MAX - regions[index].size ||
		    regions[index].iova > UINT64_MAX - regions[index].size) {
			return EINVAL;
		}
		for (uint32_t prior = 0; prior < index; prior++) {
			if (ipcr_overlap(regions[index].cpu, regions[index].size, regions[prior].cpu, regions[prior].size) ||
			    ipcr_overlap(regions[index].iova, regions[index].size, regions[prior].iova, regions[prior].size)) {
				return EINVAL;
			}
		}
	}
	return 0;
}

int
hakios_ipcr_create(const struct hakios_ipcr_config *config,
    const struct hakios_ipcr_operations *operations, void *owner, struct hakios_ipcr **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (ipcr_validate(config) != 0 || owner == NULL || operations == NULL || operations->version != HAKIOS_IPCR_VERSION ||
	    operations->sync_buffer == NULL || operations->sync_ring == NULL || operations->publish == NULL ||
	    operations->schedule == NULL || operations->quiesce == NULL) {
		return EINVAL;
	}
	struct hakios_ipcr *bridge = kalloc_type(struct hakios_ipcr, Z_WAITOK | Z_ZERO);
	if (bridge == NULL) {
		return ENOMEM;
	}
	bridge->operations = *operations;
	bridge->owner = owner;
	bridge->status.epoch = config->epoch;
	bridge->status.quiesced = true;
	bridge->tx.bridge = bridge;
	bridge->tx.number = 20;
	bridge->tx.count = config->tx_count;
	bridge->rx.bridge = bridge;
	bridge->rx.number = 21;
	bridge->rx.count = config->rx_count;
	for (uint32_t index = 0; index < config->tx_count; index++) {
		bridge->tx.slots[index].buffer = config->tx_buffers[index];
	}
	for (uint32_t index = 0; index < config->rx_count; index++) {
		bridge->rx.slots[index].buffer = config->rx_buffers[index];
	}
	lck_mtx_init(&bridge->lock, &ipcr_group, LCK_ATTR_NULL);
	const struct hakios_mhi_ring_operations ring_ops = {
		.version = HAKIOS_MHI_VERSION, .sync_buffer = ipcr_sync_buffer, .sync_ring = ipcr_sync_ring,
		.publish = ipcr_publish, .quiesce = ipcr_ring_quiesce,
	};
	const struct hakios_qrtr_mhi_operations router_ops = {
		.version = HAKIOS_QRTR_MHI_VERSION, .transmit = ipcr_transmit, .quiesce = ipcr_router_quiesce,
		.schedule = ipcr_router_schedule,
	};
	int error = hakios_mhi_ring_create(&config->tx_ring, &ring_ops, &bridge->tx, &bridge->tx.ring);
	if (error == 0) {
		error = hakios_mhi_ring_create(&config->rx_ring, &ring_ops, &bridge->rx, &bridge->rx.ring);
	}
	if (error == 0) {
		error = hakios_qrtr_mhi_create(&config->router, &router_ops, bridge, &bridge->router);
	}
	if (error != 0) {
		if (bridge->rx.ring != NULL) {
			hakios_mhi_ring_destroy(&bridge->rx.ring);
		}
		if (bridge->tx.ring != NULL) {
			hakios_mhi_ring_destroy(&bridge->tx.ring);
		}
		lck_mtx_destroy(&bridge->lock, &ipcr_group);
		kfree_type(struct hakios_ipcr, bridge);
		return error;
	}
	*result = bridge;
	return 0;
}

int
hakios_ipcr_endpoint(struct hakios_ipcr *bridge, struct hakios_qrtr_endpoint *endpoint)
{
	return bridge == NULL ? EINVAL : hakios_qrtr_mhi_endpoint(bridge->router, endpoint);
}

int
hakios_ipcr_start(struct hakios_ipcr *bridge)
{
	if (bridge == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&bridge->lock);
	int error = bridge->status.stopped ? ENOTCONN : bridge->status.started ? EALREADY : 0;
	if (error == 0) {
		bridge->status.started = true;
		bridge->status.quiesced = false;
		bridge->operations.schedule(bridge->owner);
	}
	lck_mtx_unlock(&bridge->lock);
	return error;
}

int
hakios_ipcr_complete(struct hakios_ipcr *bridge, uint64_t epoch, const uint8_t event[HAKIOS_MHI_TRE_SIZE])
{
	if (bridge == NULL || event == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_completion completions[HAKIOS_IPCR_BUFFERS];
	lck_mtx_lock(&bridge->lock);
	int error = epoch != bridge->status.epoch ? ESTALE : bridge->status.stopped ? ENOTCONN :
	    !bridge->status.started ? EAGAIN : bridge->status.fault;
	if (error != 0) {
		goto out;
	}
	uint8_t number = hakios_wire_get32(event + 12) >> 24;
	struct ipcr_channel *channel = number == 20 ? &bridge->tx : number == 21 ? &bridge->rx : NULL;
	if (channel == NULL) {
		error = ipcr_fault(bridge, EPROTO);
		goto out;
	}
	uint32_t count;
	error = hakios_mhi_ring_complete(channel->ring, event, completions, HAKIOS_IPCR_BUFFERS, &count);
	if (error != 0) {
		error = ipcr_fault(bridge, error);
		goto out;
	}
	for (uint32_t index = 0; index < count; index++) {
		struct hakios_mhi_completion *completion = &completions[index];
		if (completion->cookie == 0 || completion->cookie > channel->count || completion->error != 0 ||
		    (number == 21 && completion->length == 0)) {
			error = ipcr_fault(bridge, EPROTO);
			break;
		}
		uint32_t slot_index = (uint32_t)completion->cookie - 1;
		struct ipcr_slot *slot = &channel->slots[slot_index];
		if (slot->state != IPCR_POSTED || slot->buffer.iova != completion->iova) {
			error = ipcr_fault(bridge, EPROTO);
			break;
		}
		if (number == 20) {
			slot->state = IPCR_FREE;
			bridge->status.tx_pending--;
		} else {
			slot->state = IPCR_READY;
			slot->length = completion->length;
			bridge->ready[bridge->write] = slot_index;
			bridge->write = (bridge->write + 1) % HAKIOS_IPCR_BUFFERS;
			bridge->status.rx_ready++;
		}
	}
out:
	if (epoch == bridge->status.epoch && !bridge->status.stopped) {
		bridge->operations.schedule(bridge->owner);
	}
	lck_mtx_unlock(&bridge->lock);
	return error;
}

int
hakios_ipcr_work(struct hakios_ipcr *bridge, uint32_t budget, uint32_t *received)
{
	if (received == NULL) {
		return EINVAL;
	}
	*received = 0;
	if (bridge == NULL || budget == 0 || budget > HAKIOS_IPCR_BUFFERS) {
		return EINVAL;
	}
	lck_mtx_lock(&bridge->lock);
	int error = ipcr_available(bridge);
	if (bridge->working || bridge->status.stopped || !bridge->status.started || bridge->status.paused) {
		error = bridge->working ? EBUSY : error;
		lck_mtx_unlock(&bridge->lock);
		return error;
	}
	bridge->working = true;
	for (uint32_t index = 0; error == 0 && index < bridge->rx.count; index++) {
		if (bridge->rx.slots[index].state == IPCR_FREE) {
			error = ipcr_queue(&bridge->rx, index, HAKIOS_MHI_BUFFER_MAX);
		}
	}
	lck_mtx_unlock(&bridge->lock);
	if (error == 0 && !bridge->router_started) {
		error = hakios_qrtr_mhi_start(bridge->router);
		if (error == 0) {
			bridge->router_started = true;
		}
	}
	while (error == 0 && *received < budget) {
		error = hakios_qrtr_mhi_poll(bridge->router);
		if (error != 0 && error != EAGAIN && error != ENOSPC) {
			break;
		}
		lck_mtx_lock(&bridge->lock);
		if (bridge->status.fault != 0 || bridge->status.rx_ready == 0) {
			error = bridge->status.fault != 0 ? bridge->status.fault : error;
			lck_mtx_unlock(&bridge->lock);
			break;
		}
		uint32_t index = bridge->ready[bridge->read];
		struct ipcr_slot *slot = &bridge->rx.slots[index];
		lck_mtx_unlock(&bridge->lock);
		error = hakios_qrtr_mhi_receive(bridge->router, slot->buffer.bytes, slot->length);
		if (error != 0) {
			break;
		}
		(*received)++;
		lck_mtx_lock(&bridge->lock);
		bridge->read = (bridge->read + 1) % HAKIOS_IPCR_BUFFERS;
		bridge->status.rx_ready--;
		slot->state = IPCR_FREE;
		error = bridge->status.fault != 0 ? bridge->status.fault : ipcr_queue(&bridge->rx, index, HAKIOS_MHI_BUFFER_MAX);
		lck_mtx_unlock(&bridge->lock);
	}
	lck_mtx_lock(&bridge->lock);
	if (error != 0 && error != EAGAIN && error != ENOSPC) {
		ipcr_fault(bridge, error);
	}
	bool faulted = bridge->status.fault != 0;
	if (faulted) {
		error = bridge->status.fault;
	}
	lck_mtx_unlock(&bridge->lock);
	if (faulted) {
		hakios_qrtr_mhi_disconnect(bridge->router);
	}
	lck_mtx_lock(&bridge->lock);
	bridge->working = false;
	if (!faulted && error == 0 && *received == budget) {
		bridge->operations.schedule(bridge->owner);
	}
	lck_mtx_unlock(&bridge->lock);
	return error;
}

int
hakios_ipcr_pause(struct hakios_ipcr *bridge)
{
	if (bridge == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&bridge->lock);
	int error = ipcr_available(bridge);
	if (error == 0 && (bridge->working || bridge->status.tx_pending != 0 || bridge->status.rx_ready != 0)) {
		error = EBUSY;
	}
	if (error == 0) {
		bridge->status.paused = true;
		bridge->working = true;
	}
	lck_mtx_unlock(&bridge->lock);
	if (error == 0) {
		error = hakios_qrtr_mhi_idle(bridge->router);
		lck_mtx_lock(&bridge->lock);
		if (bridge->status.fault != 0 || bridge->status.rx_ready != 0) {
			error = bridge->status.fault != 0 ? bridge->status.fault : EBUSY;
		}
		bridge->working = false;
		if (error != 0) {
			bridge->status.paused = false;
			bridge->operations.schedule(bridge->owner);
		}
		lck_mtx_unlock(&bridge->lock);
	}
	return error;
}

int
hakios_ipcr_resume(struct hakios_ipcr *bridge)
{
	if (bridge == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&bridge->lock);
	int error = bridge->status.fault != 0 ? bridge->status.fault : bridge->status.stopped ? ENOTCONN :
	    bridge->working ? EBUSY : !bridge->status.paused ? EINVAL : 0;
	if (error == 0) {
		bridge->status.paused = false;
		bridge->operations.schedule(bridge->owner);
	}
	lck_mtx_unlock(&bridge->lock);
	return error;
}

int
hakios_ipcr_stop(struct hakios_ipcr *bridge)
{
	if (bridge == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&bridge->lock);
	if (bridge->working) {
		lck_mtx_unlock(&bridge->lock);
		return EBUSY;
	}
	bridge->working = true;
	bridge->status.stopped = true;
	bool quiesced = bridge->status.quiesced;
	lck_mtx_unlock(&bridge->lock);
	int error = hakios_qrtr_mhi_disconnect(bridge->router);
	if (error == 0 && !quiesced) {
		error = bridge->operations.quiesce(bridge->owner);
	}
	lck_mtx_lock(&bridge->lock);
	if (error == 0) {
		bridge->status.quiesced = true;
	} else {
		error = ipcr_fault(bridge, error);
	}
	bridge->working = false;
	lck_mtx_unlock(&bridge->lock);
	return error;
}

int
hakios_ipcr_status(struct hakios_ipcr *bridge, struct hakios_ipcr_status *status)
{
	if (bridge == NULL || status == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&bridge->lock);
	*status = bridge->status;
	lck_mtx_unlock(&bridge->lock);
	return 0;
}

int
hakios_ipcr_qmi_bind(struct hakios_ipcr *bridge, struct qmi_handle *qmi)
{
	if (bridge == NULL || qmi == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&ipcr_bind_lock);
	lck_mtx_lock(&bridge->lock);
	int error = bridge->status.stopped || bridge->status.fault != 0 ? ENOTCONN :
	    bridge->qmi != NULL || qmi->sock != NULL ? EBUSY : 0;
	for (struct hakios_ipcr *entry = ipcr_bindings; error == 0 && entry != NULL; entry = entry->binding_next) {
		if (entry->qmi == qmi) {
			error = EEXIST;
		}
	}
	if (error == 0) {
		bridge->qmi = qmi;
		bridge->binding_next = ipcr_bindings;
		ipcr_bindings = bridge;
	}
	lck_mtx_unlock(&bridge->lock);
	lck_mtx_unlock(&ipcr_bind_lock);
	return error;
}

int
hakios_ipcr_qmi_unbind(struct hakios_ipcr *bridge, struct qmi_handle *qmi)
{
	if (bridge == NULL || qmi == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&ipcr_bind_lock);
	int error = bridge->qmi != qmi ? ENOENT : qmi->sock != NULL ? EBUSY : 0;
	if (error == 0) {
		struct hakios_ipcr **entry = &ipcr_bindings;
		while (*entry != NULL && *entry != bridge) {
			entry = &(*entry)->binding_next;
		}
		if (*entry == NULL) {
			error = EIO;
		} else {
			*entry = bridge->binding_next;
			bridge->binding_next = NULL;
			bridge->qmi = NULL;
		}
	}
	lck_mtx_unlock(&ipcr_bind_lock);
	return error;
}

int
hakios_ipcr_qmi_handle_init(struct qmi_handle *qmi, size_t largest, const struct qmi_ops *operations,
    const struct qmi_msg_handler *handlers)
{
	if (qmi == NULL) {
		return -EINVAL;
	}
	lck_mtx_lock(&ipcr_bind_lock);
	int error = -ENXIO;
	for (struct hakios_ipcr *entry = ipcr_bindings; entry != NULL; entry = entry->binding_next) {
		if (entry->qmi != qmi) {
			continue;
		}
		struct hakios_qrtr_endpoint endpoint;
		if (qmi->sock != NULL) {
			error = -EBUSY;
		} else {
			error = hakios_ipcr_endpoint(entry, &endpoint);
			error = error == 0 ? hakios_qmi_handle_init_endpoint(qmi, largest, operations, handlers, &endpoint) : -error;
		}
		break;
	}
	lck_mtx_unlock(&ipcr_bind_lock);
	return error;
}

int
hakios_ipcr_destroy(struct hakios_ipcr **bridge)
{
	if (bridge == NULL || *bridge == NULL) {
		return EINVAL;
	}
	struct hakios_ipcr *state = *bridge;
	lck_mtx_lock(&ipcr_bind_lock);
	bool bound = state->qmi != NULL;
	lck_mtx_unlock(&ipcr_bind_lock);
	if (bound) {
		return EBUSY;
	}
	int error = hakios_ipcr_stop(state);
	if (error == 0) {
		error = hakios_qrtr_mhi_destroy(&state->router);
	}
	if (error != 0) {
		return error;
	}
	error = hakios_mhi_ring_destroy(&state->rx.ring);
	if (error == 0) {
		error = hakios_mhi_ring_destroy(&state->tx.ring);
	}
	if (error != 0) {
		return error;
	}
	lck_mtx_destroy(&state->lock, &ipcr_group);
	kfree_type(struct hakios_ipcr, state);
	*bridge = NULL;
	return 0;
}

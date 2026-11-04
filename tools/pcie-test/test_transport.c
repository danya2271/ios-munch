#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/hakios.msi.h>
#include <sys/hakios.mhi.h>
#include <sys/hakios.qrtr.wire.h>
#include <sys/hakios.wire.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;
static unsigned msi_failures, mhi_failures;

struct fault {
	unsigned calls;
	unsigned fail_at;
	int error;
	bool after;
};

static int
callback(struct fault *fault)
{
	fault->calls++;
	sched_yield();
	return fault->calls == fault->fail_at ? fault->error : 0;
}

struct msi_owner {
	struct fault fault;
	struct hakios_msi *msi;
	struct hakios_msi_config config;
	uint8_t bytes[256];
	uint8_t original[256];
	unsigned writes;
	uint16_t ignore_write;
	bool is_64bit;
	bool armed;
};

static uint32_t
config_value(struct msi_owner *owner, uint16_t offset, uint8_t width)
{
	uint32_t value = 0;
	for (unsigned index = 0; index < width; index++) {
		value |= (uint32_t)owner->bytes[offset + index] << (index * 8);
	}
	return value;
}

static int
config_read(void *context, uint16_t offset, uint8_t width, uint32_t *value)
{
	struct msi_owner *owner = context;
	assert((width == 2 || width == 4) && offset % width == 0 && offset + width <= 256);
	int error = callback(&owner->fault);
	if (error == 0 || owner->fault.after) {
		*value = config_value(owner, offset, width);
	}
	return error;
}

static int
config_write(void *context, uint16_t offset, uint8_t width, uint32_t value)
{
	struct msi_owner *owner = context;
	assert((width == 2 && (offset == 0x52 || offset == (owner->is_64bit ? 0x5c : 0x58))) ||
	    (width == 4 && (offset == 0x54 || (owner->is_64bit ? offset == 0x58 || offset == 0x60 : offset == 0x5c))));
	assert(width == 4 || value <= UINT16_MAX);
	if (offset == 0x52) {
		assert((value & 0x18e) == (config_value(owner, offset, width) & 0x18e));
	}
	owner->writes++;
	int error = callback(&owner->fault);
	if ((error == 0 || owner->fault.after) && offset != owner->ignore_write) {
		for (unsigned index = 0; index < width; index++) {
			owner->bytes[offset + index] = (uint8_t)(value >> (index * 8));
		}
	}
	return error;
}

static int
msi_arm(void *context)
{
	struct msi_owner *owner = context;
	assert((config_value(owner, 0x52, 2) & 0x71) == 0x51);
	assert(config_value(owner, 0x54, 4) == (uint32_t)owner->config.doorbell_iova);
	if (owner->is_64bit) {
		assert(config_value(owner, 0x58, 4) == owner->config.doorbell_iova >> 32);
		assert(config_value(owner, 0x5c, 2) == owner->config.data);
	} else {
		assert(config_value(owner, 0x58, 2) == owner->config.data);
	}
	if (config_value(owner, 0x52, 2) & 0x100) {
		assert(config_value(owner, owner->is_64bit ? 0x60 : 0x5c, 4) == 0);
	}
	int error = callback(&owner->fault);
	if (error == 0 || owner->fault.after) {
		owner->armed = true;
	}
	return error;
}

static int
msi_quiesce(void *context)
{
	struct msi_owner *owner = context;
	int error = callback(&owner->fault);
	if (error == 0 || owner->fault.after) {
		owner->armed = false;
	}
	return error;
}

static const struct hakios_msi_operations msi_operations = {
	.version = HAKIOS_MSI_VERSION, .read = config_read, .write = config_write,
	.arm = msi_arm, .quiesce = msi_quiesce,
};

static void
msi_prepare_mode(struct msi_owner *owner, bool maskable, bool is_64bit, uint16_t base)
{
	memset(owner, 0, sizeof(*owner));
	memset(owner->bytes, 0xa5, sizeof(owner->bytes));
	owner->bytes[0x50] = 5;
	owner->bytes[0x51] = 0x70;
	owner->bytes[0x52] = is_64bit ? 0x8a : 0x0a;
	owner->bytes[0x53] = maskable ? 1 : 0;
	owner->is_64bit = is_64bit;
	memcpy(owner->original, owner->bytes, sizeof(owner->bytes));
	owner->config = (struct hakios_msi_config) {
		.doorbell_iova = is_64bit ? 0x123450040ull : 0x34500040ull,
		.capability = 0x50, .data = base
	};
	for (unsigned index = 0; index < 32; index++) {
		owner->config.intids[index] = 800 + index;
	}
	owner->fault.error = EIO;
}

static void
msi_prepare(struct msi_owner *owner, bool maskable, uint16_t base)
{
	msi_prepare_mode(owner, maskable, true, base);
}

static void
msi_init(struct msi_owner *owner, bool maskable)
{
	msi_prepare(owner, maskable, 0x400);
	assert(hakios_msi_create(&owner->config, &msi_operations, owner, &owner->msi) == 0);
	assert(owner->writes == 0);
}

static void
msi_init_32(struct msi_owner *owner, bool maskable)
{
	msi_prepare_mode(owner, maskable, false, 0x400);
	assert(hakios_msi_create(&owner->config, &msi_operations, owner, &owner->msi) == 0);
	assert(owner->writes == 0);
}

static void
msi_finish(struct msi_owner *owner)
{
	owner->fault.fail_at = 0;
	assert(hakios_msi_destroy(&owner->msi) == 0 && owner->msi == NULL);
	assert(!owner->armed && memcmp(owner->original, owner->bytes, sizeof(owner->bytes)) == 0);
}

static void
test_msi_layout_and_rejection(void)
{
	struct msi_owner owner;
	struct hakios_msi_message message;
	for (unsigned maskable = 0; maskable < 2; maskable++) {
		msi_prepare(&owner, maskable != 0, 0x400);
		assert(hakios_msi_create(&owner.config, &msi_operations, &owner, &owner.msi) == 0);
		assert(hakios_msi_message(owner.msi, HAKIOS_MSI_MHI, 0, &message) == ENXIO && message.address == 0);
		assert(hakios_msi_start(owner.msi) == 0 && owner.armed);
		assert(hakios_msi_start(owner.msi) == EBUSY);
		const unsigned counts[] = {3, 10, 1, 18};
		unsigned vector = 0;
		for (unsigned user = 0; user < 4; user++) {
			for (unsigned index = 0; index < counts[user]; index++, vector++) {
				assert(hakios_msi_message(owner.msi, user, index, &message) == 0);
				assert(message.address == owner.config.doorbell_iova && message.data == 0x400 + vector &&
				    message.intid == 800 + vector);
			}
			assert(hakios_msi_message(owner.msi, user, counts[user], &message) == EINVAL && message.address == 0);
		}
		assert(vector == 32);
		msi_finish(&owner);
	}
	for (unsigned maskable = 0; maskable < 2; maskable++) {
		msi_init_32(&owner, maskable != 0);
		assert(hakios_msi_start(owner.msi) == 0 && owner.armed);
		assert(hakios_msi_message(owner.msi, HAKIOS_MSI_MHI, 2, &message) == 0 &&
		    message.address == 0x34500040ull && message.data == 0x402 && message.intid == 802);
		msi_finish(&owner);
	}
	msi_prepare_mode(&owner, false, false, 0x400);
	owner.config.doorbell_iova = UINT64_C(0x100000000);
	assert(hakios_msi_create(&owner.config, &msi_operations, &owner, &owner.msi) == EOVERFLOW);
	assert(owner.msi == NULL && owner.writes == 0);
	for (unsigned invalid = 0; invalid < 8; invalid++) {
		msi_prepare(&owner, true, 0x400);
		switch (invalid) {
		case 0: owner.config.doorbell_iova++; break;
		case 1: owner.config.data++; break;
		case 2: owner.config.intids[31] = owner.config.intids[30]; break;
		case 3: owner.config.capability = 0xf0; break;
		case 4: owner.bytes[0x52] |= 1; break;
		case 5: owner.bytes[0x52] &= ~0x80; break;
		case 6: owner.bytes[0x50] = 0x11; break;
		case 7: owner.bytes[0x52] = 0x88; break;
		}
		assert(hakios_msi_create(&owner.config, &msi_operations, &owner, &owner.msi) != 0);
		assert(owner.msi == NULL && owner.writes == 0);
	}
	msi_prepare(&owner, false, 0x400);
	pcie_test_fail_allocation = true;
	assert(hakios_msi_create(&owner.config, &msi_operations, &owner, &owner.msi) == ENOMEM);
	pcie_test_fail_allocation = false;
	msi_init(&owner, true);
	owner.ignore_write = 0x54;
	assert(hakios_msi_start(owner.msi) == EIO && !owner.armed);
	assert(hakios_msi_message(owner.msi, HAKIOS_MSI_DP, 0, &message) == EIO && message.address == 0);
	msi_finish(&owner);
}

static void
test_msi_faults(void)
{
	const int errors[] = {EIO, EINVAL, -ETIMEDOUT};
	for (unsigned maskable = 0; maskable < 2; maskable++) {
		struct msi_owner probe;
		msi_init(&probe, maskable != 0);
		unsigned create_calls = probe.fault.calls;
		msi_finish(&probe);
		for (unsigned after = 0; after < 2; after++) {
			for (unsigned failure = 1; failure <= create_calls; failure++) {
				for (unsigned code = 0; code < 3; code++) {
					msi_prepare(&probe, maskable != 0, 0x400);
					probe.fault.fail_at = failure;
					probe.fault.after = after != 0;
					probe.fault.error = errors[code];
					assert(hakios_msi_create(&probe.config, &msi_operations, &probe, &probe.msi) ==
					    (errors[code] < 0 ? EIO : errors[code]));
					assert(probe.msi == NULL && probe.writes == 0 && memcmp(probe.bytes, probe.original, 256) == 0);
					msi_failures++;
				}
			}
		}
		for (unsigned teardown = 0; teardown < 2; teardown++) {
			struct msi_owner baseline;
			msi_init(&baseline, maskable != 0);
			if (teardown) {
				assert(hakios_msi_start(baseline.msi) == 0);
			}
			baseline.fault.calls = 0;
			assert((teardown ? hakios_msi_destroy(&baseline.msi) : hakios_msi_start(baseline.msi)) == 0);
			unsigned calls = baseline.fault.calls;
			if (!teardown) {
				msi_finish(&baseline);
			}
			for (unsigned after = 0; after < 2; after++) {
				for (unsigned failure = 1; failure <= calls; failure++) {
					for (unsigned code = 0; code < 3; code++) {
						struct msi_owner owner;
						msi_init(&owner, maskable != 0);
						if (teardown) {
							assert(hakios_msi_start(owner.msi) == 0);
						}
						owner.fault.calls = 0;
						owner.fault.fail_at = failure;
						owner.fault.after = after != 0;
						owner.fault.error = errors[code];
						struct hakios_msi *retained = owner.msi;
						int error = teardown ? hakios_msi_destroy(&owner.msi) : hakios_msi_start(owner.msi);
						assert(error == (errors[code] < 0 ? EIO : errors[code]) && owner.msi == retained);
						unsigned stopped = owner.fault.calls;
						struct hakios_msi_message message;
						assert(hakios_msi_message(owner.msi, HAKIOS_MSI_MHI, 0, &message) == EIO && message.address == 0);
						assert(hakios_msi_start(owner.msi) == EIO && owner.fault.calls == stopped);
						msi_finish(&owner);
						msi_failures++;
					}
				}
			}
		}
	}
}

#define RING_IOVA 0x120000000ull
#define DATA_IOVA 0x130000000ull

struct mhi_owner {
	struct fault fault;
	struct hakios_mhi_ring *ring;
	uint8_t *cpu;
	uint8_t visible[HAKIOS_MHI_RING_SIZE];
	bool device_owned[1024];
	uint8_t data_cpu[8192];
	uint8_t data_device[8192];
	uint64_t last_cookie;
	uint64_t published;
	uint8_t channel;
	bool stopped;
};

static int
buffer_sync(void *context, uint64_t cookie, bool for_device, uint32_t length)
{
	struct mhi_owner *owner = context;
	assert(cookie < 1024 && length <= 8192);
	int error = callback(&owner->fault);
	if (error == 0 || owner->fault.after) {
		owner->device_owned[cookie] = for_device;
		if (for_device) {
			owner->last_cookie = cookie;
		}
		if (cookie == 1) {
			if (for_device && owner->channel == 20) {
				memcpy(owner->data_device, owner->data_cpu, length);
			} else if (!for_device && owner->channel == 21) {
				memcpy(owner->data_cpu, owner->data_device, length);
			}
		}
	}
	return error;
}

static int
ring_sync(void *context, uint32_t offset, uint32_t length)
{
	struct mhi_owner *owner = context;
	assert(offset % 16 == 0 && length == 16 && offset + length <= 1024);
	assert(owner->device_owned[owner->last_cookie]);
	int error = callback(&owner->fault);
	if (error == 0 || owner->fault.after) {
		memcpy(owner->visible + offset, owner->cpu + offset, length);
	}
	return error;
}

static int
ring_publish(void *context, uint8_t channel, uint64_t next_wp)
{
	struct mhi_owner *owner = context;
	assert(channel == owner->channel && next_wp >= RING_IOVA && next_wp < RING_IOVA + 1024);
	unsigned slot = ((unsigned)((next_wp - RING_IOVA) / 16) + 63) % 64;
	const uint8_t *tre = owner->visible + slot * 16;
	assert(hakios_wire_get64(tre) == DATA_IOVA + owner->last_cookie * 0x4000);
	const uint8_t flags[] = {0, 2, 2, 0};
	assert(memcmp(tre + 12, flags, sizeof(flags)) == 0 && owner->device_owned[owner->last_cookie]);
	int error = callback(&owner->fault);
	if (error == 0 || owner->fault.after) {
		owner->published = next_wp;
	}
	return error;
}

static int
ring_quiesce(void *context)
{
	struct mhi_owner *owner = context;
	int error = callback(&owner->fault);
	if (error == 0 || owner->fault.after) {
		owner->stopped = true;
		memset(owner->device_owned, 0, sizeof(owner->device_owned));
	}
	return error;
}

static const struct hakios_mhi_ring_operations ring_operations = {
	.version = HAKIOS_MHI_VERSION, .sync_buffer = buffer_sync, .sync_ring = ring_sync,
	.publish = ring_publish, .quiesce = ring_quiesce,
};

static void
ring_init(struct mhi_owner *owner, uint8_t channel)
{
	memset(owner, 0, sizeof(*owner));
	owner->cpu = aligned_alloc(1024, 1024);
	assert(owner->cpu != NULL);
	owner->channel = channel;
	owner->fault.error = EIO;
	struct hakios_mhi_ring_config config = {.elements = owner->cpu, .iova = RING_IOVA, .channel = channel};
	assert(hakios_mhi_ring_create(&config, &ring_operations, owner, &owner->ring) == 0);
	assert(owner->fault.calls == 0);
}

static void
ring_finish(struct mhi_owner *owner)
{
	owner->fault.fail_at = 0;
	assert(hakios_mhi_ring_destroy(&owner->ring) == 0 && owner->ring == NULL && owner->stopped);
	free(owner->cpu);
}

static int
queue(struct mhi_owner *owner, uint64_t cookie, uint32_t length)
{
	return hakios_mhi_ring_queue(owner->ring, cookie, DATA_IOVA + cookie * 0x4000, length);
}

static void
make_event(uint8_t event[16], uint8_t channel, unsigned slot, unsigned code, unsigned length)
{
	hakios_wire_put64(event, RING_IOVA + slot * 16);
	hakios_wire_put32(event + 8, code << 24 | length);
	hakios_wire_put32(event + 12, (uint32_t)channel << 24 | 0x22u << 16);
}

static void
test_mhi_geometry_and_completion(void)
{
	uint8_t context[44], command[16], event[16];
	assert(hakios_mhi_channel_context(20, RING_IOVA, context) == 0);
	assert(context[0] == 0 && context[1] == 2 && context[4] == 1 && context[8] == 1);
	assert(hakios_wire_get64(context + 12) == RING_IOVA && hakios_wire_get64(context + 20) == 1024);
	assert(hakios_wire_get64(context + 28) == RING_IOVA && hakios_wire_get64(context + 36) == RING_IOVA);
	assert(hakios_mhi_command(21, 0x12, command) == 0);
	const uint8_t start[] = {0, 0, 0x12, 21};
	assert(memcmp(command + 12, start, 4) == 0);
	assert(hakios_mhi_command(21, 0x49, command) == ENOTSUP);
	assert(hakios_mhi_channel_context(21, RING_IOVA + 1, context) == EINVAL);
	for (unsigned channel = 20; channel <= 21; channel++) {
		struct mhi_owner owner;
		ring_init(&owner, channel);
		assert(hakios_mhi_ring_queue(owner.ring, 1, RING_IOVA, 64) == EINVAL);
		assert(hakios_mhi_ring_queue(owner.ring, 1, UINT64_MAX, 64) == EINVAL);
		assert(queue(&owner, 1, 8193) == EINVAL && owner.fault.calls == 0);
		for (unsigned index = 1; index <= 63; index++) {
			assert(queue(&owner, index, 64) == 0);
		}
		assert(queue(&owner, 64, 64) == ENOSPC);
		make_event(event, channel, 62, 2, 13);
		struct hakios_mhi_completion completions[63];
		unsigned count = 99, calls = owner.fault.calls;
		assert(hakios_mhi_ring_complete(owner.ring, event, completions, 62, &count) == ENOSPC && count == 0);
		assert(owner.fault.calls == calls && hakios_mhi_ring_status(owner.ring) == 0);
		assert(hakios_mhi_ring_complete(owner.ring, event, completions, 63, &count) == 0 && count == 63);
		for (unsigned index = 0; index < 63; index++) {
			assert(completions[index].cookie == index + 1 && completions[index].length == (index == 62 ? 13 : 64));
		}
		for (unsigned round = 0; round < 100; round++) {
			assert(queue(&owner, 1, 64) == 0);
			assert(queue(&owner, 1, 64) == EEXIST);
			assert(hakios_mhi_ring_queue(owner.ring, 2, DATA_IOVA + 0x4001, 64) == EEXIST);
			make_event(event, channel, (63 + round) % 64, round % 2 ? 2 : 3, 32);
			assert(hakios_mhi_ring_complete(owner.ring, event, completions, 63, &count) == 0 && count == 1);
			assert(completions[0].error == (round % 2 ? 0 : EOVERFLOW));
		}
		assert(queue(&owner, 1, 64) == 0 && queue(&owner, 2, 64) == 0);
		make_event(event, channel, (63 + 101) % 64, 3, 64);
		assert(hakios_mhi_ring_complete(owner.ring, event, completions, 63, &count) == 0 && count == 2);
		assert(completions[0].error == EOVERFLOW && completions[1].error == EOVERFLOW);
		assert(hakios_mhi_ring_complete(owner.ring, event, completions, 63, &count) == EPROTO && count == 0);
		ring_finish(&owner);
	}
}

static void
test_mhi_faults(void)
{
	const int errors[] = {EIO, EINVAL, -ETIMEDOUT};
	for (unsigned complete = 0; complete < 2; complete++) {
		for (unsigned after = 0; after < 2; after++) {
			for (unsigned failure = 1; failure <= 3; failure++) {
				for (unsigned code = 0; code < 3; code++) {
					struct mhi_owner owner;
					ring_init(&owner, 21);
					if (complete) {
						for (unsigned cookie = 1; cookie <= 3; cookie++) {
							assert(queue(&owner, cookie, 64) == 0);
						}
					}
					owner.fault.calls = 0;
					owner.fault.fail_at = failure;
					owner.fault.after = after != 0;
					owner.fault.error = errors[code];
					uint8_t event[16];
					make_event(event, 21, 2, 2, 16);
					struct hakios_mhi_completion completions[3];
					unsigned count = 0;
					int error = complete ? hakios_mhi_ring_complete(owner.ring, event, completions, 3, &count) : queue(&owner, 1, 64);
					assert(error == (errors[code] < 0 ? EIO : errors[code]) && count == 0);
					assert(hakios_mhi_ring_status(owner.ring) == EIO);
					unsigned stopped = owner.fault.calls;
					assert(queue(&owner, 99, 64) == EIO);
					assert(hakios_mhi_ring_complete(owner.ring, event, completions, 3, &count) == EIO && count == 0);
					assert(owner.fault.calls == stopped);
					owner.fault.fail_at = owner.fault.calls + 1;
					struct hakios_mhi_ring *retained = owner.ring;
					assert(hakios_mhi_ring_destroy(&owner.ring) != 0 && owner.ring == retained);
					ring_finish(&owner);
					mhi_failures++;
				}
			}
		}
	}
	for (unsigned invalid = 0; invalid < 7; invalid++) {
		struct mhi_owner owner;
		ring_init(&owner, 21);
		assert(queue(&owner, 1, 64) == 0);
		uint8_t event[16];
		make_event(event, 21, 0, 2, 32);
		switch (invalid) {
		case 0: event[15] = 20; break;
		case 1: event[14] = 0x21; break;
		case 2: event[11] = 0x11; break;
		case 3: event[0]++; break;
		case 4: hakios_wire_put64(event, RING_IOVA + 1024); break;
		case 5: hakios_wire_put64(event, RING_IOVA + 16); break;
		case 6: event[8] = 65; break;
		}
		struct hakios_mhi_completion result;
		unsigned count = 99;
		assert(hakios_mhi_ring_complete(owner.ring, event, &result, 1, &count) == EPROTO && count == 0);
		assert(hakios_mhi_ring_status(owner.ring) == EIO);
		ring_finish(&owner);
	}
}

struct ring_worker {
	struct mhi_owner *owner;
	unsigned first_cookie;
};

static void *
ring_producer(void *context)
{
	struct ring_worker *worker = context;
	for (unsigned index = 0; index < 12; index++) {
		assert(queue(worker->owner, worker->first_cookie + index, 64) == 0);
	}
	return NULL;
}

static void
test_mhi_concurrent_queue(void)
{
	struct mhi_owner owner;
	ring_init(&owner, 20);
	pthread_t threads[4];
	struct ring_worker workers[4];
	for (unsigned index = 0; index < 4; index++) {
		workers[index] = (struct ring_worker) {.owner = &owner, .first_cookie = 1 + 12 * index};
		assert(pthread_create(&threads[index], NULL, ring_producer, &workers[index]) == 0);
	}
	for (unsigned index = 0; index < 4; index++) {
		assert(pthread_join(threads[index], NULL) == 0);
	}
	uint8_t event[16];
	make_event(event, 20, 47, 2, 64);
	struct hakios_mhi_completion completions[63];
	unsigned count;
	bool seen[49] = {0};
	assert(hakios_mhi_ring_complete(owner.ring, event, completions, 63, &count) == 0 && count == 48);
	for (unsigned index = 0; index < count; index++) {
		uint64_t cookie = completions[index].cookie;
		assert(cookie >= 1 && cookie <= 48 && !seen[cookie]);
		seen[cookie] = true;
	}
	ring_finish(&owner);
}

static void
test_qrtr_wire(void)
{
	uint8_t frame[8192], payload[8192], reply[64];
	memset(payload, 0x6b, sizeof(payload));
	struct hakios_qrtr_packet packet = {
		.type = HAKIOS_QRTR_DATA, .source_node = 1, .source_port = 0x4000,
		.destination_node = 7, .destination_port = 0x1234, .confirm_rx = true,
		.payload = payload, .length = 5,
	};
	size_t size, reply_size;
	assert(hakios_qrtr_wire_encode(&packet, frame, sizeof(frame), &size) == 0 && size == 40);
	const uint8_t header[] = {1,0,0,0, 1,0,0,0, 1,0,0,0, 0,0x40,0,0,
	    1,0,0,0, 5,0,0,0, 7,0,0,0, 0x34,0x12,0,0};
	assert(memcmp(frame, header, 32) == 0 && frame[37] == 0 && frame[38] == 0 && frame[39] == 0);
	struct hakios_qrtr_packet decoded;
	assert(hakios_qrtr_wire_decode(frame, size, &decoded) == 0 && decoded.length == 5 && decoded.confirm_rx);
	assert(memcmp(decoded.payload, payload, 5) == 0);
	assert(hakios_qrtr_wire_resume(&decoded, reply, sizeof(reply), &reply_size) == 0 && reply_size == 52);
	assert(hakios_qrtr_wire_decode(reply, reply_size, &decoded) == 0 && decoded.type == HAKIOS_QRTR_RESUME_TX);
	assert(decoded.source_node == 7 && decoded.source_port == 0x1234 && decoded.destination_node == 1 && decoded.destination_port == 0x4000);
	struct hakios_qrtr_control control;
	assert(hakios_qrtr_control_decode(&decoded, &control) == 0 && control.node == 7 && control.port == 0x1234);
	for (unsigned length = 1; length <= 8160; length++) {
		packet.length = length;
		assert(hakios_qrtr_wire_encode(&packet, frame, sizeof(frame), &size) == 0);
		assert(hakios_qrtr_wire_decode(frame, size, &decoded) == 0 && decoded.length == length);
	}
	packet.length = 8161;
	assert(hakios_qrtr_wire_encode(&packet, frame, sizeof(frame), &size) == EMSGSIZE && size == 0);
	packet.length = 5;
	assert(hakios_qrtr_wire_encode(&packet, frame, 39, &size) == EMSGSIZE && size == 0);
	packet.destination_port = HAKIOS_QRTR_CONTROL_PORT;
	const unsigned types[] = {2,3,4,5,6,7,10,11};
	for (unsigned index = 0; index < sizeof(types) / sizeof(types[0]); index++) {
		control = (struct hakios_qrtr_control) {.type = types[index], .service = 0x45,
		    .instance = 0x123456, .version = 1, .node = 7, .port = 0x1234};
		assert(hakios_qrtr_control_encode(&control, &packet, frame, sizeof(frame), &size) == 0);
		assert(hakios_qrtr_wire_decode(frame, size, &decoded) == 0);
		assert(hakios_qrtr_control_decode(&decoded, &control) == 0 && control.type == types[index]);
		if (types[index] == 4 || types[index] == 5 || types[index] >= 10) {
			assert(control.instance == 0x123456 && control.version == 1 && control.service == 0x45);
		}
	}
	frame[32] = HAKIOS_QRTR_HELLO;
	assert(hakios_qrtr_wire_decode(frame, size, &decoded) == EPROTO && decoded.payload == NULL);
	const uint8_t control_v3[] = {3,4,0,0, 20,0,0,0, 7,0,0xfe,0xff, 1,0,0xff,0xff,
	    4,0,0,0, 0x45,0,0,0, 1,2,0,0, 7,0,0,0, 0x34,0x12,0,0};
	assert(hakios_qrtr_wire_decode(control_v3, sizeof(control_v3), &decoded) == 0);
	assert(decoded.source_port == HAKIOS_QRTR_CONTROL_PORT && decoded.destination_port == HAKIOS_QRTR_CONTROL_PORT);
	assert(hakios_qrtr_control_decode(&decoded, &control) == 0 && control.version == 1 && control.instance == 2);
	const uint8_t v3[] = {3,1,1,0, 1,0,0,0, 7,0,0x34,0x12, 1,0,0,0x40, 0xaa,0,0,0};
	assert(hakios_qrtr_wire_decode(v3, sizeof(v3), &decoded) == 0 && decoded.source_node == 7 && decoded.payload[0] == 0xaa);
	for (unsigned truncated = 0; truncated < sizeof(v3); truncated++) {
		assert(hakios_qrtr_wire_decode(v3, truncated, &decoded) != 0 && decoded.payload == NULL);
	}
	for (unsigned invalid = 0; invalid < 7; invalid++) {
		memcpy(frame, v3, sizeof(v3));
		switch (invalid) {
		case 0: frame[0] = 2; break;
		case 1: frame[1] = 0xff; break;
		case 2: frame[2] = 2; break;
		case 3: frame[3] = 4; break;
		case 4: hakios_wire_put32(frame + 4, UINT32_MAX); break;
		case 5: frame[4] = 0; break;
		case 6: frame[1] = HAKIOS_QRTR_NEW_SERVER; break;
		}
		assert(hakios_qrtr_wire_decode(frame, sizeof(v3), &decoded) != 0 && decoded.payload == NULL);
	}
}

static void
test_ipcr_pipeline(void)
{
	struct msi_owner msi;
	msi_init(&msi, false);
	assert(hakios_msi_start(msi.msi) == 0);
	struct hakios_msi_message message;
	assert(hakios_msi_message(msi.msi, HAKIOS_MSI_MHI, 2, &message) == 0 && message.intid == 802);
	struct mhi_owner tx, rx;
	ring_init(&tx, 20);
	ring_init(&rx, 21);
	const uint8_t qmi[] = {0, 1, 0, 0x20, 0, 0, 0};
	struct hakios_qrtr_packet packet = {.type = HAKIOS_QRTR_DATA, .source_node = 1, .source_port = 0x4000,
	    .destination_node = 7, .destination_port = 0x1234, .payload = qmi, .length = sizeof(qmi), .confirm_rx = true};
	size_t size;
	assert(hakios_qrtr_wire_encode(&packet, tx.data_cpu, sizeof(tx.data_cpu), &size) == 0);
	assert(queue(&tx, 1, size) == 0);
	assert(hakios_qrtr_wire_decode(tx.data_device, size, &packet) == 0 && memcmp(packet.payload, qmi, sizeof(qmi)) == 0);
	assert(queue(&rx, 1, sizeof(rx.data_cpu)) == 0);
	memcpy(rx.data_device, tx.data_device, size);
	uint8_t event[16];
	make_event(event, 21, 0, 2, size);
	struct hakios_mhi_completion result;
	unsigned count;
	assert(hakios_mhi_ring_complete(rx.ring, event, &result, 1, &count) == 0 && result.length == size);
	assert(hakios_qrtr_wire_decode(rx.data_cpu, result.length, &packet) == 0);
	uint8_t resume[52];
	assert(hakios_qrtr_wire_resume(&packet, resume, sizeof(resume), &size) == 0 && size == 52);
	make_event(event, 20, 0, 2, 40);
	assert(hakios_mhi_ring_complete(tx.ring, event, &result, 1, &count) == 0);
	ring_finish(&rx);
	ring_finish(&tx);
	msi_finish(&msi);
}

int
main(void)
{
	test_msi_layout_and_rejection();
	test_msi_faults();
	test_mhi_geometry_and_completion();
	test_mhi_faults();
	test_mhi_concurrent_queue();
	test_qrtr_wire();
	test_ipcr_pipeline();
	printf("transport-test: PASS (%u MSI and %u MHI callback failures; QRTR framing, control, IPCR model)\n", msi_failures, mhi_failures);
	return 0;
}

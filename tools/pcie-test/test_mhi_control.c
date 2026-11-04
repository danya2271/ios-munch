#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/hakios.mhi.h>
#include <sys/hakios.wire.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;
static unsigned injected_failures;

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

static void
event_bytes(uint8_t *event, uint64_t pointer, uint8_t type, uint8_t channel, uint8_t code, uint16_t length)
{
	hakios_wire_put64(event, pointer);
	hakios_wire_put32(event + 8, (uint32_t)code << 24 | length);
	hakios_wire_put32(event + 12, (uint32_t)channel << 24 | (uint32_t)type << 16);
}

struct command_model {
	struct fault fault;
	struct hakios_mhi_command_ring *ring;
	struct hakios_mhi_command_config config;
	_Alignas(16) uint8_t cpu[2048];
	uint8_t device[2048];
	uint64_t wp;
	unsigned syncs;
	unsigned published;
	bool stopped;
};

static int
command_sync(void *context, uint32_t offset, uint32_t length)
{
	struct command_model *model = context;
	assert(offset < 2048 && offset % 16 == 0 && length == 16);
	assert(offset == (model->syncs % 128) * 16);
	int error = callback(&model->fault);
	if (error == 0 || model->fault.after) {
		memcpy(model->device + offset, model->cpu + offset, length);
		model->syncs++;
	}
	return error;
}

static int
command_publish(void *context, uint64_t next_wp)
{
	struct command_model *model = context;
	assert(model->syncs == model->published + 1);
	assert(next_wp == model->config.iova + (model->syncs % 128) * 16);
	unsigned offset = (model->published % 128) * 16;
	assert(memcmp(model->cpu + offset, model->device + offset, 16) == 0);
	int error = callback(&model->fault);
	if (error == 0 || model->fault.after) {
		model->wp = next_wp;
		model->published++;
	}
	return error;
}

static int
command_quiesce(void *context)
{
	struct command_model *model = context;
	int error = callback(&model->fault);
	if (error == 0 || model->fault.after) {
		model->stopped = true;
	}
	return error;
}

static const struct hakios_mhi_command_operations command_operations = {
	.version = HAKIOS_MHI_VERSION, .sync_ring = command_sync,
	.publish = command_publish, .quiesce = command_quiesce,
};

static void
command_init(struct command_model *model)
{
	memset(model, 0, sizeof(*model));
	model->config = (struct hakios_mhi_command_config) {.elements = model->cpu, .iova = 0x100002000ULL};
	assert(hakios_mhi_command_create(&model->config, &command_operations, model, &model->ring) == 0);
	assert(hakios_mhi_command_status(model->ring) == 0);
}

static void
command_finish(struct command_model *model, unsigned slot, uint64_t cookie, uint8_t channel, uint8_t command)
{
	uint8_t event[16];
	struct hakios_mhi_command_completion completion;
	event_bytes(event, model->config.iova + slot * 16, 0x21, 0xfe, 1, 0);
	assert(hakios_mhi_command_complete(model->ring, event, &completion) == 0);
	assert(completion.cookie == cookie && completion.channel == channel && completion.command == command);
}

struct event_model {
	struct fault fault;
	struct hakios_mhi_event_ring *ring;
	struct hakios_mhi_event_config config;
	struct command_model *commands;
	struct hakios_mhi_ring *transfer;
	_Alignas(16) uint8_t cpu[4096];
	uint8_t device[4096];
	uint64_t delivered[2048];
	struct hakios_mhi_command_completion command_result;
	struct hakios_mhi_completion transfers[63];
	uint32_t transfer_capacity;
	uint32_t transfer_count;
	uint32_t elements;
	uint64_t device_rp;
	uint64_t wp;
	unsigned produced;
	unsigned credits;
	unsigned accepted;
	unsigned published;
	unsigned returned;
	unsigned stalls;
	unsigned stall_after;
	int stall_error;
	bool ready;
	bool stopped;
	bool refill;
};

static void push_sequence(struct event_model *model);

static int
event_sync(void *context, uint32_t offset, uint32_t length, bool for_device)
{
	struct event_model *model = context;
	assert(offset % 16 == 0 && offset + length <= model->elements * 16);
	int error = callback(&model->fault);
	if (error != 0 && !model->fault.after) {
		return error;
	}
	if (!model->ready) {
		assert(for_device && offset == 0 && length == model->elements * 16);
		memcpy(model->device, model->cpu, length);
	} else if (!for_device) {
		assert(length == 16 && offset == (model->accepted % model->elements) * 16);
		memcpy(model->cpu + offset, model->device + offset, length);
	} else {
		assert(length == 16 && model->accepted == model->returned + 1);
		assert(offset == (model->returned % model->elements) * 16);
		model->returned++;
	}
	return error;
}

static int
event_pointer(void *context, uint64_t *pointer)
{
	struct event_model *model = context;
	int error = callback(&model->fault);
	if (error == 0 || model->fault.after) {
		*pointer = model->device_rp;
	}
	return error;
}

static int
event_publish(void *context, uint64_t credit_wp)
{
	struct event_model *model = context;
	if (!model->ready) {
		assert(credit_wp == model->config.iova + (model->elements - 1) * 16);
	} else {
		assert(model->returned == model->published + 1);
		assert(credit_wp == model->config.iova + (model->published % model->elements) * 16);
	}
	int error = callback(&model->fault);
	if (error == 0 || model->fault.after) {
		if (!model->ready) {
			model->credits = model->elements - 1;
			model->ready = true;
		} else {
			model->credits++;
			model->published++;
			if (model->refill) {
				push_sequence(model);
			}
		}
		model->wp = credit_wp;
	}
	return error;
}

static int
event_dispatch(void *context, const uint8_t *event)
{
	struct event_model *model = context;
	assert((uintptr_t)event < (uintptr_t)model->cpu || (uintptr_t)event >= (uintptr_t)(model->cpu + 4096));
	assert(model->accepted < 2048);
	assert(memcmp(event, model->device + (model->accepted % model->elements) * 16, 16) == 0);
	int error = callback(&model->fault);
	if (error != 0 && !model->fault.after) {
		return error;
	}
	if (model->stalls != 0 && model->accepted >= model->stall_after) {
		model->stalls--;
		return model->stall_error;
	}
	int result = 0;
	uint32_t type = (hakios_wire_get32(event + 12) >> 16) & 255;
	if (model->commands != NULL && type == 0x21) {
		result = hakios_mhi_command_complete(model->commands->ring, event, &model->command_result);
	} else if (model->transfer != NULL && type == 0x22) {
		result = hakios_mhi_ring_complete(model->transfer, event, model->transfers,
		    model->transfer_capacity, &model->transfer_count);
	}
	if (result != 0) {
		return result;
	}
	model->delivered[model->accepted++] = hakios_wire_get64(event);
	return error;
}

static int
event_quiesce(void *context)
{
	struct event_model *model = context;
	int error = callback(&model->fault);
	if (error == 0 || model->fault.after) {
		model->stopped = true;
	}
	return error;
}

static const struct hakios_mhi_event_operations event_operations = {
	.version = HAKIOS_MHI_VERSION, .sync_ring = event_sync, .read_pointer = event_pointer,
	.publish = event_publish, .dispatch = event_dispatch, .quiesce = event_quiesce,
};

static void
event_init(struct event_model *model, uint8_t index)
{
	memset(model, 0, sizeof(*model));
	model->config = (struct hakios_mhi_event_config) {.elements = model->cpu, .iova = 0x100004000ULL, .index = index};
	model->elements = index == 0 ? 32 : 256;
	model->device_rp = model->config.iova;
	model->transfer_capacity = 63;
	assert(hakios_mhi_event_create(&model->config, &event_operations, model, &model->ring) == 0);
	assert(hakios_mhi_event_status(model->ring) == EAGAIN);
}

static bool
device_push(struct event_model *model, const uint8_t *event)
{
	if (model->credits == 0) {
		return false;
	}
	assert(model->ready && !model->stopped);
	memcpy(model->device + (model->produced % model->elements) * 16, event, 16);
	model->produced++;
	model->credits--;
	model->device_rp = model->config.iova + (model->produced % model->elements) * 16;
	return true;
}

static void
push_sequence(struct event_model *model)
{
	uint8_t event[16];
	event_bytes(event, model->produced + 1, model->config.index == 0 ? 0x20 : 0x22, 20, 2, 1);
	assert(device_push(model, event));
}

static void
test_contexts(void)
{
	uint8_t bytes[HAKIOS_MHI_CONTEXT_SIZE + 1];
	uint8_t *context = bytes + 1;
	uint64_t base = 0x100004000ULL;
	assert(hakios_mhi_command_context(base, context) == 0);
	for (unsigned index = 0; index < 12; index++) {
		assert(context[index] == 0);
	}
	assert(hakios_wire_get64(context + 12) == base && hakios_wire_get64(context + 20) == 2048);
	assert(hakios_wire_get64(context + 28) == base && hakios_wire_get64(context + 36) == base);
	for (unsigned index = 0; index < 2; index++) {
		assert(hakios_mhi_event_context(index, base, context) == 0);
		assert(hakios_wire_get32(context) == (index == 0 ? 0 : 0x10000));
		assert(hakios_wire_get32(context + 4) == 1 && hakios_wire_get32(context + 8) == index + 1);
		assert(hakios_wire_get64(context + 12) == base);
		assert(hakios_wire_get64(context + 20) == (index == 0 ? 512 : 4096));
		assert(hakios_wire_get64(context + 28) == base && hakios_wire_get64(context + 36) == base);
	}
	assert(hakios_mhi_event_context(2, base, context) == EINVAL);
	assert(hakios_mhi_event_context(1, base + 512, context) == EINVAL);
	assert(hakios_mhi_event_context(0, 0, context) == EINVAL);
	assert(hakios_mhi_event_context(1, UINT64_MAX & ~4095ULL, context) == EINVAL);
	assert(hakios_mhi_command_context(base + 16, context) == EINVAL);
	assert(hakios_mhi_command_context(0, context) == EINVAL);
	assert(hakios_mhi_command_context(UINT64_MAX & ~2047ULL, context) == EINVAL);
	for (unsigned index = 0; index < HAKIOS_MHI_CONTEXT_SIZE; index++) {
		assert(context[index] == 0);
	}
	assert(hakios_mhi_command_context(base, NULL) == EINVAL);
	assert(hakios_mhi_event_context(0, base, NULL) == EINVAL);
}

static void
test_constructors(void)
{
	struct event_model events;
	struct command_model commands;
	event_init(&events, 0);
	command_init(&commands);
	struct hakios_mhi_event_ring *event_ring = NULL;
	struct hakios_mhi_command_ring *command_ring = NULL;
	pcie_test_fail_allocation = true;
	assert(hakios_mhi_event_create(&events.config, &event_operations, &events, &event_ring) == ENOMEM);
	assert(hakios_mhi_command_create(&commands.config, &command_operations, &commands, &command_ring) == ENOMEM);
	pcie_test_fail_allocation = false;
	for (unsigned variant = 0; variant < 10; variant++) {
		struct hakios_mhi_event_config config = events.config;
		struct hakios_mhi_event_operations operations = event_operations;
		switch (variant) {
		case 0: config.elements = NULL; break;
		case 1: config.elements++; break;
		case 2: config.iova++; break;
		case 3: config.index = 2; break;
		case 4: operations.version++; break;
		case 5: operations.sync_ring = NULL; break;
		case 6: operations.read_pointer = NULL; break;
		case 7: operations.publish = NULL; break;
		case 8: operations.dispatch = NULL; break;
		case 9: operations.quiesce = NULL; break;
		}
		assert(hakios_mhi_event_create(&config, &operations, &events, &event_ring) == EINVAL);
		assert(event_ring == NULL);
	}
	for (unsigned variant = 0; variant < 7; variant++) {
		struct hakios_mhi_command_config config = commands.config;
		struct hakios_mhi_command_operations operations = command_operations;
		switch (variant) {
		case 0: config.elements = NULL; break;
		case 1: config.elements++; break;
		case 2: config.iova++; break;
		case 3: operations.version++; break;
		case 4: operations.sync_ring = NULL; break;
		case 5: operations.publish = NULL; break;
		case 6: operations.quiesce = NULL; break;
		}
		assert(hakios_mhi_command_create(&config, &operations, &commands, &command_ring) == EINVAL);
		assert(command_ring == NULL);
	}
	assert(hakios_mhi_event_create(NULL, &event_operations, &events, &event_ring) == EINVAL);
	assert(hakios_mhi_event_create(&events.config, NULL, &events, &event_ring) == EINVAL);
	assert(hakios_mhi_event_create(&events.config, &event_operations, NULL, &event_ring) == EINVAL);
	assert(hakios_mhi_event_create(&events.config, &event_operations, &events, NULL) == EINVAL);
	assert(hakios_mhi_command_create(NULL, &command_operations, &commands, &command_ring) == EINVAL);
	assert(hakios_mhi_command_create(&commands.config, NULL, &commands, &command_ring) == EINVAL);
	assert(hakios_mhi_command_create(&commands.config, &command_operations, NULL, &command_ring) == EINVAL);
	assert(hakios_mhi_command_create(&commands.config, &command_operations, &commands, NULL) == EINVAL);
	assert(hakios_mhi_event_status(NULL) == EINVAL && hakios_mhi_command_status(NULL) == EINVAL);
	assert(hakios_mhi_event_start(NULL) == EINVAL);
	assert(hakios_mhi_event_destroy(NULL) == EINVAL && hakios_mhi_event_destroy(&event_ring) == EINVAL);
	assert(hakios_mhi_command_destroy(NULL) == EINVAL && hakios_mhi_command_destroy(&command_ring) == EINVAL);
	assert(hakios_mhi_event_destroy(&events.ring) == 0 && events.stopped);
	assert(hakios_mhi_command_destroy(&commands.ring) == 0 && commands.stopped);
}

static void
test_commands(void)
{
	struct command_model model;
	command_init(&model);
	assert(hakios_mhi_command_submit(model.ring, 0, 20, 0x12) == EINVAL);
	assert(hakios_mhi_command_submit(model.ring, 1, 19, 0x12) == ENOTSUP);
	assert(hakios_mhi_command_submit(model.ring, 1, 20, 0x13) == ENOTSUP);
	assert(model.fault.calls == 0);
	for (unsigned round = 0; round < 200; round++) {
		uint8_t command = 0x10 + round % 3;
		uint64_t cookie = round * 2 + 1;
		assert(hakios_mhi_command_submit(model.ring, cookie, 20, command) == 0);
		assert(hakios_mhi_command_submit(model.ring, cookie, 21, command) == EEXIST);
		assert(hakios_mhi_command_submit(model.ring, cookie + 1, 20, command) == EBUSY);
		assert(hakios_mhi_command_submit(model.ring, cookie + 1, 21, command) == 0);
		unsigned offset = ((round * 2) % 128) * 16;
		assert(hakios_wire_get64(model.device + offset) == 0);
		assert(hakios_wire_get32(model.device + offset + 8) == 0);
		assert(hakios_wire_get32(model.device + offset + 12) == (20u << 24 | (uint32_t)command << 16));
		command_finish(&model, (round * 2) % 128, cookie, 20, command);
		command_finish(&model, (round * 2 + 1) % 128, cookie + 1, 21, command);
	}
	assert(model.published == 400);
	assert(hakios_mhi_command_destroy(&model.ring) == 0 && model.ring == NULL);
	for (unsigned variant = 0; variant < 8; variant++) {
		command_init(&model);
		uint8_t event[16];
		struct hakios_mhi_command_completion completion;
		assert(hakios_mhi_command_submit(model.ring, 1, 20, 0x12) == 0);
		assert(hakios_mhi_command_submit(model.ring, 2, 21, 0x12) == 0);
		event_bytes(event, model.config.iova, 0x21, 0, 1, 0);
		switch (variant) {
		case 0: hakios_wire_put64(event, model.config.iova + 16); break;
		case 1: hakios_wire_put64(event, model.config.iova + 1); break;
		case 2: hakios_wire_put64(event, model.config.iova + 2048); break;
		case 3: hakios_wire_put64(event, 0); break;
		case 4: event[14] = 0x22; break;
		case 5: event[11] = 0x10; break;
		case 6: event[11] = 0; break;
		case 7: command_finish(&model, 0, 1, 20, 0x12); break;
		}
		assert(hakios_mhi_command_complete(model.ring, event, &completion) == (variant == 5 || variant == 6 ? EIO : EPROTO));
		assert(completion.cookie == 0 && completion.channel == 0 && completion.command == 0);
		assert(hakios_mhi_command_status(model.ring) == EIO);
		assert(hakios_mhi_command_submit(model.ring, 3, 20, 0x10) == EIO);
		assert(hakios_mhi_command_complete(model.ring, event, &completion) == EIO);
		assert(hakios_mhi_command_destroy(&model.ring) == 0);
	}
}

static void
test_event_flow(void)
{
	for (unsigned index = 0; index < 2; index++) {
		struct event_model model;
		uint32_t count = 77;
		event_init(&model, index);
		assert(hakios_mhi_event_poll(model.ring, 1, &count) == EAGAIN && count == 0);
		assert(model.fault.calls == 0);
		assert(hakios_mhi_event_start(model.ring) == 0);
		assert(hakios_mhi_event_start(model.ring) == EALREADY);
		assert(hakios_mhi_event_poll(model.ring, 0, &count) == EINVAL && count == 0);
		assert(hakios_mhi_event_poll(model.ring, model.elements, &count) == EINVAL);
		assert(hakios_mhi_event_poll(model.ring, 1, NULL) == EINVAL);
		assert(hakios_mhi_event_poll(NULL, 1, &count) == EINVAL);
		for (unsigned round = 0; round < 4; round++) {
			while (model.credits != 0) {
				push_sequence(&model);
			}
			uint8_t unused[16] = {0};
			assert(!device_push(&model, unused));
			assert(hakios_mhi_event_poll(model.ring, 1, &count) == 0 && count == 1);
			assert(model.credits == 1);
			push_sequence(&model);
			while (model.accepted != model.produced) {
				assert(hakios_mhi_event_poll(model.ring, 7, &count) == 0 && count > 0 && count <= 7);
			}
			assert(model.credits == model.elements - 1);
			assert(hakios_mhi_event_poll(model.ring, 1, &count) == 0 && count == 0);
		}
		assert(model.accepted == model.elements * 4);
		for (unsigned serial = 0; serial < model.accepted; serial++) {
			assert(model.delivered[serial] == serial + 1);
		}
		assert(hakios_mhi_event_destroy(&model.ring) == 0 && model.ring == NULL);
	}
}

static void
test_backpressure(void)
{
	for (unsigned variant = 0; variant < 2; variant++) {
		struct event_model model;
		event_init(&model, 0);
		assert(hakios_mhi_event_start(model.ring) == 0);
		push_sequence(&model);
		push_sequence(&model);
		uint32_t count;
		assert(hakios_mhi_event_poll(model.ring, 1, &count) == 0 && count == 1);
		uint64_t saved_wp = model.wp;
		model.stall_error = variant == 0 ? EAGAIN : ENOSPC;
		model.stalls = 2;
		for (unsigned attempt = 0; attempt < 2; attempt++) {
			assert(hakios_mhi_event_poll(model.ring, 1, &count) == model.stall_error && count == 0);
			assert(model.wp == saved_wp && model.accepted == 1 && model.returned == 1);
			assert(hakios_mhi_event_status(model.ring) == 0);
		}
		push_sequence(&model);
		assert(hakios_mhi_event_poll(model.ring, 31, &count) == 0 && count == 2);
		assert(model.accepted == 3 && model.delivered[1] == 2 && model.delivered[2] == 3);
		assert(hakios_mhi_event_destroy(&model.ring) == 0);
	}
}

static void
test_partial_backpressure(void)
{
	struct event_model model;
	event_init(&model, 0);
	assert(hakios_mhi_event_start(model.ring) == 0);
	push_sequence(&model);
	push_sequence(&model);
	model.stalls = 1;
	model.stall_after = 1;
	model.stall_error = ENOSPC;
	uint32_t count;
	assert(hakios_mhi_event_poll(model.ring, 31, &count) == ENOSPC && count == 1);
	assert(model.accepted == 1 && model.published == 1 && model.credits == 30);
	assert(hakios_mhi_event_poll(model.ring, 31, &count) == 0 && count == 1);
	assert(model.accepted == 2 && model.published == 2 && model.credits == 31);
	assert(hakios_mhi_event_destroy(&model.ring) == 0);
	for (unsigned variant = 0; variant < 2; variant++) {
		event_init(&model, 0);
		assert(hakios_mhi_event_start(model.ring) == 0);
		push_sequence(&model);
		model.fault.fail_at = model.fault.calls + 1 + variant;
		model.fault.error = variant == 0 ? EAGAIN : ENOSPC;
		assert(hakios_mhi_event_poll(model.ring, 31, &count) == model.fault.error && count == 0);
		assert(hakios_mhi_event_status(model.ring) == EIO && model.accepted == 0 && model.published == 0);
		model.fault.fail_at = 0;
		assert(hakios_mhi_event_destroy(&model.ring) == 0);
		injected_failures++;
	}
}

static void
test_bad_events(void)
{
	for (unsigned variant = 0; variant < 10; variant++) {
		struct event_model model;
		event_init(&model, variant >= 8 ? 1 : 0);
		assert(hakios_mhi_event_start(model.ring) == 0);
		uint8_t event[16];
		event_bytes(event, 0, 0x20, 0, 2, 0);
		uint32_t count;
		int expected = EPROTO;
		switch (variant) {
		case 0: model.device_rp = model.config.iova - 16; break;
		case 1: model.device_rp = model.config.iova + 512; break;
		case 2: model.device_rp = model.config.iova + 1; break;
		case 3: model.device_rp = UINT64_MAX; break;
		case 4:
		case 5:
			while (model.credits != 0) {
				push_sequence(&model);
			}
			model.stalls = 1;
			model.stall_error = EAGAIN;
			assert(hakios_mhi_event_poll(model.ring, 1, &count) == EAGAIN && count == 0);
			model.device_rp = model.config.iova + (variant == 4 ? 0 : 30 * 16);
			break;
		case 6: event[14] = 0xff; expected = ENOTSUP; assert(device_push(&model, event)); break;
		case 7: event[14] = 0x22; expected = ENOTSUP; assert(device_push(&model, event)); break;
		case 8: expected = ENOTSUP; assert(device_push(&model, event)); break;
		case 9: event[14] = 0x22; event[15] = 19; assert(device_push(&model, event)); break;
		}
		assert(hakios_mhi_event_poll(model.ring, 1, &count) == expected && count == 0);
		assert(hakios_mhi_event_status(model.ring) == EIO && model.published == 0);
		unsigned calls = model.fault.calls;
		assert(hakios_mhi_event_poll(model.ring, 1, &count) == EIO && count == 0);
		assert(hakios_mhi_event_start(model.ring) == EIO && model.fault.calls == calls);
		assert(hakios_mhi_event_destroy(&model.ring) == 0);
	}
	struct event_model model;
	event_init(&model, 0);
	model.device_rp += 16;
	assert(hakios_mhi_event_start(model.ring) == EPROTO && !model.ready);
	assert(hakios_mhi_event_destroy(&model.ring) == 0);
}

static void
test_bounded_snapshot(void)
{
	struct event_model model;
	event_init(&model, 0);
	assert(hakios_mhi_event_start(model.ring) == 0);
	push_sequence(&model);
	push_sequence(&model);
	model.refill = true;
	uint32_t count;
	for (unsigned round = 0; round < 100; round++) {
		assert(hakios_mhi_event_poll(model.ring, 31, &count) == 0 && count == 2);
		assert(model.produced == model.accepted + 2);
	}
	model.refill = false;
	assert(hakios_mhi_event_poll(model.ring, 31, &count) == 0 && count == 2);
	assert(hakios_mhi_event_destroy(&model.ring) == 0);
}

static void
test_injected_failures(void)
{
	for (unsigned variant = 0; variant < 4; variant++) {
		int injected = variant & 1 ? -ETIMEDOUT : ETIMEDOUT;
		int expected = injected < 0 ? EIO : injected;
		for (unsigned fail_at = 1; fail_at <= 3; fail_at++) {
			struct command_model model;
			command_init(&model);
			model.fault = (struct fault) {.fail_at = fail_at, .error = injected, .after = (variant & 2) != 0};
			int error = hakios_mhi_command_submit(model.ring, 1, 20, 0x12);
			if (error == 0) {
				command_finish(&model, 0, 1, 20, 0x12);
				error = hakios_mhi_command_destroy(&model.ring);
			}
			assert(error == expected && model.ring != NULL);
			assert(hakios_mhi_command_status(model.ring) == EIO);
			unsigned calls = model.fault.calls;
			assert(hakios_mhi_command_submit(model.ring, 2, 21, 0x12) == EIO);
			assert(model.fault.calls == calls);
			model.fault.fail_at = 0;
			assert(hakios_mhi_command_destroy(&model.ring) == 0 && model.stopped);
			injected_failures++;
		}
		for (unsigned fail_at = 1; fail_at <= 13; fail_at++) {
			struct event_model model;
			event_init(&model, 0);
			model.fault = (struct fault) {.fail_at = fail_at, .error = injected, .after = (variant & 2) != 0};
			uint32_t count = 0;
			int error = hakios_mhi_event_start(model.ring);
			if (error == 0) {
				push_sequence(&model);
				push_sequence(&model);
				error = hakios_mhi_event_poll(model.ring, 31, &count);
			}
			if (error == 0) {
				error = hakios_mhi_event_destroy(&model.ring);
			}
			assert(error == expected && model.ring != NULL);
			assert(count == (fail_at <= 6 ? 0u : fail_at <= 10 ? 1u : 2u));
			assert(hakios_mhi_event_status(model.ring) == EIO);
			unsigned calls = model.fault.calls, accepted = model.accepted, published = model.published;
			assert(hakios_mhi_event_poll(model.ring, 31, &count) == EIO && count == 0);
			assert(hakios_mhi_event_start(model.ring) == EIO);
			assert(model.fault.calls == calls && model.accepted == accepted && model.published == published);
			model.fault.fail_at = model.fault.calls + 1;
			assert(hakios_mhi_event_destroy(&model.ring) == expected && model.ring != NULL);
			model.fault.fail_at = 0;
			assert(hakios_mhi_event_destroy(&model.ring) == 0 && model.stopped);
			injected_failures += 2;
		}
	}
}

struct transfer_model {
	struct hakios_mhi_ring *ring;
	_Alignas(16) uint8_t cpu[1024];
	uint8_t device[1024];
	unsigned owned;
	unsigned returned;
	unsigned published;
	bool stopped;
};

static int
transfer_buffer(void *context, uint64_t cookie, bool for_device, uint32_t length)
{
	struct transfer_model *model = context;
	assert(length == 128 && cookie >= 1 && cookie <= 2);
	if (for_device) {
		model->owned++;
	} else {
		model->returned++;
	}
	assert(model->returned <= model->owned);
	return 0;
}

static int
transfer_sync(void *context, uint32_t offset, uint32_t length)
{
	struct transfer_model *model = context;
	assert(length == 16 && offset < 1024);
	memcpy(model->device + offset, model->cpu + offset, length);
	return 0;
}

static int
transfer_publish(void *context, uint8_t channel, uint64_t next_wp)
{
	struct transfer_model *model = context;
	assert(channel == 21);
	model->published++;
	assert(next_wp == 0x100008000ULL + model->published * 16);
	assert(hakios_wire_get32(model->device + (model->published - 1) * 16 + 8) == 128);
	return 0;
}

static int
transfer_quiesce(void *context)
{
	struct transfer_model *model = context;
	model->stopped = true;
	return 0;
}

static void
test_dispatch_pipeline(void)
{
	struct command_model commands;
	struct event_model control, data;
	command_init(&commands);
	event_init(&control, 0);
	event_init(&data, 1);
	control.commands = &commands;
	assert(hakios_mhi_event_start(control.ring) == 0);
	assert(hakios_mhi_event_start(data.ring) == 0);
	assert(hakios_mhi_command_submit(commands.ring, 7, 20, 0x12) == 0);
	assert(hakios_mhi_command_submit(commands.ring, 8, 21, 0x12) == 0);
	uint8_t event[16];
	event_bytes(event, 0, 0x20, 0, 2, 0);
	assert(device_push(&control, event));
	event_bytes(event, 0, 0x40, 0, 2, 0);
	assert(device_push(&control, event));
	event_bytes(event, commands.config.iova, 0x21, 0, 1, 0);
	assert(device_push(&control, event));
	uint32_t count;
	assert(hakios_mhi_event_poll(control.ring, 31, &count) == 0 && count == 3);
	assert(control.command_result.cookie == 7 && control.command_result.channel == 20);
	event_bytes(event, commands.config.iova + 16, 0x21, 0, 1, 0);
	assert(device_push(&control, event));
	assert(hakios_mhi_event_poll(control.ring, 31, &count) == 0 && count == 1);
	assert(control.command_result.cookie == 8 && control.command_result.channel == 21);
	struct transfer_model transfers = {0};
	struct hakios_mhi_ring_config config = {.elements = transfers.cpu, .iova = 0x100008000ULL, .channel = 21};
	struct hakios_mhi_ring_operations operations = {
		.version = HAKIOS_MHI_VERSION, .sync_buffer = transfer_buffer, .sync_ring = transfer_sync,
		.publish = transfer_publish, .quiesce = transfer_quiesce,
	};
	assert(hakios_mhi_ring_create(&config, &operations, &transfers, &transfers.ring) == 0);
	data.transfer = transfers.ring;
	assert(hakios_mhi_ring_queue(transfers.ring, 1, 0x100010000ULL, 128) == 0);
	assert(hakios_mhi_ring_queue(transfers.ring, 2, 0x100011000ULL, 128) == 0);
	event_bytes(event, config.iova + 16, 0x22, 21, 2, 42);
	assert(device_push(&data, event));
	data.transfer_capacity = 1;
	assert(hakios_mhi_event_poll(data.ring, 255, &count) == ENOSPC && count == 0);
	assert(data.published == 0 && data.transfer_count == 0 && transfers.returned == 0);
	data.transfer_capacity = 63;
	assert(hakios_mhi_event_poll(data.ring, 255, &count) == 0 && count == 1);
	assert(data.transfer_count == 2 && transfers.returned == 2);
	assert(data.transfers[0].cookie == 1 && data.transfers[0].length == 128);
	assert(data.transfers[1].cookie == 2 && data.transfers[1].length == 42);
	assert(device_push(&data, event));
	assert(hakios_mhi_event_poll(data.ring, 255, &count) == EPROTO && count == 0);
	assert(hakios_mhi_ring_status(transfers.ring) == EIO && hakios_mhi_event_status(data.ring) == EIO);
	assert(data.published == 1 && transfers.returned == 2);
	assert(hakios_mhi_event_destroy(&data.ring) == 0);
	assert(hakios_mhi_ring_destroy(&transfers.ring) == 0 && transfers.stopped);
	event_bytes(event, commands.config.iova + 16, 0x21, 0, 1, 0);
	assert(device_push(&control, event));
	assert(hakios_mhi_event_poll(control.ring, 31, &count) == EPROTO && count == 0);
	assert(hakios_mhi_command_status(commands.ring) == EIO && hakios_mhi_event_status(control.ring) == EIO);
	assert(hakios_mhi_event_destroy(&control.ring) == 0);
	assert(hakios_mhi_command_destroy(&commands.ring) == 0);
}

struct command_thread {
	struct command_model *model;
	uint8_t channel;
	uint64_t cookie;
	int error;
};

static void *
command_submit_thread(void *context)
{
	struct command_thread *thread = context;
	thread->error = hakios_mhi_command_submit(thread->model->ring, thread->cookie, thread->channel, 0x12);
	return NULL;
}

static void *
event_poll_thread(void *context)
{
	struct event_model *model = context;
	uint32_t count;
	do {
		assert(hakios_mhi_event_poll(model->ring, 1, &count) == 0);
	} while (count != 0);
	return NULL;
}

static void
test_concurrency(void)
{
	struct command_model commands;
	command_init(&commands);
	pthread_t threads[4];
	struct command_thread requests[4];
	for (unsigned index = 0; index < 4; index++) {
		requests[index] = (struct command_thread) {.model = &commands, .channel = 20 + index % 2, .cookie = index + 1};
		assert(pthread_create(&threads[index], NULL, command_submit_thread, &requests[index]) == 0);
	}
	unsigned accepted = 0;
	for (unsigned index = 0; index < 4; index++) {
		assert(pthread_join(threads[index], NULL) == 0);
		assert(requests[index].error == 0 || requests[index].error == EBUSY);
		accepted += requests[index].error == 0;
	}
	assert(accepted == 2 && commands.published == 2);
	for (unsigned slot = 0; slot < 2; slot++) {
		uint8_t channel = commands.device[slot * 16 + 15];
		for (unsigned index = 0; index < 4; index++) {
			if (requests[index].error == 0 && requests[index].channel == channel) {
				command_finish(&commands, slot, requests[index].cookie, channel, 0x12);
			}
		}
	}
	assert(hakios_mhi_command_destroy(&commands.ring) == 0);
	struct event_model events;
	event_init(&events, 1);
	assert(hakios_mhi_event_start(events.ring) == 0);
	while (events.credits != 0) {
		push_sequence(&events);
	}
	for (unsigned index = 0; index < 4; index++) {
		assert(pthread_create(&threads[index], NULL, event_poll_thread, &events) == 0);
	}
	for (unsigned index = 0; index < 4; index++) {
		assert(pthread_join(threads[index], NULL) == 0);
	}
	assert(events.accepted == 255 && events.published == 255 && events.credits == 255);
	for (unsigned index = 0; index < events.accepted; index++) {
		assert(events.delivered[index] == index + 1);
	}
	assert(hakios_mhi_event_destroy(&events.ring) == 0);
}

int
main(void)
{
	test_contexts();
	test_constructors();
	test_commands();
	test_event_flow();
	test_backpressure();
	test_partial_backpressure();
	test_bad_events();
	test_bounded_snapshot();
	test_injected_failures();
	test_dispatch_pipeline();
	test_concurrency();
	printf("MHI control tests: PASS (%u injected failures)\n", injected_failures);
	return 0;
}

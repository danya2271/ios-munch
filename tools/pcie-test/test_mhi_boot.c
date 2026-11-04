#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/hakios.mhi.boot.h>
#include <sys/hakios.mhi.runtime.h>
#include <sys/hakios.wire.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;

struct boot_model {
	struct hakios_mhi_boot *boot;
	struct hakios_mhi_boot_config config;
	struct hakios_mhi_boot_buffer segments[32];
	uint8_t *device[32];
	uint8_t *image;
	uint8_t vectors[512];
	uint8_t device_vectors[512];
	uint32_t registers[256];
	uint32_t writes[256];
	unsigned write_count;
	unsigned sync_count;
	unsigned context_count;
	unsigned calls;
	unsigned fail_at;
	int failure;
	uint32_t ignored_write;
	bool after;
	bool active;
	bool stopped;
	bool ready;
};

static int
boot_callback(struct boot_model *model)
{
	model->calls++;
	return model->calls == model->fail_at ? model->failure : 0;
}

static int
model_activate(void *owner)
{
	struct boot_model *model = owner;
	int error = boot_callback(model);
	if (error == 0 || model->after) {
		model->active = true;
	}
	return error;
}

static int
model_read(void *owner, uint32_t offset, uint32_t *value)
{
	struct boot_model *model = owner;
	assert(model->active && offset < 1024 && offset % 4 == 0);
	int error = boot_callback(model);
	if (error == 0 || model->after) {
		*value = model->registers[offset / 4];
	}
	return error;
}

static uint64_t
register_address(struct boot_model *model, uint32_t offset)
{
	return (uint64_t)model->registers[(offset + 4) / 4] << 32 | model->registers[offset / 4];
}

static int
model_write(void *owner, uint32_t offset, uint32_t value)
{
	struct boot_model *model = owner;
	assert(model->active && offset < 1024 && offset % 4 == 0 && model->write_count < 256);
	if (offset == 0x118) {
		assert(model->sync_count == model->config.segment_count + 1);
		assert(register_address(model, 0x108) == model->segments[0].iova);
		assert(model->registers[0x110 / 4] == (model->config.image_size < 524288 ? model->config.image_size : 524288));
		assert(value == model->config.sequence);
	}
	if (offset == 0x23c) {
		assert(model->ready && model->context_count == 3);
		assert(register_address(model, 0x22c) == model->config.vectors.iova);
		assert(model->registers[0x234 / 4] == model->config.segment_count * 16);
		assert((value & 0x3fffffff) == model->config.sequence + 1);
		for (unsigned index = 0; index < model->config.segment_count; index++) {
			assert(hakios_wire_get64(model->device_vectors + index * 16) == model->segments[index].iova);
			uint32_t offset_bytes = index * 524288;
			uint32_t size = model->config.image_size - offset_bytes;
			if (size > 524288) {
				size = 524288;
			}
			assert(hakios_wire_get64(model->device_vectors + index * 16 + 8) == size);
			assert(memcmp(model->image + offset_bytes, model->device[index], size) == 0);
		}
	}
	int error = boot_callback(model);
	if ((error == 0 || model->after) && offset != model->ignored_write) {
		model->registers[offset / 4] = value;
		model->writes[model->write_count++] = offset;
	}
	return error;
}

static int
model_sync(void *owner, uint64_t iova, uint32_t length)
{
	struct boot_model *model = owner;
	assert(model->active);
	int error = boot_callback(model);
	if (error != 0 && !model->after) {
		return error;
	}
	for (unsigned index = 0; index < model->config.segment_count; index++) {
		if (iova == model->segments[index].iova) {
			assert(length == model->segments[index].capacity);
			memcpy(model->device[index], model->segments[index].bytes, length);
			model->sync_count++;
			return error;
		}
	}
	if (iova == model->config.vectors.iova) {
		assert(length == model->config.segment_count * 16);
		memcpy(model->device_vectors, model->vectors, length);
		model->sync_count++;
	} else {
		assert((iova == model->config.channel_context && length == 128 * 44) ||
		    (iova == model->config.event_context && length == 88) ||
		    (iova == model->config.command_context && length == 44));
		model->context_count++;
	}
	return error;
}

static int
model_ready(void *owner)
{
	struct boot_model *model = owner;
	assert(model->context_count == 3);
	assert(register_address(model, 0x58) == model->config.channel_context);
	assert(register_address(model, 0x60) == model->config.event_context);
	assert(register_address(model, 0x68) == model->config.command_context);
	assert(register_address(model, 0x80) == model->config.aperture_start);
	assert(register_address(model, 0x88) == model->config.aperture_end);
	assert(register_address(model, 0x98) == model->config.aperture_start);
	assert(register_address(model, 0xa0) == model->config.aperture_end);
	assert(model->registers[0x10 / 4] == 0x20080);
	int error = boot_callback(model);
	if (error == 0 || model->after) {
		model->ready = true;
	}
	return error;
}

static int
model_quiesce(void *owner)
{
	struct boot_model *model = owner;
	int error = boot_callback(model);
	if (error == 0 || model->after) {
		model->stopped = true;
	}
	return error;
}

static const struct hakios_mhi_boot_operations operations = {
	.version = 1, .activate = model_activate, .read = model_read, .write = model_write,
	.sync = model_sync, .ready = model_ready, .quiesce = model_quiesce,
};

static void
model_init(struct boot_model *model, uint32_t image_size)
{
	memset(model, 0, sizeof(*model));
	model->ignored_write = UINT32_MAX;
	model->image = malloc(image_size);
	assert(model->image != NULL);
	for (unsigned index = 0; index < image_size; index++) {
		model->image[index] = (uint8_t)(index * 17 + index / 256);
	}
	model->config = (struct hakios_mhi_boot_config) {
		.image = model->image, .image_size = image_size, .segments = model->segments,
		.segment_count = (image_size + 524287) / 524288,
		.vectors = {.bytes = model->vectors, .iova = 0x100002000ULL, .capacity = 512},
		.channel_context = 0x100004000ULL, .event_context = 0x100006000ULL, .command_context = 0x100007000ULL,
		.aperture_start = 0x100000000ULL, .aperture_end = 0x1ffffffffULL, .sequence = 17, .timeout_ms = 100,
	};
	for (unsigned index = 0; index < model->config.segment_count; index++) {
		uint32_t size = image_size - index * 524288;
		if (size > 524288) {
			size = 524288;
		}
		model->segments[index] = (struct hakios_mhi_boot_buffer) {
			.bytes = calloc(1, size), .iova = 0x100100000ULL + index * 524288, .capacity = size,
		};
		model->device[index] = calloc(1, size);
		assert(model->segments[index].bytes != NULL && model->device[index] != NULL);
	}
	model->registers[0x28 / 4] = 0x100;
	model->registers[0x2c / 4] = 0x200;
	model->registers[0x10 / 4] = 128;
}

static void
model_free(struct boot_model *model)
{
	assert(model->boot == NULL);
	for (unsigned index = 0; index < model->config.segment_count; index++) {
		free(model->segments[index].bytes);
		free(model->device[index]);
	}
	free(model->image);
}

static void
model_create(struct boot_model *model)
{
	assert(hakios_mhi_boot_create(&model->config, &operations, model, &model->boot) == 0);
	assert(model->calls == 0);
}

static int
model_step(struct boot_model *model, unsigned step)
{
	switch (step) {
	case 1: model->registers[0x12c / 4] = 2u << 30; break;
	case 2: model->registers[0x48 / 4] = 0x101; break;
	case 3: model->registers[0x128 / 4] = 1; model->registers[0x48 / 4] = 0x200; break;
	case 4: model->registers[0x244 / 4] = 2u << 30 | 18; break;
	case 5: model->registers[0x128 / 4] = 2; break;
	default: assert(false);
	}
	return hakios_mhi_boot_step(model->boot, 10 + step);
}

static unsigned
test_success(uint32_t image_size)
{
	struct boot_model model;
	model_init(&model, image_size);
	model_create(&model);
	enum hakios_mhi_boot_phase phase;
	assert(hakios_mhi_boot_status(model.boot, &phase) == EAGAIN && phase == HAKIOS_MHI_BOOT_NEW);
	assert(hakios_mhi_boot_step(model.boot, 0) == EAGAIN);
	assert(hakios_mhi_boot_start(model.boot, 10) == 0);
	assert(hakios_mhi_boot_start(model.boot, 10) == EALREADY);
	for (unsigned step = 1; step <= 5; step++) {
		assert(model_step(&model, step) == (step == 5 ? 0 : EAGAIN));
	}
	assert(hakios_mhi_boot_status(model.boot, &phase) == 0 && phase == HAKIOS_MHI_BOOT_ONLINE);
	for (unsigned index = 1; index < model.write_count; index++) {
		uint32_t offset = model.writes[index];
		if (offset == 0x108 || offset == 0x22c || offset == 0x58 || offset == 0x60 || offset == 0x68 ||
		    offset == 0x80 || offset == 0x88 || offset == 0x98 || offset == 0xa0) {
			assert(model.writes[index - 1] == offset + 4);
		}
	}
	assert(hakios_mhi_boot_destroy(&model.boot) == 0 && model.stopped);
	unsigned calls = model.calls;
	model_free(&model);
	return calls;
}

static unsigned
test_failures(unsigned callbacks)
{
	unsigned failures = 0;
	for (unsigned variant = 0; variant < 4; variant++) {
		for (unsigned fail_at = 1; fail_at <= callbacks; fail_at++) {
			struct boot_model model;
			model_init(&model, 524325);
			model_create(&model);
			model.fail_at = fail_at;
			model.failure = variant & 1 ? -ETIMEDOUT : ETIMEDOUT;
			model.after = (variant & 2) != 0;
			int expected = model.failure < 0 ? EIO : model.failure;
			int error = hakios_mhi_boot_start(model.boot, 10);
			for (unsigned step = 1; step <= 5 && (error == 0 || error == EAGAIN); step++) {
				error = model_step(&model, step);
			}
			if (error == 0) {
				error = hakios_mhi_boot_destroy(&model.boot);
			}
			assert(error == expected && model.boot != NULL);
			enum hakios_mhi_boot_phase phase;
			assert(hakios_mhi_boot_status(model.boot, &phase) == EIO && phase == HAKIOS_MHI_BOOT_FAULT);
			unsigned calls = model.calls;
			assert(hakios_mhi_boot_step(model.boot, 20) == EIO);
			assert(hakios_mhi_boot_start(model.boot, 20) == EIO && model.calls == calls);
			model.fail_at = model.calls + 1;
			assert(hakios_mhi_boot_destroy(&model.boot) == expected && model.boot != NULL);
			model.fail_at = 0;
			assert(hakios_mhi_boot_destroy(&model.boot) == 0);
			model_free(&model);
			failures += 2;
		}
	}
	return failures;
}

static void
test_validation(void)
{
	struct boot_model model;
	model_init(&model, 524325);
	for (unsigned variant = 0; variant < 12; variant++) {
		struct hakios_mhi_boot_config config = model.config;
		switch (variant) {
		case 0: config.segment_count++; break;
		case 1: config.image_size = 0; break;
		case 2: config.sequence = 0; break;
		case 3: config.sequence = 0x3fffffff; break;
		case 4: config.channel_context = config.event_context; break;
		case 5: config.channel_context = model.segments[0].iova; break;
		case 6: config.vectors.iova = model.segments[1].iova; break;
		case 7: config.vectors.bytes = model.segments[0].bytes; break;
		case 8: config.vectors.capacity = 16; break;
		case 9: config.image = model.segments[0].bytes; break;
		case 10: config.aperture_end = config.aperture_start + 1; break;
		case 11: config.timeout_ms = 0; break;
		}
		assert(hakios_mhi_boot_create(&config, &operations, &model, &model.boot) == EINVAL && model.boot == NULL);
		assert(model.segments[0].bytes[1] == 0 && model.calls == 0);
	}
	pcie_test_fail_allocation = true;
	assert(hakios_mhi_boot_create(&model.config, &operations, &model, &model.boot) == ENOMEM);
	pcie_test_fail_allocation = false;
	model_free(&model);
}

static void
test_state_errors(void)
{
	for (unsigned variant = 0; variant < 14; variant++) {
		struct boot_model model;
		model_init(&model, 524325);
		model_create(&model);
		int error, expected = EIO;
		if (variant < 4) {
			switch (variant) {
			case 0: model.registers[0x28 / 4] = 0x200; expected = EPROTO; break;
			case 1: model.registers[0x28 / 4] = 0x100000; expected = EPROTO; break;
			case 2: model.registers[0x128 / 4] = 2; expected = EBUSY; break;
			case 3: model.ignored_write = 0x108; break;
			}
			error = hakios_mhi_boot_start(model.boot, 10);
		} else {
			assert(hakios_mhi_boot_start(model.boot, 10) == 0);
			if (variant == 4) {
				error = hakios_mhi_boot_step(model.boot, 110);
				expected = ETIMEDOUT;
			} else if (variant == 5) {
				model.registers[0x128 / 4] = 3;
				error = hakios_mhi_boot_step(model.boot, 11);
			} else if (variant == 6) {
				model.registers[0x48 / 4] = UINT32_MAX;
				error = hakios_mhi_boot_step(model.boot, 11);
			} else if (variant == 7) {
				error = hakios_mhi_boot_step(model.boot, 9);
				expected = EINVAL;
			} else if (variant == 8) {
				model.ignored_write = 0x58;
				assert(model_step(&model, 1) == EAGAIN);
				error = model_step(&model, 2);
			} else if (variant == 9) {
				model.registers[0x12c / 4] = 3u << 30;
				error = hakios_mhi_boot_step(model.boot, 11);
			} else if (variant == 13) {
				assert(model_step(&model, 1) == EAGAIN);
				model.registers[0x128 / 4] = 2;
				error = model_step(&model, 2);
				expected = EPROTO;
				assert(model.context_count == 0 && !model.ready);
			} else {
				for (unsigned step = 1; step <= 3; step++) {
					assert(model_step(&model, step) == EAGAIN);
				}
				if (variant == 10) {
					model.registers[0x244 / 4] = 2u << 30 | 17;
					assert(hakios_mhi_boot_step(model.boot, 14) == EAGAIN);
					error = hakios_mhi_boot_step(model.boot, 113);
					expected = ETIMEDOUT;
				} else if (variant == 11) {
					model.registers[0x244 / 4] = 3u << 30 | 18;
					error = hakios_mhi_boot_step(model.boot, 14);
				} else {
					assert(model_step(&model, 4) == EAGAIN && model_step(&model, 5) == 0);
					model.registers[0x48 / 4] = 0x500;
					error = hakios_mhi_boot_step(model.boot, 16);
					expected = ENOTSUP;
				}
			}
		}
		assert(error == expected);
		assert(hakios_mhi_boot_destroy(&model.boot) == 0);
		model_free(&model);
	}
}

struct runtime_model {
	struct boot_model boot;
	struct hakios_mhi_runtime *runtime;
	uint32_t state;
	uint32_t environment;
	uint32_t requested;
	unsigned calls;
	unsigned fail_at;
	unsigned faults;
	int failure;
	bool after;
	bool wake;
	bool gated;
	bool sleeping;
	bool stopped;
	bool busy;
};

static int
runtime_call(struct runtime_model *model)
{
	model->calls++;
	return model->calls == model->fail_at ? model->failure : 0;
}

static int
runtime_model_read(void *owner, uint32_t *state, uint32_t *environment)
{
	struct runtime_model *model = owner;
	assert(!model->sleeping && !model->stopped);
	int error = runtime_call(model);
	if (error == 0 || model->after) {
		*state = model->state;
		*environment = model->environment;
	}
	return error;
}

static int
runtime_model_request(void *owner, uint32_t state)
{
	struct runtime_model *model = owner;
	assert(!model->sleeping && model->gated && (state == 2 || state == 5));
	int error = runtime_call(model);
	if (error == 0 || model->after) {
		model->requested = state;
	}
	return error;
}

static int
runtime_model_wake(void *owner, bool asserted)
{
	struct runtime_model *model = owner;
	assert(!model->sleeping);
	if (!asserted) {
		assert(model->requested == 5 && model->gated);
	}
	int error = runtime_call(model);
	if (error == 0 || model->after) {
		model->wake = asserted;
	}
	return error;
}

static int
runtime_model_drain(void *owner)
{
	struct runtime_model *model = owner;
	assert(!model->sleeping && model->wake);
	int error = runtime_call(model);
	if (model->busy) {
		return EBUSY;
	}
	if (error == 0 || model->after) {
		model->gated = true;
	}
	return error;
}

static int
runtime_model_rearm(void *owner)
{
	struct runtime_model *model = owner;
	assert(model->state == 2 && !model->sleeping && model->wake);
	int error = runtime_call(model);
	if (error == 0 || model->after) {
		model->gated = false;
	}
	return error;
}

static int
runtime_model_suspend(void *owner)
{
	struct runtime_model *model = owner;
	assert(model->state == 5 && model->gated && !model->wake);
	int error = runtime_call(model);
	if (error == 0 || model->after) {
		model->sleeping = true;
	}
	return error;
}

static int
runtime_model_resume(void *owner)
{
	struct runtime_model *model = owner;
	assert(model->sleeping && model->gated);
	int error = runtime_call(model);
	if (error == 0 || model->after) {
		model->sleeping = false;
	}
	return error;
}

static void
runtime_model_fault(void *owner, int error)
{
	struct runtime_model *model = owner;
	assert(error != 0 && model->faults == 0);
	assert(hakios_mhi_runtime_get(model->runtime) == EBUSY);
	model->faults++;
	model->gated = true;
}

static int
runtime_model_quiesce(void *owner)
{
	struct runtime_model *model = owner;
	int error = runtime_call(model);
	if (error == 0 || model->after) {
		model->stopped = true;
		model->gated = true;
	}
	return error;
}

static const struct hakios_mhi_runtime_operations runtime_operations = {
	.version = HAKIOS_MHI_RUNTIME_VERSION, .read_state = runtime_model_read, .request_state = runtime_model_request,
	.wake = runtime_model_wake, .drain = runtime_model_drain, .rearm = runtime_model_rearm,
	.link_suspend = runtime_model_suspend, .link_resume = runtime_model_resume,
	.fault = runtime_model_fault, .quiesce = runtime_model_quiesce,
};

static void
runtime_model_init(struct runtime_model *model)
{
	memset(model, 0, sizeof(*model));
	model_init(&model->boot, 1024);
	model_create(&model->boot);
	assert(hakios_mhi_runtime_create(model->boot.boot, 100, &runtime_operations, model, &model->runtime) == EBUSY);
	assert(hakios_mhi_boot_start(model->boot.boot, 10) == 0);
	for (unsigned step = 1; step <= 5; step++) {
		assert(model_step(&model->boot, step) == (step == 5 ? 0 : EAGAIN));
	}
	model->state = 2;
	model->environment = 2;
	assert(hakios_mhi_runtime_create(model->boot.boot, 100, &runtime_operations, model, &model->runtime) == 0);
	assert(hakios_mhi_boot_destroy(&model->boot.boot) == EBUSY);
	unsigned calls = model->boot.calls;
	assert(hakios_mhi_boot_step(model->boot.boot, 100) == EALREADY && calls == model->boot.calls);
	assert(hakios_mhi_runtime_destroy(&model->runtime) == EBUSY);
}

static void
runtime_model_finish(struct runtime_model *model)
{
	model->fail_at = 0;
	assert(hakios_mhi_runtime_retire(model->runtime) == 0 && model->stopped);
	assert(hakios_mhi_runtime_destroy(&model->runtime) == 0 && model->runtime == NULL);
	assert(hakios_mhi_boot_destroy(&model->boot.boot) == 0);
	model_free(&model->boot);
}

static int
runtime_cycle(struct runtime_model *model)
{
	int error = hakios_mhi_runtime_start(model->runtime, 20);
	if (error != 0) {
		return error;
	}
	error = hakios_mhi_runtime_suspend(model->runtime, 30);
	if (error != EAGAIN) {
		return error;
	}
	assert(hakios_mhi_runtime_get(model->runtime) == EAGAIN);
	model->state = 5;
	error = hakios_mhi_runtime_step(model->runtime, 40);
	if (error != 0) {
		return error;
	}
	unsigned calls = model->calls;
	assert(hakios_mhi_runtime_step(model->runtime, 41) == 0 && calls == model->calls);
	error = hakios_mhi_runtime_resume(model->runtime, 50);
	if (error != EAGAIN) {
		return error;
	}
	model->state = 2;
	error = hakios_mhi_runtime_step(model->runtime, 60);
	return error == 0 ? hakios_mhi_runtime_retire(model->runtime) : error;
}

static void
test_runtime(void)
{
	struct runtime_model model;
	runtime_model_init(&model);
	assert(runtime_cycle(&model) == 0 && model.faults == 0);
	unsigned callbacks = model.calls;
	runtime_model_finish(&model);
	for (unsigned after = 0; after < 2; after++) {
		for (unsigned call = 1; call <= callbacks; call++) {
			runtime_model_init(&model);
			model.fail_at = call;
			model.failure = after != 0 ? ENOSPC : -EIO;
			model.after = after != 0;
			assert(runtime_cycle(&model) == EIO);
			struct hakios_mhi_runtime_status status;
			assert(hakios_mhi_runtime_status(model.runtime, &status) == 0 && status.phase == HAKIOS_MHI_RUNTIME_FAULT);
			assert(model.faults == 1 && model.gated);
			assert(hakios_mhi_runtime_get(model.runtime) == ENOTCONN);
			assert(hakios_mhi_boot_destroy(&model.boot.boot) == EBUSY);
			runtime_model_finish(&model);
		}
	}
	runtime_model_init(&model);
	assert(hakios_mhi_runtime_start(model.runtime, 20) == 0);
	assert(hakios_mhi_runtime_get(model.runtime) == 0);
	unsigned calls = model.calls;
	assert(hakios_mhi_runtime_suspend(model.runtime, 30) == EBUSY && calls == model.calls);
	assert(hakios_mhi_runtime_put(model.runtime) == 0);
	assert(hakios_mhi_runtime_put(model.runtime) == EINVAL);
	model.busy = true;
	assert(hakios_mhi_runtime_suspend(model.runtime, 30) == EBUSY && !model.gated);
	model.busy = false;
	assert(hakios_mhi_runtime_suspend(model.runtime, 30) == EAGAIN && model.gated);
	assert(hakios_mhi_runtime_step(model.runtime, 129) == EAGAIN);
	assert(hakios_mhi_runtime_step(model.runtime, 130) == ETIMEDOUT);
	runtime_model_finish(&model);
	for (unsigned invalid = 0; invalid < 6; invalid++) {
		runtime_model_init(&model);
		assert(hakios_mhi_runtime_start(model.runtime, 20) == 0);
		if (invalid == 0) {
			model.environment = 3;
		} else if (invalid == 1) {
			model.state = 255;
		} else if (invalid == 2 || invalid == 3) {
			model.state = invalid + 1;
		}
		int error = hakios_mhi_runtime_step(model.runtime, invalid == 4 ? 19 : invalid == 5 ? UINT64_MAX : 21);
		assert(error == (invalid < 2 ? EIO : invalid < 4 ? ENOTSUP : EINVAL));
		runtime_model_finish(&model);
	}
	runtime_model_init(&model);
	assert(hakios_mhi_runtime_start(model.runtime, 20) == 0);
	assert(hakios_mhi_runtime_get(model.runtime) == 0);
	assert(hakios_mhi_runtime_retire(model.runtime) == 0);
	assert(hakios_mhi_runtime_destroy(&model.runtime) == EBUSY);
	assert(hakios_mhi_runtime_put(model.runtime) == 0);
	runtime_model_finish(&model);
	printf("mhi-runtime-test: PASS (%u callback failures; M0/M3, no MMIO asleep, boot lease, wake votes, retired recovery)\n",
	    callbacks * 2);
}

int
main(void)
{
	test_validation();
	unsigned callbacks = test_success(524325);
	test_success(4364432);
	test_success(1);
	test_state_errors();
	unsigned failures = test_failures(callbacks);
	test_runtime();
	printf("mhi-boot-test: PASS (%u injected failures; BHI/BHIE, contexts, AMSS/M0, timeout, retained teardown)\n", failures);
	return 0;
}

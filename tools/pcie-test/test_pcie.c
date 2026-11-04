#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/hakios.pcie.h>
#include <sys/hakios.pcie.kona.h>
#include <sys/hakios.pcie.kona.platform.h>

#define REGION_WORDS 7u
#define REGION_ENABLE (1u << 31)

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;
static unsigned fault_cases;

struct model {
	struct hakios_pcie_kona *controller;
	struct hakios_pcie_kona_cold_owner *cold_owner;
	struct hakios_pcie_kona_platform *kona_platform;
	uint32_t region[REGION_WORDS];
	uint32_t original[REGION_WORDS];
	uint8_t config[4096];
	uint32_t bar[256];
	uint16_t saved_command;
	unsigned calls;
	unsigned fault_at;
	unsigned config_reads;
	unsigned config_writes;
	unsigned region_writes;
	unsigned delays;
	unsigned end_calls;
	unsigned drain_calls;
	unsigned bar_calls;
	unsigned bar_fault_at;
	unsigned bar_begin_calls;
	unsigned bar_end_calls;
	unsigned cold_calls;
	unsigned cold_fail_at;
	unsigned cold_release_fail_at;
	unsigned cold_active;
	unsigned cold_revokes;
	unsigned acquire_calls;
	unsigned release_calls;
	int callback_error;
	int acquire_error;
	int release_error;
	int bar_begin_error;
	int bar_end_error;
	bool write_before_error;
	bool guard_active;
	bool config_pending;
	bool begin_partial;
	bool begin_without_guard;
	bool link_down;
	bool stuck_enable;
	bool stuck_disable;
	bool drain_failure;
	bool restore_mismatch;
	bool config_mismatch;
	bool wide_read;
	bool delay_failure;
	bool owner_active;
	bool acquire_without_owner;
	bool bad_sid;
	bool bar_active;
	bool wake_seen;
	bool cold_owned;
	bool cold_partial_fail;
	bool cold_authorize_partial_fail;
};

static const uint32_t config_region[REGION_WORDS] = {
	4, REGION_ENABLE, 0x60100000, 0, 0x60100fff, 0x01000000, 0,
};

static const struct hakios_pcie_kona_resources resources = {
	.dbi = { .address = 0x60000000, .size = 0xf1d },
	.iatu = { .address = 0x60001000, .size = 0x1000 },
	.config = { .address = 0x60100000, .size = 0x100000 },
};

static const struct hakios_pcie_endpoint_request endpoint = {
	.domain = 0, .requester_id = 0x100, .stream_id = 0x1c01,
	.vendor = 0x17cb, .device = 0x1101, .class_code = 0x028000, .revision = 1,
	.bar0_address = 0x60300000, .bar0_size = 0x100000,
};

static int
model_call(struct model *model)
{
	model->calls++;
	sched_yield();
	return model->calls == model->fault_at ? model->callback_error : 0;
}

static uint32_t
config_load(struct model *model, uint32_t offset, uint8_t width)
{
	uint32_t value = 0;
	for (uint8_t byte = 0; byte < width; byte++) {
		value |= (uint32_t)model->config[offset + byte] << (byte * 8);
	}
	return value;
}

static void
config_store(struct model *model, uint32_t offset, uint8_t width, uint32_t value)
{
	for (uint8_t byte = 0; byte < width; byte++) {
		model->config[offset + byte] = (uint8_t)(value >> (byte * 8));
	}
}

static int
model_begin(void *context, void **guard)
{
	struct model *model = context;
	assert(!model->guard_active);
	int error = model_call(model);
	if (model->begin_without_guard || (error != 0 && !model->begin_partial)) {
		*guard = NULL;
		return error;
	}
	model->guard_active = true;
	*guard = model;
	return error;
}

static int
model_end(void *context, void *guard)
{
	struct model *model = context;
	assert(guard == model && model->guard_active && !model->config_pending);
	assert(memcmp(model->region, model->original, sizeof(model->region)) == 0);
	model->end_calls++;
	int error = model_call(model);
	if (error == 0) {
		model->guard_active = false;
	}
	return error;
}

static int
model_read(void *context, void *guard, enum hakios_pcie_kona_space space,
    uint32_t offset, uint8_t width, uint32_t *value)
{
	struct model *model = context;
	assert(guard == model && model->guard_active);
	if (space == HAKIOS_PCIE_KONA_CONFIG) {
		assert(memcmp(model->region, config_region, sizeof(config_region)) == 0);
		assert(offset + width <= sizeof(model->config));
		model->config_reads++;
		model->config_pending = true;
	}
	int error = model_call(model);
	*value = UINT32_MAX;
	if (error != 0) {
		return error;
	}
	if (space == HAKIOS_PCIE_KONA_DBI) {
		assert(width == 4 && offset == 0x80);
		*value = model->link_down ? 0 : 1u << 29;
	} else if (space == HAKIOS_PCIE_KONA_IATU) {
		assert(width == 4 && offset % 4 == 0 && offset / 4 < REGION_WORDS);
		*value = model->region[offset / 4];
		if (offset == 0x14 && ((model->config_mismatch && model->region[0] == 4) ||
		    (model->restore_mismatch && model->drain_calls != 0 && model->region[0] != 4))) {
			*value ^= 0x10000;
		}
	} else {
		assert(space == HAKIOS_PCIE_KONA_CONFIG);
		*value = model->wide_read ? UINT32_MAX : config_load(model, offset, width);
	}
	return 0;
}

static int
model_write(void *context, void *guard, enum hakios_pcie_kona_space space,
    uint32_t offset, uint8_t width, uint32_t value)
{
	struct model *model = context;
	assert(guard == model && model->guard_active);
	if (space == HAKIOS_PCIE_KONA_IATU) {
		assert(!model->config_pending && width == 4);
		assert(offset % 4 == 0 && offset / 4 < REGION_WORDS);
		assert(offset == 4 || (model->region[1] & REGION_ENABLE) == 0);
		model->region_writes++;
	} else {
		assert(space == HAKIOS_PCIE_KONA_CONFIG);
		assert(memcmp(model->region, config_region, sizeof(config_region)) == 0);
		assert(offset + width <= sizeof(model->config));
		model->config_writes++;
		model->config_pending = true;
	}
	int error = model_call(model);
	if (error != 0 && !model->write_before_error) {
		return error;
	}
	if (space == HAKIOS_PCIE_KONA_IATU) {
		if (offset != 4 || !((model->stuck_disable && (value & REGION_ENABLE) == 0) ||
		    (model->stuck_enable && (value & REGION_ENABLE) != 0))) {
			model->region[offset / 4] = value;
		}
	} else {
		for (uint8_t byte = 0; byte < width; byte++) {
			uint32_t address = offset + byte;
			uint8_t octet = (uint8_t)(value >> (byte * 8));
			if (address == 6 || address == 7) {
				model->config[address] &= (uint8_t)~octet;
			} else {
				model->config[address] = octet;
			}
		}
	}
	return error;
}

static int
model_drain(void *context, void *guard)
{
	struct model *model = context;
	assert(guard == model && model->guard_active && model->config_pending);
	model->drain_calls++;
	int error = model_call(model);
	if (error != 0 || model->drain_failure) {
		return error != 0 ? error : ETIMEDOUT;
	}
	model->config_pending = false;
	return 0;
}

static int
model_delay(void *context, void *guard, uint32_t microseconds)
{
	struct model *model = context;
	assert(guard == model && model->guard_active && microseconds == 100);
	model->delays++;
	assert(model->delays <= 198);
	int error = model_call(model);
	return error != 0 ? error : model->delay_failure ? EIO : 0;
}

static const struct hakios_pcie_kona_operations operations = {
	.version = HAKIOS_PCIE_KONA_OPERATIONS_VERSION,
	.begin = model_begin, .end = model_end, .read = model_read, .write = model_write,
	.config_drain = model_drain, .delay_us = model_delay,
};

static void
model_init(struct model *model, bool enabled)
{
	memset(model, 0, sizeof(*model));
	const uint32_t original[REGION_WORDS] = {
		0, enabled ? REGION_ENABLE : 0, 0x60200000, 0, 0x60200fff, 0x1000, 0,
	};
	memcpy(model->region, original, sizeof(original));
	memcpy(model->original, original, sizeof(original));
	model->callback_error = EIO;
	config_store(model, 0, 4, 0x110117cb);
	config_store(model, 6, 2, 0xa55a);
	config_store(model, 8, 4, 0x02800001);
	config_store(model, 0x10, 4, 0x60300000);
	config_store(model, 0x100, 4, 0x11223344);
	assert(hakios_pcie_kona_create(&operations, model, &resources, &model->controller) == 0);
}

static void
model_destroy(struct model *model)
{
	assert(!model->guard_active && !model->config_pending);
	assert(memcmp(model->region, model->original, sizeof(model->region)) == 0);
	assert(hakios_pcie_kona_destroy(&model->controller) == 0);
	assert(model->controller == NULL);
}

static void
test_arguments(void)
{
	struct model model;
	model_init(&model, true);
	uint32_t value = UINT32_MAX;
	assert(hakios_pcie_kona_config_read(model.controller, 0, 0, 4, &value) == ENODEV && value == 0);
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 1, 2, &value) == EINVAL);
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0x1000, 4, &value) == EINVAL);
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 0, &value) == EINVAL);
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 3, &value) == EINVAL);
	assert(hakios_pcie_kona_config_read(NULL, 0x100, 0, 4, &value) == EINVAL);
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 4, NULL) == EINVAL);
	assert(hakios_pcie_kona_config_write(model.controller, 0x100, 0, 1, 0x100) == EINVAL);
	assert(model.calls == 0);
	struct hakios_pcie_kona *other = NULL;
	struct hakios_pcie_kona_resources wrong = resources;
	wrong.iatu.address += 0x1000;
	assert(hakios_pcie_kona_create(&operations, &model, &wrong, &other) == ENOTSUP && other == NULL);
	struct hakios_pcie_kona_operations missing = operations;
	missing.config_drain = NULL;
	assert(hakios_pcie_kona_create(&missing, &model, &resources, &other) == EINVAL);
	pcie_test_fail_allocation = true;
	assert(hakios_pcie_kona_create(&operations, &model, &resources, &other) == ENOMEM);
	pcie_test_fail_allocation = false;
	assert(hakios_pcie_kona_config_write(model.controller, 0x100, 0xfff, 1, 0x5a) == 0);
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0xfff, 1, &value) == 0 && value == 0x5a);
	model_destroy(&model);
}

static void
test_widths(void)
{
	for (unsigned enabled = 0; enabled < 2; enabled++) {
		struct model model;
		model_init(&model, enabled != 0);
		for (uint8_t width = 1; width <= 4; width *= 2) {
			uint32_t value = 0;
			uint32_t expected = 0x11223344u & (UINT32_MAX >> ((4 - width) * 8));
			assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0x100, width, &value) == 0);
			assert(value == expected);
			assert(hakios_pcie_kona_config_write(model.controller, 0x100, 0x104, width, expected) == 0);
			assert(config_load(&model, 0x104, width) == expected);
		}
		assert(hakios_pcie_kona_config_write(model.controller, 0x100, 4, 2, 2) == 0);
		assert(config_load(&model, 4, 2) == 2 && config_load(&model, 6, 2) == 0xa55a);
		model_destroy(&model);
	}
}

static int
transfer(struct model *model, bool write, uint32_t *value)
{
	return write ? hakios_pcie_kona_config_write(model->controller, 0x100, 0x100, 4, 0x55667788) :
	       hakios_pcie_kona_config_read(model->controller, 0x100, 0x100, 4, value);
}

static void
test_fault_sweep(void)
{
	for (unsigned enabled = 0; enabled < 2; enabled++) {
		for (unsigned write = 0; write < 2; write++) {
			struct model baseline;
			model_init(&baseline, enabled != 0);
			uint32_t value = 0;
			assert(transfer(&baseline, write != 0, &value) == 0);
			unsigned call_count = baseline.calls;
			model_destroy(&baseline);
			for (unsigned after = 0; after < 2; after++) {
				for (unsigned fault = 1; fault <= call_count; fault++) {
					struct model model;
					model_init(&model, enabled != 0);
					model.fault_at = fault;
					model.write_before_error = after != 0;
					model.begin_partial = after != 0;
					model.callback_error = after != 0 ? -EIO : EIO;
					value = UINT32_MAX;
					assert(transfer(&model, write != 0, &value) == EIO);
					assert(write != 0 || value == 0);
					if (model.guard_active) {
						assert(hakios_pcie_kona_destroy(&model.controller) == EBUSY);
						assert(model.controller != NULL);
					}
					unsigned transactions = model.config_reads + model.config_writes;
					model.fault_at = 0;
					assert(hakios_pcie_kona_recover(model.controller) == 0);
					assert(transactions == model.config_reads + model.config_writes);
					unsigned calls = model.calls;
					assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 4, &value) == EIO);
					assert(value == 0 && model.calls == calls);
					model_destroy(&model);
					fault_cases++;
				}
			}
		}
	}
}

static void
test_retained_cleanup(void)
{
	struct model model;
	uint32_t value;
	model_init(&model, true);
	model.drain_failure = true;
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 4, &value) == ETIMEDOUT);
	assert(value == 0 && model.guard_active && model.config_pending && model.end_calls == 0);
	assert(memcmp(model.region, config_region, sizeof(config_region)) == 0);
	unsigned writes = model.region_writes;
	assert(hakios_pcie_kona_recover(model.controller) == ETIMEDOUT && model.region_writes == writes);
	assert(hakios_pcie_kona_destroy(&model.controller) == EBUSY);
	model.drain_failure = false;
	model.restore_mismatch = true;
	assert(hakios_pcie_kona_recover(model.controller) == EIO);
	assert(model.guard_active && !model.config_pending && model.end_calls == 0);
	model.restore_mismatch = false;
	assert(hakios_pcie_kona_recover(model.controller) == 0);
	assert(model.config_reads == 1 && model.end_calls == 1);
	model_destroy(&model);
	model_init(&model, false);
	model.stuck_enable = true;
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 4, &value) == ETIMEDOUT);
	assert(model.delays == 99 && model.config_reads == 0);
	model_destroy(&model);
	model_init(&model, false);
	model.stuck_enable = true;
	model.delay_failure = true;
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 4, &value) == EIO);
	assert(model.delays == 1 && model.config_reads == 0);
	model_destroy(&model);
	model_init(&model, true);
	model.stuck_disable = true;
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 4, &value) == ETIMEDOUT);
	assert(model.delays == 198 && model.guard_active && model.config_reads == 0);
	model.stuck_disable = false;
	assert(hakios_pcie_kona_recover(model.controller) == 0);
	model_destroy(&model);
	model_init(&model, true);
	model.config_mismatch = true;
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 4, &value) == EIO);
	assert(model.config_reads == 0);
	model_destroy(&model);
	model_init(&model, true);
	model.link_down = true;
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 4, &value) == ENXIO);
	assert(model.region_writes == 0);
	model_destroy(&model);
	model_init(&model, true);
	model.wide_read = true;
	assert(hakios_pcie_kona_config_read(model.controller, 0x100, 0, 1, &value) == EIO && value == 0);
	model_destroy(&model);
}

static int
provider_acquire(void *context, const struct hakios_pcie_endpoint_request *request,
    struct hakios_pcie_endpoint_resources *result, void **owner)
{
	struct model *model = context;
	assert(request != NULL && !model->owner_active);
	model->acquire_calls++;
	model->saved_command = (uint16_t)config_load(model, 4, 2);
	*result = (struct hakios_pcie_endpoint_resources) {
		.domain = 0, .requester_id = 0x100, .stream_id = model->bad_sid ? 0x1c00 : 0x1c01,
		.bar0_address = 0x60300000, .bar0_size = 0x100000,
	};
	model->owner_active = !model->acquire_without_owner;
	*owner = model->owner_active ? model : NULL;
	return model->acquire_error;
}

static int
provider_release(void *context, void *owner)
{
	struct model *model = context;
	assert(owner == model && model->owner_active && !model->bar_active);
	model->release_calls++;
	if (model->release_error != 0) {
		return model->release_error;
	}
	int error = hakios_pcie_kona_recover(model->controller);
	if (error != 0) {
		return error;
	}
	config_store(model, 4, 2, model->saved_command);
	model->owner_active = false;
	return 0;
}

static int
provider_read(void *context, void *owner, uint16_t offset, uint8_t width, uint32_t *value)
{
	struct model *model = context;
	assert(owner == model && model->owner_active);
	return hakios_pcie_kona_config_read(model->controller, 0x100, offset, width, value);
}

static int
provider_write(void *context, void *owner, uint16_t offset, uint8_t width, uint32_t value)
{
	struct model *model = context;
	assert(owner == model && model->owner_active);
	return hakios_pcie_kona_config_write(model->controller, 0x100, offset, width, value);
}

static int
provider_bar_begin(void *context, void *owner, bool wake_required)
{
	struct model *model = context;
	assert(owner == model && model->owner_active && !model->bar_active);
	model->bar_begin_calls++;
	model->wake_seen = wake_required;
	model->bar_active = model->bar_begin_error == 0;
	return model->bar_begin_error;
}

static int
provider_bar_end(void *context, void *owner)
{
	struct model *model = context;
	assert(owner == model && model->bar_active);
	model->bar_end_calls++;
	model->bar_active = false;
	return model->bar_end_error;
}

static int
provider_bar_read(void *context, void *owner, uint32_t offset, uint32_t *value)
{
	struct model *model = context;
	assert(owner == model && model->bar_active && offset / 4 < 256);
	sched_yield();
	model->bar_calls++;
	*value = model->bar[offset / 4];
	return model->bar_calls == model->bar_fault_at ? EIO : 0;
}

static int
provider_bar_write(void *context, void *owner, uint32_t offset, uint32_t value)
{
	struct model *model = context;
	assert(owner == model && model->bar_active && offset / 4 < 256);
	sched_yield();
	model->bar_calls++;
	model->bar[offset / 4] = value;
	return model->bar_calls == model->bar_fault_at ? EIO : 0;
}

static const struct hakios_pcie_rc0_provider provider = {
	.version = HAKIOS_PCIE_PROVIDER_VERSION,
	.acquire = provider_acquire, .release = provider_release,
	.config_read = provider_read, .config_write = provider_write,
	.access_begin = provider_bar_begin, .access_end = provider_bar_end,
	.bar0_read32 = provider_bar_read, .bar0_write32 = provider_bar_write,
};

static hakios_pcie_lease_t
register_endpoint(struct model *model)
{
	hakios_pcie_lease_t lease = 0;
	model_init(model, true);
	assert(hakios_pcie_rc0_register(&provider, model) == 0);
	assert(hakios_pcie_endpoint_acquire(&endpoint, &lease) == 0 && lease != 0);
	assert(config_load(model, 4, 2) == 2 && config_load(model, 6, 2) == 0xa55a);
	return lease;
}

static void
unregister_endpoint(struct model *model, hakios_pcie_lease_t lease)
{
	assert(hakios_pcie_endpoint_release(lease) == 0);
	assert(config_load(model, 4, 2) == model->saved_command);
	assert(hakios_pcie_endpoint_status(lease) == ESTALE);
	assert(hakios_pcie_rc0_unregister(model) == 0);
	model_destroy(model);
}

static void
test_endpoint_lifecycle(void)
{
	struct model model;
	hakios_pcie_lease_t lease = UINT64_MAX;
	assert(hakios_pcie_endpoint_acquire(&endpoint, &lease) == ENOTSUP && lease == 0);
	lease = register_endpoint(&model);
	struct hakios_pcie_endpoint_resources resources, empty = {0};
	unsigned config_reads = model.config_reads;
	assert(hakios_pcie_endpoint_resources(lease, &resources) == 0);
	assert(resources.domain == 0 && resources.requester_id == 0x100 && resources.stream_id == 0x1c01);
	assert(resources.bar0_address == 0x60300000 && resources.bar0_size == 0x100000);
	assert(model.config_reads == config_reads);
	assert(hakios_pcie_endpoint_resources(lease, NULL) == EINVAL);
	assert(hakios_pcie_endpoint_drop(lease) == EINVAL);
	assert(hakios_pcie_endpoint_hold(lease) == 0);
	assert(hakios_pcie_endpoint_release(lease) == EBUSY);
	assert(hakios_pcie_endpoint_drop(lease) == 0);
	assert(hakios_pcie_rc0_register(&provider, &model) == EBUSY);
	assert(hakios_pcie_rc0_unregister(&model) == EBUSY);
	assert(hakios_pcie_rc0_recover(&model) == EBUSY);
	hakios_pcie_lease_t other = UINT64_MAX;
	assert(hakios_pcie_endpoint_acquire(&endpoint, &other) == EBUSY && other == 0);
	uint32_t value;
	assert(hakios_pcie_config_read(lease, 0, 4, &value) == 0 && value == 0x110117cb);
	assert(hakios_pcie_config_read(lease, 1, 2, &value) == EINVAL && value == 0);
	model.release_error = -ETIMEDOUT;
	assert(hakios_pcie_endpoint_release(lease) == EIO);
	assert(hakios_pcie_endpoint_status(lease) == EIO);
	assert(hakios_pcie_endpoint_resources(lease, &resources) == EIO);
	assert(memcmp(&resources, &empty, sizeof(resources)) == 0);
	assert(hakios_pcie_rc0_unregister(&model) == EBUSY);
	model.release_error = 0;
	unregister_endpoint(&model, lease);
	other = register_endpoint(&model);
	assert(other > lease && hakios_pcie_endpoint_status(lease) == ESTALE);
	assert(hakios_pcie_endpoint_hold(lease) == ESTALE && hakios_pcie_endpoint_drop(lease) == ESTALE);
	memset(&resources, 0xff, sizeof(resources));
	assert(hakios_pcie_endpoint_resources(lease, &resources) == ESTALE);
	assert(memcmp(&resources, &empty, sizeof(resources)) == 0);
	unregister_endpoint(&model, other);
}

static void
test_endpoint_rejections(void)
{
	for (unsigned scenario = 0; scenario < 5; scenario++) {
		struct model model;
		model_init(&model, true);
		int expected = ENODEV;
		if (scenario == 0) {
			model.bad_sid = true;
		} else if (scenario == 1) {
			config_store(&model, 0, 4, UINT32_MAX);
		} else if (scenario == 2) {
			config_store(&model, 0x10, 4, 0x60300001);
		} else if (scenario == 3) {
			config_store(&model, 4, 2, 4);
			expected = EBUSY;
		} else {
			model.acquire_error = ENXIO;
			expected = ENXIO;
		}
		assert(hakios_pcie_rc0_register(&provider, &model) == 0);
		hakios_pcie_lease_t lease;
		assert(hakios_pcie_endpoint_acquire(&endpoint, &lease) == expected && lease == 0);
		assert(model.config_writes == 0 && model.release_calls == 1 && !model.owner_active);
		assert(hakios_pcie_rc0_unregister(&model) == 0);
		model_destroy(&model);
	}
	struct model model;
	model_init(&model, true);
	model.bad_sid = true;
	model.release_error = ETIMEDOUT;
	assert(hakios_pcie_rc0_register(&provider, &model) == 0);
	hakios_pcie_lease_t lease;
	assert(hakios_pcie_endpoint_acquire(&endpoint, &lease) == ETIMEDOUT && lease == 0);
	assert(model.owner_active && hakios_pcie_rc0_unregister(&model) == EBUSY);
	assert(hakios_pcie_endpoint_acquire(&endpoint, &lease) == EIO);
	model.release_error = 0;
	assert(hakios_pcie_rc0_recover(&model) == 0 && !model.owner_active);
	assert(hakios_pcie_rc0_unregister(&model) == 0);
	model_destroy(&model);
}

static void
test_endpoint_config_recovery(void)
{
	struct model model;
	hakios_pcie_lease_t lease = register_endpoint(&model);
	assert(hakios_pcie_endpoint_hold(lease) == 0);
	model.drain_failure = true;
	uint32_t value = UINT32_MAX;
	assert(hakios_pcie_config_read(lease, 0, 4, &value) == ETIMEDOUT && value == 0);
	assert(hakios_pcie_endpoint_status(lease) == EIO);
	assert(hakios_pcie_endpoint_release(lease) == EBUSY);
	assert(hakios_pcie_endpoint_drop(lease) == 0);
	assert(hakios_pcie_endpoint_release(lease) == ETIMEDOUT);
	assert(model.owner_active && model.guard_active && model.config_pending);
	assert(hakios_pcie_rc0_unregister(&model) == EBUSY);
	unsigned transactions = model.config_reads + model.config_writes;
	model.drain_failure = false;
	unregister_endpoint(&model, lease);
	assert(transactions == model.config_reads + model.config_writes);
}

static void
test_bar_batches(void)
{
	for (unsigned scenario = 0; scenario < 6; scenario++) {
		struct model model;
		hakios_pcie_lease_t lease = register_endpoint(&model);
		struct hakios_pcie_bar_operation batch[] = {
			{ .type = HAKIOS_PCIE_BAR_WRITE32, .offset = 0, .value = 0x40000003 },
			{ .type = HAKIOS_PCIE_BAR_VERIFY32, .offset = 0, .value = 0x40000003, .mask = 0x4000003f },
			{ .type = HAKIOS_PCIE_BAR_READ32, .offset = 4, .value = UINT32_MAX },
			{ .type = HAKIOS_PCIE_BAR_READ32, .offset = 8, .value = UINT32_MAX },
		};
		model.bar[1] = 0x12345678;
		int expected = 0;
		if (scenario == 1) {
			batch[3].offset = 0x100000;
			expected = EINVAL;
		} else if (scenario == 2) {
			batch[1].value = 0x40000004;
			expected = EIO;
		} else if (scenario == 3) {
			model.bar_fault_at = 4;
			expected = EIO;
		} else if (scenario == 4) {
			model.bar_end_error = EIO;
			expected = EIO;
		} else if (scenario == 5) {
			model.bar_begin_error = ENOTSUP;
			expected = ENOTSUP;
		}
		assert(hakios_pcie_bar0_transfer(lease, batch, 4, true) == expected);
		if (expected != 0) {
			assert(batch[2].value == 0 && batch[3].value == 0);
		} else {
			assert(batch[2].value == 0x12345678 && model.wake_seen);
		}
		assert(hakios_pcie_endpoint_status(lease) == (expected == EIO ? EIO : 0));
		if (scenario == 1 || scenario == 5) {
			assert(model.bar_calls == 0 && model.bar_end_calls == 0);
		} else {
			assert(model.bar_end_calls == 1);
		}
		if (scenario == 2) {
			assert(model.bar_calls == 2);
		}
		unregister_endpoint(&model, lease);
	}
}

struct reader {
	struct hakios_pcie_kona *controller;
	hakios_pcie_lease_t lease;
	bool bar;
	uint32_t marker;
};

static void *
read_thread(void *context)
{
	struct reader *reader = context;
	for (unsigned iteration = 0; iteration < 100; iteration++) {
		if (reader->bar) {
			struct hakios_pcie_bar_operation batch[] = {
				{ .type = HAKIOS_PCIE_BAR_WRITE32, .offset = 0, .value = reader->marker },
				{ .type = HAKIOS_PCIE_BAR_VERIFY32, .offset = 0, .value = reader->marker, .mask = UINT32_MAX },
				{ .type = HAKIOS_PCIE_BAR_READ32, .offset = 0 },
			};
			assert(hakios_pcie_bar0_transfer(reader->lease, batch, 3, true) == 0);
			assert(batch[2].value == reader->marker);
			continue;
		}
		uint32_t value;
		int error = reader->lease != 0 ? hakios_pcie_config_read(reader->lease, 0, 4, &value) :
		    hakios_pcie_kona_config_read(reader->controller, 0x100, 0, 4, &value);
		assert(error == 0 && value == 0x110117cb);
	}
	return NULL;
}

static void
test_serialization(void)
{
	for (unsigned native = 0; native < 3; native++) {
		struct model model;
		hakios_pcie_lease_t lease = 0;
		if (native != 0) {
			lease = register_endpoint(&model);
		} else {
			model_init(&model, true);
		}
		struct reader readers[4];
		pthread_t threads[4];
		for (unsigned index = 0; index < 4; index++) {
			readers[index] = (struct reader) {
				.controller = model.controller, .lease = lease, .bar = native == 2, .marker = index + 1,
			};
			assert(pthread_create(&threads[index], NULL, read_thread, &readers[index]) == 0);
		}
		for (unsigned index = 0; index < 4; index++) {
			assert(pthread_join(threads[index], NULL) == 0);
		}
		if (native != 0) {
			unregister_endpoint(&model, lease);
		} else {
			model_destroy(&model);
		}
	}
}

static struct model quarantined_model;

static void
test_contract_violations(void)
{
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		model_init(&quarantined_model, true);
		quarantined_model.begin_without_guard = true;
		uint32_t value;
		assert(hakios_pcie_kona_config_read(quarantined_model.controller, 0x100, 0, 4, &value) == EIO);
		assert(value == 0 && hakios_pcie_kona_recover(quarantined_model.controller) == EIO);
		assert(hakios_pcie_kona_destroy(&quarantined_model.controller) == EBUSY);
		struct model model;
		model_init(&model, true);
		model.acquire_without_owner = true;
		assert(hakios_pcie_rc0_register(&provider, &model) == 0);
		hakios_pcie_lease_t lease;
		assert(hakios_pcie_endpoint_acquire(&endpoint, &lease) == EIO && lease == 0);
		assert(hakios_pcie_rc0_recover(&model) == EIO);
		assert(hakios_pcie_rc0_unregister(&model) == EBUSY);
		model_destroy(&model);
		exit(0);
	}
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int
cold_model_authorize(void *context, void **ownership)
{
	struct model *model = context;

	model->cold_calls++;
	if (model->cold_owned) {
		*ownership = NULL;
		return EBUSY;
	}
	model->cold_owned = true;
	*ownership = model;
	return model->cold_authorize_partial_fail ? EIO : 0;
}

static int
cold_model_revoke(void *context, void **ownership)
{
	struct model *model = context;

	assert(*ownership == model && model->cold_active == 0);
	model->cold_revokes++;
	model->cold_calls++;
	if (model->cold_calls == model->cold_fail_at) {
		return EIO;
	}
	model->cold_owned = false;
	*ownership = NULL;
	return 0;
}

static int
cold_model_stage(void *context, void *ownership, enum hakios_pcie_kona_cold_stage stage,
    bool acquire, void **cookie)
{
	struct model *model = context;

	assert(ownership == model && stage < HAKIOS_PCIE_KONA_COLD_STAGE_COUNT);
	model->cold_calls++;
	if (model->cold_calls == model->cold_fail_at) {
		if (acquire && model->cold_partial_fail) {
			model->cold_active |= 1u << stage;
			*cookie = (void *)(uintptr_t)(stage + 1);
		}
		return EIO;
	}
	if (acquire) {
		assert(*cookie == NULL);
		model->cold_active |= 1u << stage;
		*cookie = (void *)(uintptr_t)(stage + 1);
	} else {
		assert(*cookie == (void *)(uintptr_t)(stage + 1));
		model->cold_active &= ~(1u << stage);
		*cookie = NULL;
	}
	return 0;
}

static const struct hakios_pcie_kona_cold_owner_operations cold_operations = {
	.version = HAKIOS_PCIE_KONA_COLD_OWNER_VERSION,
	.authorize = cold_model_authorize,
	.revoke = cold_model_revoke,
	.stage = cold_model_stage,
};

static int
platform_bar_begin(void *context, void *owner, bool wake_required)
{
	struct model *model = context;

	assert(owner == model->kona_platform && model->cold_owned && !model->bar_active);
	model->bar_begin_calls++;
	model->wake_seen = wake_required;
	model->bar_active = true;
	return 0;
}

static int
platform_bar_end(void *context, void *owner)
{
	struct model *model = context;

	assert(owner == model->kona_platform && model->bar_active);
	model->bar_end_calls++;
	model->bar_active = false;
	return 0;
}

static int
platform_bar_read(void *context, void *owner, uint32_t offset, uint32_t *value)
{
	struct model *model = context;

	assert(owner == model->kona_platform && model->bar_active && offset / 4 < 256);
	*value = model->bar[offset / 4];
	model->bar_calls++;
	return 0;
}

static int
platform_bar_write(void *context, void *owner, uint32_t offset, uint32_t value)
{
	struct model *model = context;

	assert(owner == model->kona_platform && model->bar_active && offset / 4 < 256);
	model->bar[offset / 4] = value;
	model->bar_calls++;
	return 0;
}

static const struct hakios_pcie_kona_platform_operations platform_operations = {
	.version = HAKIOS_PCIE_KONA_PLATFORM_VERSION,
	.cold = {
		.version = HAKIOS_PCIE_KONA_COLD_OWNER_VERSION,
		.authorize = cold_model_authorize,
		.revoke = cold_model_revoke,
		.stage = cold_model_stage,
	},
	.config = {
		.version = HAKIOS_PCIE_KONA_OPERATIONS_VERSION,
		.begin = model_begin,
		.end = model_end,
		.read = model_read,
		.write = model_write,
		.config_drain = model_drain,
		.delay_us = model_delay,
	},
	.bar_begin = platform_bar_begin,
	.bar_end = platform_bar_end,
	.bar_read32 = platform_bar_read,
	.bar_write32 = platform_bar_write,
};

static void
test_cold_owner(void)
{
	struct model model = { 0 };

	assert(hakios_pcie_kona_cold_owner_create(&cold_operations, &model, &model.cold_owner) == 0);
	assert(hakios_pcie_kona_cold_owner_acquire(model.cold_owner) == 0);
	assert(model.cold_active == (1u << HAKIOS_PCIE_KONA_COLD_STAGE_COUNT) - 1);
	assert(hakios_pcie_kona_cold_owner_acquire(model.cold_owner) == EBUSY);
	assert(hakios_pcie_kona_cold_owner_release(model.cold_owner) == 0);
	assert(model.cold_active == 0 && !model.cold_owned && model.cold_revokes == 1);
	assert(hakios_pcie_kona_cold_owner_release(model.cold_owner) == ENXIO);
	assert(hakios_pcie_kona_cold_owner_destroy(&model.cold_owner) == 0);

	memset(&model, 0, sizeof(model));
	assert(hakios_pcie_kona_cold_owner_create(&cold_operations, &model, &model.cold_owner) == 0);
	model.cold_authorize_partial_fail = true;
	assert(hakios_pcie_kona_cold_owner_acquire(model.cold_owner) == EIO);
	assert(!model.cold_owned && model.cold_revokes == 1);
	assert(hakios_pcie_kona_cold_owner_destroy(&model.cold_owner) == 0);

	memset(&model, 0, sizeof(model));
	assert(hakios_pcie_kona_cold_owner_create(&cold_operations, &model, &model.cold_owner) == 0);
	model.cold_fail_at = 5;
	model.cold_partial_fail = true;
	assert(hakios_pcie_kona_cold_owner_acquire(model.cold_owner) == EIO);
	assert(model.cold_active == 0 && !model.cold_owned);
	assert(hakios_pcie_kona_cold_owner_destroy(&model.cold_owner) == 0);

	memset(&model, 0, sizeof(model));
	assert(hakios_pcie_kona_cold_owner_create(&cold_operations, &model, &model.cold_owner) == 0);
	assert(hakios_pcie_kona_cold_owner_acquire(model.cold_owner) == 0);
	model.cold_fail_at = model.cold_calls + HAKIOS_PCIE_KONA_COLD_STAGE_COUNT + 1;
	assert(hakios_pcie_kona_cold_owner_release(model.cold_owner) == EIO);
	assert(model.cold_active == 0 && model.cold_owned);
	model.cold_fail_at = 0;
	assert(hakios_pcie_kona_cold_owner_recover(model.cold_owner) == 0);
	assert(!model.cold_owned);
	assert(hakios_pcie_kona_cold_owner_destroy(&model.cold_owner) == 0);

	memset(&model, 0, sizeof(model));
	assert(hakios_pcie_kona_cold_owner_create(&cold_operations, &model, &model.cold_owner) == 0);
	assert(hakios_pcie_kona_cold_owner_acquire(model.cold_owner) == 0);
	model.cold_fail_at = model.cold_calls + 1;
	assert(hakios_pcie_kona_cold_owner_release(model.cold_owner) == EIO);
	assert(model.cold_owned && model.cold_active != 0);
	model.cold_fail_at = 0;
	assert(hakios_pcie_kona_cold_owner_recover(model.cold_owner) == 0);
	assert(model.cold_active == 0 && !model.cold_owned);
	assert(hakios_pcie_kona_cold_owner_destroy(&model.cold_owner) == 0);

	memset(&model, 0, sizeof(model));
	assert(hakios_pcie_kona_cold_owner_create(&cold_operations, &model, &model.cold_owner) == 0);
	assert(hakios_pcie_kona_cold_owner_acquire(model.cold_owner) == 0);
	model.cold_fail_at = model.cold_calls + HAKIOS_PCIE_KONA_COLD_STAGE_COUNT + 1;
	assert(hakios_pcie_kona_cold_owner_release(model.cold_owner) == EIO);
	assert(model.cold_active == 0 && model.cold_owned);
	model.cold_fail_at = 0;
	assert(hakios_pcie_kona_cold_owner_recover(model.cold_owner) == 0);
	assert(!model.cold_owned);
	assert(hakios_pcie_kona_cold_owner_destroy(&model.cold_owner) == 0);

	memset(&model, 0, sizeof(model));
	assert(hakios_pcie_kona_cold_owner_create(&cold_operations, &model, &model.cold_owner) == 0);
	model.cold_fail_at = 2;
	assert(hakios_pcie_kona_cold_owner_acquire(model.cold_owner) == EIO);
	assert(!model.cold_owned && model.cold_active == 0);
	assert(hakios_pcie_kona_cold_owner_destroy(&model.cold_owner) == 0);
}

static void
test_kona_platform_provider(void)
{
	struct model model;
	struct hakios_pcie_bar_operation operation = {
		.type = HAKIOS_PCIE_BAR_READ32,
		.offset = 0x224,
	};
	hakios_pcie_lease_t lease = 0;

	model_init(&model, true);
	model.bar[0x224 / 4] = 0x200;
	assert(hakios_pcie_kona_platform_create(&platform_operations, &model,
	    &model.kona_platform) == 0);
	assert(!hakios_pcie_kona_platform_active(model.kona_platform));
	assert(hakios_pcie_kona_platform_activate(model.kona_platform) == 0);
	assert(hakios_pcie_kona_platform_active(model.kona_platform));
	assert(model.cold_owned && model.cold_active != 0);
	assert(hakios_pcie_endpoint_acquire(&endpoint, &lease) == 0 && lease != 0);
	assert(hakios_pcie_bar0_transfer(lease, &operation, 1, false) == 0);
	assert(operation.value == 0x200 && model.bar_begin_calls == 1 && model.bar_end_calls == 1);
	assert(hakios_pcie_endpoint_release(lease) == 0);
	assert(model.cold_owned && model.cold_active != 0);
	assert(hakios_pcie_kona_platform_active(model.kona_platform));
	assert(hakios_pcie_kona_platform_deactivate(model.kona_platform) == 0);
	assert(!model.cold_owned && model.cold_active == 0);
	assert(hakios_pcie_kona_platform_activate(model.kona_platform) == 0);
	assert(model.cold_owned && model.cold_active != 0);
	assert(hakios_pcie_kona_platform_deactivate(model.kona_platform) == 0);
	assert(!model.cold_owned && model.cold_active == 0);
	assert(hakios_pcie_kona_platform_destroy(&model.kona_platform) == 0);
	model_destroy(&model);

	model_init(&model, true);
	assert(hakios_pcie_kona_platform_create(&platform_operations, &model,
	    &model.kona_platform) == 0);
	model.cold_fail_at = 5;
	assert(hakios_pcie_kona_platform_activate(model.kona_platform) == EIO);
	assert(!hakios_pcie_kona_platform_active(model.kona_platform));
	assert(!model.cold_owned && model.cold_active == 0);
	assert(hakios_pcie_kona_platform_destroy(&model.kona_platform) == 0);
	model_destroy(&model);

	model_init(&model, true);
	assert(hakios_pcie_kona_platform_create(&platform_operations, &model,
	    &model.kona_platform) == 0);
	assert(hakios_pcie_kona_platform_activate(model.kona_platform) == 0);
	model.cold_fail_at = model.cold_calls + 1;
	assert(hakios_pcie_kona_platform_deactivate(model.kona_platform) == EIO);
	assert(model.cold_owned && model.cold_active != 0);
	model.cold_fail_at = 0;
	assert(hakios_pcie_kona_platform_deactivate(model.kona_platform) == 0);
	assert(!model.cold_owned && model.cold_active == 0);
	assert(hakios_pcie_kona_platform_destroy(&model.kona_platform) == 0);
	model_destroy(&model);
}

int
main(void)
{
	test_arguments();
	test_widths();
	test_fault_sweep();
	test_retained_cleanup();
	test_endpoint_lifecycle();
	test_endpoint_rejections();
	test_endpoint_config_recovery();
	test_bar_batches();
	test_serialization();
	test_contract_violations();
	test_cold_owner();
	test_kona_platform_provider();
	printf("pcie-test: PASS (%u injected callback failures; Kona provider glue, lifecycle, cold-owner rollback, BAR, widths, polling, recovery, concurrency)\n",
	    fault_cases);
	return 0;
}

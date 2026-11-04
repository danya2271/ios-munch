#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/hakios.mhi.boot.h>
#include <sys/hakios.wire.h>

struct hakios_mhi_boot {
	lck_mtx_t lock;
	struct hakios_mhi_boot_config config;
	struct hakios_mhi_boot_operations operations;
	struct hakios_mhi_boot_buffer segments[HAKIOS_MHI_FW_SEGMENTS];
	void *owner;
	enum hakios_mhi_boot_phase phase;
	uint64_t deadline;
	uint64_t last_time;
	uint32_t bhi;
	uint32_t bhie;
};

static LCK_GRP_DECLARE(mhi_boot_group, "hakios-mhi-boot");

static int
boot_error(int error)
{
	return error < 0 ? EIO : error;
}

static bool
boot_range(uint64_t start, uint32_t size, uint64_t low, uint64_t high)
{
	return start != 0 && size != 0 && start >= low && start <= high && size - 1 <= high - start;
}

static bool
boot_overlap(uint64_t first, uint32_t first_size, uint64_t second, uint32_t second_size)
{
	return first <= second ? second - first < first_size : first - second < second_size;
}

static int
boot_validate(const struct hakios_mhi_boot_config *config)
{
	if (config == NULL || config->image == NULL || config->segments == NULL || config->image_size == 0 ||
	    config->image_size > HAKIOS_MHI_FW_SEGMENT * HAKIOS_MHI_FW_SEGMENTS ||
	    (uintptr_t)config->image > UINTPTR_MAX - config->image_size ||
	    config->segment_count != (config->image_size + HAKIOS_MHI_FW_SEGMENT - 1) / HAKIOS_MHI_FW_SEGMENT ||
	    config->sequence == 0 || config->sequence >= 0x3fffffff || config->timeout_ms == 0 || config->timeout_ms > 120000 ||
	    config->aperture_start > config->aperture_end || config->aperture_end == UINT64_MAX) {
		return EINVAL;
	}
	uint64_t contexts[] = {config->channel_context, config->event_context, config->command_context};
	uint32_t sizes[] = {128 * 44, 2 * 44, 44};
	for (unsigned index = 0; index < 3; index++) {
		if ((contexts[index] & 7) != 0 || !boot_range(contexts[index], sizes[index], config->aperture_start, config->aperture_end)) {
			return EINVAL;
		}
		for (unsigned prior = 0; prior < index; prior++) {
			if (boot_overlap(contexts[index], sizes[index], contexts[prior], sizes[prior])) {
				return EINVAL;
			}
		}
	}
	for (unsigned index = 0; index <= config->segment_count; index++) {
		const struct hakios_mhi_boot_buffer *buffer = index == config->segment_count ? &config->vectors : &config->segments[index];
		uint32_t needed = index == config->segment_count ? config->segment_count * 16 :
		    MIN(HAKIOS_MHI_FW_SEGMENT, config->image_size - index * HAKIOS_MHI_FW_SEGMENT);
		if (buffer->bytes == NULL || buffer->capacity < needed || (buffer->iova & 15) != 0 ||
		    !boot_range(buffer->iova, buffer->capacity, config->aperture_start, config->aperture_end) ||
		    (uintptr_t)buffer->bytes > UINTPTR_MAX - buffer->capacity ||
		    boot_overlap((uintptr_t)buffer->bytes, buffer->capacity, (uintptr_t)config->image, config->image_size)) {
			return EINVAL;
		}
		for (unsigned prior = 0; prior < index; prior++) {
			const struct hakios_mhi_boot_buffer *other = &config->segments[prior];
			if (boot_overlap(buffer->iova, buffer->capacity, other->iova, other->capacity) ||
			    boot_overlap((uintptr_t)buffer->bytes, buffer->capacity, (uintptr_t)other->bytes, other->capacity)) {
				return EINVAL;
			}
		}
		for (unsigned context = 0; context < 3; context++) {
			if (boot_overlap(buffer->iova, buffer->capacity, contexts[context], sizes[context])) {
				return EINVAL;
			}
		}
	}
	return 0;
}

int
hakios_mhi_boot_create(const struct hakios_mhi_boot_config *config,
    const struct hakios_mhi_boot_operations *operations, void *owner, struct hakios_mhi_boot **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	int error = boot_validate(config);
	if (error != 0 || operations == NULL || owner == NULL || operations->version != HAKIOS_MHI_BOOT_VERSION ||
	    operations->activate == NULL || operations->read == NULL || operations->write == NULL ||
	    operations->sync == NULL || operations->ready == NULL || operations->quiesce == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_boot *boot = kalloc_type(struct hakios_mhi_boot, Z_WAITOK | Z_ZERO);
	if (boot == NULL) {
		return ENOMEM;
	}
	boot->config = *config;
	boot->config.image = NULL;
	boot->config.segments = boot->segments;
	boot->operations = *operations;
	boot->owner = owner;
	for (unsigned index = 0; index < config->segment_count; index++) {
		uint32_t size = MIN(HAKIOS_MHI_FW_SEGMENT, config->image_size - index * HAKIOS_MHI_FW_SEGMENT);
		boot->segments[index] = config->segments[index];
		memcpy(boot->segments[index].bytes, config->image + index * HAKIOS_MHI_FW_SEGMENT, size);
		hakios_wire_put64(config->vectors.bytes + index * 16, boot->segments[index].iova);
		hakios_wire_put64(config->vectors.bytes + index * 16 + 8, size);
	}
	lck_mtx_init(&boot->lock, &mhi_boot_group, LCK_ATTR_NULL);
	*result = boot;
	return 0;
}

static int
boot_read(struct hakios_mhi_boot *boot, uint32_t offset, uint32_t *value)
{
	*value = UINT32_MAX;
	return boot_error(boot->operations.read(boot->owner, offset, value));
}

static int
boot_write(struct hakios_mhi_boot *boot, uint32_t offset, uint32_t value)
{
	return boot_error(boot->operations.write(boot->owner, offset, value));
}

static int
boot_address(struct hakios_mhi_boot *boot, uint32_t low, uint64_t address)
{
	uint32_t lower, upper;
	int error = boot_write(boot, low + 4, (uint32_t)(address >> 32));
	if (error != 0 || (error = boot_write(boot, low, (uint32_t)address)) != 0 ||
	    (error = boot_read(boot, low + 4, &upper)) != 0 || (error = boot_read(boot, low, &lower)) != 0) {
		return error;
	}
	return upper == (uint32_t)(address >> 32) && lower == (uint32_t)address ? 0 : EIO;
}

static void
boot_phase(struct hakios_mhi_boot *boot, enum hakios_mhi_boot_phase phase, uint64_t now)
{
	boot->phase = phase;
	boot->deadline = now + boot->config.timeout_ms;
}

int
hakios_mhi_boot_start(struct hakios_mhi_boot *boot, uint64_t now_ms)
{
	if (boot == NULL || now_ms > UINT64_MAX - boot->config.timeout_ms) {
		return EINVAL;
	}
	lck_mtx_lock(&boot->lock);
	int error = 0;
	uint32_t value;
	if (boot->phase != HAKIOS_MHI_BOOT_NEW) {
		error = boot->phase == HAKIOS_MHI_BOOT_FAULT ? EIO : EALREADY;
		goto out;
	}
	error = boot_error(boot->operations.activate(boot->owner));
	if (error != 0 || (error = boot_read(boot, 0x28, &boot->bhi)) != 0 ||
	    (error = boot_read(boot, 0x2c, &boot->bhie)) != 0) {
		goto fault;
	}
	if (boot->bhi < 0x100 || boot->bhi > 0x100000 - 0x48 || (boot->bhi & 3) != 0 ||
	    boot->bhie < 0x100 || boot->bhie > 0x100000 - 0x7c || (boot->bhie & 3) != 0 ||
	    boot_overlap(boot->bhi, 0x48, boot->bhie, 0x7c)) {
		error = EPROTO;
		goto fault;
	}
	if ((error = boot_read(boot, boot->bhi + 0x28, &value)) != 0) {
		goto fault;
	}
	if (value != 0) {
		error = EBUSY;
		goto fault;
	}
	if ((error = boot_read(boot, 0x48, &value)) != 0) {
		goto fault;
	}
	if ((value & 4) != 0 || (((value >> 8) & 255) != 0 && ((value >> 8) & 255) != 7)) {
		error = EBUSY;
		goto fault;
	}
	if ((error = boot_read(boot, 0x38, &value)) != 0) {
		goto fault;
	}
	if ((value & 2) != 0) {
		error = EBUSY;
		goto fault;
	}
	for (unsigned index = 0; index < boot->config.segment_count; index++) {
		uint32_t size = MIN(HAKIOS_MHI_FW_SEGMENT, boot->config.image_size - index * HAKIOS_MHI_FW_SEGMENT);
		error = boot_error(boot->operations.sync(boot->owner, boot->segments[index].iova, size));
		if (error != 0) {
			goto fault;
		}
	}
	if ((error = boot_error(boot->operations.sync(boot->owner, boot->config.vectors.iova, boot->config.segment_count * 16))) != 0 ||
	    (error = boot_write(boot, boot->bhi + 0x2c, 0)) != 0 ||
	    (error = boot_read(boot, boot->bhi + 0x2c, &value)) != 0) {
		goto fault;
	}
	if (value != 0) {
		error = EIO;
		goto fault;
	}
	if ((error = boot_address(boot, boot->bhi + 8, boot->segments[0].iova)) != 0 ||
	    (error = boot_write(boot, boot->bhi + 0x10, MIN(boot->config.image_size, HAKIOS_MHI_FW_SEGMENT))) != 0 ||
	    (error = boot_write(boot, boot->bhi + 0x18, boot->config.sequence)) != 0) {
		goto fault;
	}
	boot->last_time = now_ms;
	boot_phase(boot, HAKIOS_MHI_BOOT_BHI, now_ms);
	goto out;
fault:
	boot->phase = HAKIOS_MHI_BOOT_FAULT;
out:
	lck_mtx_unlock(&boot->lock);
	return error;
}

static int
boot_contexts(struct hakios_mhi_boot *boot)
{
	uint64_t contexts[] = {boot->config.channel_context, boot->config.event_context, boot->config.command_context};
	uint32_t sizes[] = {128 * 44, 2 * 44, 44};
	int error;
	for (unsigned index = 0; index < 3; index++) {
		error = boot_error(boot->operations.sync(boot->owner, contexts[index], sizes[index]));
		if (error != 0 || (error = boot_address(boot, 0x58 + index * 8, contexts[index])) != 0) {
			return error;
		}
	}
	uint32_t config, control, observed;
	if ((error = boot_read(boot, 0x10, &config)) != 0 ||
	    (error = boot_write(boot, 0x10, (config & 0xffff) | 2u << 16)) != 0 ||
	    (error = boot_read(boot, 0x10, &observed)) != 0) {
		return error;
	}
	if ((observed >> 16) != 2) {
		return EIO;
	}
	if ((error = boot_address(boot, 0x80, boot->config.aperture_start)) != 0 ||
	    (error = boot_address(boot, 0x88, boot->config.aperture_end)) != 0 ||
	    (error = boot_address(boot, 0x98, boot->config.aperture_start)) != 0 ||
	    (error = boot_address(boot, 0xa0, boot->config.aperture_end)) != 0 ||
	    (error = boot_error(boot->operations.ready(boot->owner))) != 0 ||
	    (error = boot_read(boot, 0x38, &control)) != 0) {
		return error;
	}
	if ((control & 2) != 0) {
		return EPROTO;
	}
	return boot_write(boot, 0x38, (control & ~0xff00u) | 2u << 8);
}

int
hakios_mhi_boot_step(struct hakios_mhi_boot *boot, uint64_t now_ms)
{
	if (boot == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&boot->lock);
	int error = 0;
	uint32_t status, environment, value;
	if (boot->phase == HAKIOS_MHI_BOOT_RUNTIME) {
		error = EALREADY;
		goto out;
	}
	if (boot->phase == HAKIOS_MHI_BOOT_FAULT || boot->phase == HAKIOS_MHI_BOOT_NEW) {
		error = boot->phase == HAKIOS_MHI_BOOT_FAULT ? EIO : EAGAIN;
		goto out;
	}
	if (now_ms < boot->last_time || now_ms > UINT64_MAX - boot->config.timeout_ms) {
		error = EINVAL;
		goto fault;
	}
	boot->last_time = now_ms;
	if (boot->phase != HAKIOS_MHI_BOOT_ONLINE && now_ms >= boot->deadline) {
		error = ETIMEDOUT;
		goto fault;
	}
	if ((error = boot_read(boot, 0x48, &status)) != 0 ||
	    (error = boot_read(boot, boot->bhi + 0x28, &environment)) != 0) {
		goto fault;
	}
	if ((status & 4) != 0 || ((status >> 8) & 255) == 255 || environment == 3) {
		error = EIO;
		goto fault;
	}
	if (environment > 2) {
		error = ENOTSUP;
		goto fault;
	}
	if ((environment == 2 && boot->phase < HAKIOS_MHI_BOOT_BHIE) ||
	    (environment == 0 && boot->phase >= HAKIOS_MHI_BOOT_BHIE)) {
		error = EPROTO;
		goto fault;
	}
	switch (boot->phase) {
	case HAKIOS_MHI_BOOT_BHI:
		error = boot_read(boot, boot->bhi + 0x2c, &value);
		if (error == 0 && (value >> 30) == 2) {
			boot_phase(boot, HAKIOS_MHI_BOOT_READY, now_ms);
		} else if (error == 0 && (value >> 30) != 0) {
			error = EIO;
		}
		break;
	case HAKIOS_MHI_BOOT_READY:
		error = boot_read(boot, 0x38, &value);
		if (error == 0 && (value & 2) == 0 && (status & 1) != 0) {
			error = boot_contexts(boot);
			if (error == 0) {
				boot_phase(boot, HAKIOS_MHI_BOOT_SBL, now_ms);
			}
		}
		break;
	case HAKIOS_MHI_BOOT_SBL:
		if (environment == 1 && ((status >> 8) & 255) == 2) {
			if ((error = boot_address(boot, boot->bhie + 0x2c, boot->config.vectors.iova)) == 0 &&
			    (error = boot_write(boot, boot->bhie + 0x34, boot->config.segment_count * 16)) == 0 &&
			    (error = boot_read(boot, boot->bhie + 0x3c, &value)) == 0) {
				error = boot_write(boot, boot->bhie + 0x3c, (value & 0xc0000000) | (boot->config.sequence + 1));
			}
			if (error == 0) {
				boot_phase(boot, HAKIOS_MHI_BOOT_BHIE, now_ms);
			}
		}
		break;
	case HAKIOS_MHI_BOOT_BHIE:
		error = boot_read(boot, boot->bhie + 0x44, &value);
		if (error == 0 && (value >> 30) == 2 && (value & 0x3fffffff) == boot->config.sequence + 1) {
			boot_phase(boot, HAKIOS_MHI_BOOT_AMSS, now_ms);
		} else if (error == 0 && ((value >> 30) == 3 || (value >> 30) == 1)) {
			error = EIO;
		}
		break;
	case HAKIOS_MHI_BOOT_AMSS:
		if (environment == 2 && ((status >> 8) & 255) == 2) {
			boot_phase(boot, HAKIOS_MHI_BOOT_ONLINE, now_ms);
		}
		break;
	case HAKIOS_MHI_BOOT_ONLINE:
		if (environment != 2 || ((status >> 8) & 255) != 2) {
			error = ENOTSUP;
		}
		break;
	default:
		error = EPROTO;
		break;
	}
	if (error != 0) {
		goto fault;
	}
	error = boot->phase == HAKIOS_MHI_BOOT_ONLINE ? 0 : EAGAIN;
	goto out;
fault:
	boot->phase = HAKIOS_MHI_BOOT_FAULT;
out:
	lck_mtx_unlock(&boot->lock);
	return error;
}

int
hakios_mhi_boot_status(struct hakios_mhi_boot *boot, enum hakios_mhi_boot_phase *phase)
{
	if (phase == NULL) {
		return EINVAL;
	}
	*phase = HAKIOS_MHI_BOOT_FAULT;
	if (boot == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&boot->lock);
	*phase = boot->phase;
	int error = boot->phase == HAKIOS_MHI_BOOT_FAULT ? EIO : boot->phase == HAKIOS_MHI_BOOT_RUNTIME ? EALREADY :
	    boot->phase == HAKIOS_MHI_BOOT_ONLINE ? 0 : EAGAIN;
	lck_mtx_unlock(&boot->lock);
	return error;
}

int
hakios_mhi_boot_take_runtime(struct hakios_mhi_boot *boot)
{
	if (boot == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&boot->lock);
	int error = boot->phase == HAKIOS_MHI_BOOT_ONLINE ? 0 : EBUSY;
	if (error == 0) {
		boot->phase = HAKIOS_MHI_BOOT_RUNTIME;
	}
	lck_mtx_unlock(&boot->lock);
	return error;
}

int
hakios_mhi_boot_retire_runtime(struct hakios_mhi_boot *boot)
{
	if (boot == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&boot->lock);
	int error = boot->phase == HAKIOS_MHI_BOOT_RUNTIME ? 0 : EINVAL;
	if (error == 0) {
		boot->phase = HAKIOS_MHI_BOOT_FAULT;
	}
	lck_mtx_unlock(&boot->lock);
	return error;
}

int
hakios_mhi_boot_destroy(struct hakios_mhi_boot **boot)
{
	if (boot == NULL || *boot == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_boot *state = *boot;
	lck_mtx_lock(&state->lock);
	if (state->phase == HAKIOS_MHI_BOOT_RUNTIME) {
		lck_mtx_unlock(&state->lock);
		return EBUSY;
	}
	int error = boot_error(state->operations.quiesce(state->owner));
	if (error != 0) {
		state->phase = HAKIOS_MHI_BOOT_FAULT;
		lck_mtx_unlock(&state->lock);
		return error;
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &mhi_boot_group);
	kfree_type(struct hakios_mhi_boot, state);
	*boot = NULL;
	return 0;
}

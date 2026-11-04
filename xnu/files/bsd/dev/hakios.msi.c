#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.msi.h>

#define MSI_ENABLE 1u
#define MSI_MME 0x70u
#define MSI_64BIT 0x80u
#define MSI_MASKABLE 0x100u

struct hakios_msi {
	lck_mtx_t lock;
	struct hakios_msi_config config;
	struct hakios_msi_operations operations;
	void *owner;
	uint32_t saved_low;
	uint32_t saved_high;
	uint32_t saved_mask;
	uint16_t saved_data;
	uint16_t saved_control;
	uint16_t data_offset;
	uint16_t mask_offset;
	bool is_64bit;
	bool touched;
	bool active;
	bool faulted;
};

static LCK_GRP_DECLARE(msi_lock_group, "hakios-msi");

static int
msi_error(int error)
{
	return error < 0 ? EIO : error;
}

static int
msi_read(struct hakios_msi *msi, uint16_t relative, uint8_t width, uint32_t *value)
{
	*value = 0;
	int error = msi_error(msi->operations.read(msi->owner, msi->config.capability + relative, width, value));
	if (error == 0 && width == 2 && *value > UINT16_MAX) {
		error = EIO;
	}
	return error;
}

static int
msi_write(struct hakios_msi *msi, uint16_t relative, uint8_t width, uint32_t value)
{
	return msi_error(msi->operations.write(msi->owner, msi->config.capability + relative, width, value));
}

static int
msi_verify(struct hakios_msi *msi, uint16_t relative, uint8_t width, uint32_t expected)
{
	uint32_t value;
	int error = msi_read(msi, relative, width, &value);
	return error != 0 ? error : (value == expected ? 0 : EIO);
}

static int
msi_verify_all(struct hakios_msi *msi, bool original)
{
	int error;
	uint32_t control = original ? msi->saved_control : ((msi->saved_control & ~MSI_MME) | (5u << 4) | MSI_ENABLE);
	if ((error = msi_verify(msi, 2, 2, control)) != 0 ||
	    (error = msi_verify(msi, 4, 4, original ? msi->saved_low : (uint32_t)msi->config.doorbell_iova)) != 0) {
		return error;
	}
	if (msi->is_64bit && (error = msi_verify(msi, 8, 4,
		original ? msi->saved_high : (uint32_t)(msi->config.doorbell_iova >> 32))) != 0) {
		return error;
	}
	if ((error = msi_verify(msi, msi->data_offset, 2, original ? msi->saved_data : msi->config.data)) != 0) {
		return error;
	}
	return (msi->saved_control & MSI_MASKABLE) ?
	    msi_verify(msi, msi->mask_offset, 4, original ? msi->saved_mask : 0) : 0;
}

int
hakios_msi_create(const struct hakios_msi_config *config,
    const struct hakios_msi_operations *operations, void *owner, struct hakios_msi **result)
{
	struct hakios_msi *msi;
	uint32_t header, control, data;
	int error;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || operations == NULL || owner == NULL || operations->version != HAKIOS_MSI_VERSION ||
	    operations->read == NULL || operations->write == NULL || operations->arm == NULL || operations->quiesce == NULL ||
	    config->capability < 0x40 || config->capability > 0xe8 || (config->capability & 3) != 0 ||
	    config->doorbell_iova == 0 || (config->doorbell_iova & 3) != 0 || (config->data & 31) != 0 ||
	    config->data > UINT16_MAX - (HAKIOS_MSI_VECTORS - 1)) {
		return EINVAL;
	}
	for (unsigned index = 0; index < HAKIOS_MSI_VECTORS; index++) {
		if (config->intids[index] < 32 || config->intids[index] > 1019) {
			return EINVAL;
		}
		for (unsigned prior = 0; prior < index; prior++) {
			if (config->intids[prior] == config->intids[index]) {
				return EINVAL;
			}
		}
	}
	msi = kalloc_type(struct hakios_msi, Z_WAITOK | Z_ZERO);
	if (msi == NULL) {
		return ENOMEM;
	}
	msi->config = *config;
	msi->operations = *operations;
	msi->owner = owner;
	if ((error = msi_read(msi, 0, 2, &header)) != 0 || (error = msi_read(msi, 2, 2, &control)) != 0) {
		goto fail;
	}
	if ((header & 255) != 5 || ((control >> 1) & 7) != 5 || ((control >> 4) & 7) > 5) {
		error = ENOTSUP;
		goto fail;
	}
	if (!(control & MSI_64BIT) && config->doorbell_iova > UINT32_MAX) {
		error = EOVERFLOW;
		goto fail;
	}
	if (control & MSI_ENABLE) {
		error = EBUSY;
		goto fail;
	}
	msi->saved_control = (uint16_t)control;
	msi->is_64bit = (control & MSI_64BIT) != 0;
	msi->data_offset = msi->is_64bit ? 12 : 8;
	msi->mask_offset = msi->is_64bit ? 16 : 12;
	if ((error = msi_read(msi, 4, 4, &msi->saved_low)) != 0 ||
	    (msi->is_64bit && (error = msi_read(msi, 8, 4, &msi->saved_high)) != 0) ||
	    (error = msi_read(msi, msi->data_offset, 2, &data)) != 0) {
		goto fail;
	}
	msi->saved_data = (uint16_t)data;
	if ((control & MSI_MASKABLE) && (error = msi_read(msi, msi->mask_offset, 4, &msi->saved_mask)) != 0) {
		goto fail;
	}
	lck_mtx_init(&msi->lock, &msi_lock_group, LCK_ATTR_NULL);
	*result = msi;
	return 0;
fail:
	kfree_type(struct hakios_msi, msi);
	return error;
}

int
hakios_msi_start(struct hakios_msi *msi)
{
	int error;

	if (msi == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&msi->lock);
	if (msi->faulted || msi->active) {
		error = msi->faulted ? EIO : EBUSY;
		goto out;
	}
	error = msi_verify_all(msi, true);
	if (error != 0) {
		goto fault;
	}
	msi->touched = true;
	if ((msi->saved_control & MSI_MASKABLE) && (error = msi_write(msi, msi->mask_offset, 4, UINT32_MAX)) != 0) {
		goto fault;
	}
	if ((error = msi_write(msi, 4, 4, (uint32_t)msi->config.doorbell_iova)) != 0 ||
	    (msi->is_64bit && (error = msi_write(msi, 8, 4, (uint32_t)(msi->config.doorbell_iova >> 32))) != 0) ||
	    (error = msi_write(msi, msi->data_offset, 2, msi->config.data)) != 0 ||
	    (error = msi_write(msi, 2, 2, (msi->saved_control & ~MSI_MME) | (5u << 4) | MSI_ENABLE)) != 0) {
		goto fault;
	}
	if ((msi->saved_control & MSI_MASKABLE) && (error = msi_write(msi, msi->mask_offset, 4, 0)) != 0) {
		goto fault;
	}
	if ((error = msi_verify_all(msi, false)) != 0 || (error = msi_error(msi->operations.arm(msi->owner))) != 0) {
		goto fault;
	}
	msi->active = true;
	goto out;
fault:
	msi->faulted = true;
out:
	lck_mtx_unlock(&msi->lock);
	return error;
}

int
hakios_msi_message(struct hakios_msi *msi, enum hakios_msi_user user,
    uint32_t index, struct hakios_msi_message *message)
{
	static const uint32_t starts[] = {0, 3, 13, 14};
	static const uint32_t counts[] = {3, 10, 1, 18};
	int error;

	if (message == NULL) {
		return EINVAL;
	}
	bzero(message, sizeof(*message));
	if (msi == NULL || (unsigned)user >= 4 || index >= counts[user]) {
		return EINVAL;
	}
	lck_mtx_lock(&msi->lock);
	error = msi->faulted ? EIO : (msi->active ? 0 : ENXIO);
	if (error == 0) {
		uint32_t vector = starts[user] + index;
		message->address = msi->config.doorbell_iova;
		message->data = msi->config.data + vector;
		message->intid = msi->config.intids[vector];
	}
	lck_mtx_unlock(&msi->lock);
	return error;
}

int
hakios_msi_destroy(struct hakios_msi **msi)
{
	struct hakios_msi *state;
	int error = 0;

	if (msi == NULL || *msi == NULL) {
		return EINVAL;
	}
	state = *msi;
	lck_mtx_lock(&state->lock);
	if (state->touched || state->faulted) {
		if ((error = msi_error(state->operations.quiesce(state->owner))) != 0 ||
		    (error = msi_write(state, 2, 2, state->saved_control)) != 0 ||
		    (error = msi_verify(state, 2, 2, state->saved_control)) != 0 ||
		    (error = msi_write(state, 4, 4, state->saved_low)) != 0 ||
			    (state->is_64bit && (error = msi_write(state, 8, 4, state->saved_high)) != 0) ||
			    (error = msi_write(state, state->data_offset, 2, state->saved_data)) != 0) {
			goto fault;
		}
		if ((state->saved_control & MSI_MASKABLE) &&
		    (error = msi_write(state, state->mask_offset, 4, state->saved_mask)) != 0) {
			goto fault;
		}
		if ((error = msi_verify_all(state, true)) != 0) {
			goto fault;
		}
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &msi_lock_group);
	kfree_type(struct hakios_msi, state);
	*msi = NULL;
	return 0;
fault:
	state->faulted = true;
	lck_mtx_unlock(&state->lock);
	return error;
}

#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <stdbool.h>
#include <sys/errno.h>
#include <sys/hakios.pcie.kona.h>

#define KONA_RC0_DBI_ADDRESS 0x60000000ull
#define KONA_RC0_DBI_SIZE 0xf1dull
#define KONA_RC0_IATU_ADDRESS 0x60001000ull
#define KONA_RC0_IATU_SIZE 0x1000ull
#define KONA_RC0_CONFIG_ADDRESS 0x60100000ull
#define KONA_RC0_CONFIG_SIZE 0x100000ull
#define KONA_RC0_REQUESTER_ID 0x0100u
#define KONA_CONFIG_SIZE 0x1000u
#define KONA_LINK_STATUS 0x80u
#define KONA_LINK_ACTIVE (1u << 29)
#define KONA_IATU_CTRL2_INDEX 1u
#define KONA_IATU_CTRL2_OFFSET 0x04u
#define KONA_IATU_ENABLE (1u << 31)
#define KONA_IATU_CFG0 4u
#define KONA_IATU_TARGET 0x01000000u
#define KONA_IATU_REGISTERS 7u
#define KONA_IATU_POLL_ATTEMPTS 100u
#define KONA_IATU_POLL_DELAY_US 100u

static LCK_GRP_DECLARE(kona_lock_group, "hakios-pcie-kona");

struct hakios_pcie_kona {
	lck_mtx_t lock;
	struct hakios_pcie_kona_operations operations;
	void *context;
	void *guard;
	uint32_t saved_region[KONA_IATU_REGISTERS];
	bool restore_pending;
	bool config_pending;
	bool faulted;
	bool ownership_unknown;
};

struct hakios_pcie_kona_cold_owner {
	lck_mtx_t lock;
	struct hakios_pcie_kona_cold_owner_operations operations;
	void *context;
	void *ownership;
	void *stage_cookie[HAKIOS_PCIE_KONA_COLD_STAGE_COUNT];
	uint32_t active_stages;
	uint32_t uncertain_stage;
	bool ownership_unknown;
	bool active;
	bool faulted;
};

static const uint32_t kona_config_region[KONA_IATU_REGISTERS] = {
	KONA_IATU_CFG0,
	KONA_IATU_ENABLE,
	(uint32_t)KONA_RC0_CONFIG_ADDRESS,
	0,
	(uint32_t)KONA_RC0_CONFIG_ADDRESS + KONA_CONFIG_SIZE - 1,
	KONA_IATU_TARGET,
	0,
};

static int
kona_error(int error)
{
	return error < 0 ? EIO : error;
}

static bool
kona_value_fits(uint8_t width, uint32_t value)
{
	return (width == 1 && value <= 0xffu) || (width == 2 && value <= 0xffffu) || width == 4;
}

static int
kona_read_locked(struct hakios_pcie_kona *controller, enum hakios_pcie_kona_space space,
    uint32_t offset, uint8_t width, uint32_t *value)
{
	uint32_t result = 0;
	int error;

	*value = 0;
	error = kona_error(controller->operations.read(controller->context, controller->guard,
	    space, offset, width, &result));
	if (error == 0 && !kona_value_fits(width, result)) {
		error = EIO;
	}
	if (error == 0) {
		*value = result;
	}
	return error;
}

static int
kona_write_locked(struct hakios_pcie_kona *controller, enum hakios_pcie_kona_space space,
    uint32_t offset, uint8_t width, uint32_t value)
{
	return kona_error(controller->operations.write(controller->context, controller->guard,
	           space, offset, width, value));
}

static int
kona_link_locked(struct hakios_pcie_kona *controller)
{
	uint32_t status;
	int error = kona_read_locked(controller, HAKIOS_PCIE_KONA_DBI, KONA_LINK_STATUS, 4, &status);

	if (error != 0) {
		return error;
	}
	if (status == UINT32_MAX) {
		return EIO;
	}
	return (status & KONA_LINK_ACTIVE) != 0 ? 0 : ENXIO;
}

static int
kona_wait_region_locked(struct hakios_pcie_kona *controller, uint32_t control)
{
	uint32_t attempt;
	uint32_t value;
	int error;

	for (attempt = 0; attempt < KONA_IATU_POLL_ATTEMPTS; attempt++) {
		error = kona_read_locked(controller, HAKIOS_PCIE_KONA_IATU, KONA_IATU_CTRL2_OFFSET, 4, &value);
		if (error != 0) {
			return error;
		}
		if ((value & KONA_IATU_ENABLE) == (control & KONA_IATU_ENABLE)) {
			return 0;
		}
		if (attempt + 1 < KONA_IATU_POLL_ATTEMPTS) {
			error = kona_error(controller->operations.delay_us(controller->context, controller->guard,
			    KONA_IATU_POLL_DELAY_US));
			if (error != 0) {
				return error;
			}
		}
	}
	return ETIMEDOUT;
}

static int
kona_save_region_locked(struct hakios_pcie_kona *controller)
{
	uint32_t index;
	int error;

	for (index = 0; index < KONA_IATU_REGISTERS; index++) {
		error = kona_read_locked(controller, HAKIOS_PCIE_KONA_IATU, index * 4, 4,
		    &controller->saved_region[index]);
		if (error != 0) {
			return error;
		}
	}
	if (controller->saved_region[0] == UINT32_MAX ||
	    controller->saved_region[KONA_IATU_CTRL2_INDEX] == UINT32_MAX) {
		return EIO;
	}
	return 0;
}

static int
kona_program_region_locked(struct hakios_pcie_kona *controller, const uint32_t *registers)
{
	uint32_t index;
	uint32_t value;
	int error;

	error = kona_write_locked(controller, HAKIOS_PCIE_KONA_IATU, KONA_IATU_CTRL2_OFFSET, 4, 0);
	if (error != 0) {
		return error;
	}
	error = kona_wait_region_locked(controller, 0);
	if (error != 0) {
		return error;
	}
	for (index = 0; index < KONA_IATU_REGISTERS; index++) {
		if (index == KONA_IATU_CTRL2_INDEX) {
			continue;
		}
		error = kona_write_locked(controller, HAKIOS_PCIE_KONA_IATU, index * 4, 4, registers[index]);
		if (error != 0) {
			return error;
		}
	}
	error = kona_write_locked(controller, HAKIOS_PCIE_KONA_IATU, KONA_IATU_CTRL2_OFFSET, 4,
	    registers[KONA_IATU_CTRL2_INDEX]);
	if (error != 0) {
		return error;
	}
	error = kona_wait_region_locked(controller, registers[KONA_IATU_CTRL2_INDEX]);
	if (error != 0) {
		return error;
	}
	for (index = 0; index < KONA_IATU_REGISTERS; index++) {
		error = kona_read_locked(controller, HAKIOS_PCIE_KONA_IATU, index * 4, 4, &value);
		if (error != 0) {
			return error;
		}
		if (value != registers[index]) {
			return EIO;
		}
	}
	return 0;
}

static int
kona_cleanup_locked(struct hakios_pcie_kona *controller)
{
	int error;

	if (controller->ownership_unknown) {
		return EIO;
	}
	if (controller->config_pending) {
		error = kona_error(controller->operations.config_drain(controller->context, controller->guard));
		if (error != 0) {
			return error;
		}
		controller->config_pending = false;
	}
	if (controller->restore_pending) {
		error = kona_program_region_locked(controller, controller->saved_region);
		if (error != 0) {
			return error;
		}
		controller->restore_pending = false;
	}
	if (controller->guard != NULL) {
		error = kona_error(controller->operations.end(controller->context, controller->guard));
		if (error != 0) {
			return error;
		}
		controller->guard = NULL;
	}
	return 0;
}

static int
kona_config_transfer(struct hakios_pcie_kona *controller, uint16_t requester_id,
    uint16_t offset, uint8_t width, bool write, uint32_t *value)
{
	uint32_t result = *value;
	int error;
	int cleanup_error;

	if (controller == NULL || (width != 1 && width != 2 && width != 4) ||
	    (offset & (width - 1)) != 0 || (uint32_t)offset + width > KONA_CONFIG_SIZE ||
	    (write && !kona_value_fits(width, result))) {
		return EINVAL;
	}
	if (requester_id != KONA_RC0_REQUESTER_ID) {
		return ENODEV;
	}
	lck_mtx_lock(&controller->lock);
	if (controller->faulted) {
		error = EIO;
		goto out;
	}
	error = kona_error(controller->operations.begin(controller->context, &controller->guard));
	if (error == 0 && controller->guard == NULL) {
		controller->ownership_unknown = true;
		error = EIO;
	}
	if (error != 0) {
		goto cleanup;
	}
	error = kona_link_locked(controller);
	if (error != 0) {
		goto cleanup;
	}
	error = kona_save_region_locked(controller);
	if (error != 0) {
		goto cleanup;
	}
	controller->restore_pending = true;
	error = kona_program_region_locked(controller, kona_config_region);
	if (error != 0) {
		goto cleanup;
	}
	error = kona_link_locked(controller);
	if (error != 0) {
		goto cleanup;
	}
	controller->config_pending = true;
	if (write) {
		error = kona_write_locked(controller, HAKIOS_PCIE_KONA_CONFIG, offset, width, result);
	} else {
		error = kona_read_locked(controller, HAKIOS_PCIE_KONA_CONFIG, offset, width, &result);
	}

cleanup:
	cleanup_error = kona_cleanup_locked(controller);
	if (cleanup_error != 0) {
		error = cleanup_error;
	}
	if (error != 0) {
		controller->faulted = true;
	} else if (!write) {
		*value = result;
	}
out:
	lck_mtx_unlock(&controller->lock);
	return error;
}

int
hakios_pcie_kona_create(const struct hakios_pcie_kona_operations *operations, void *context,
    const struct hakios_pcie_kona_resources *resources, struct hakios_pcie_kona **result)
{
	struct hakios_pcie_kona *controller;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (operations == NULL || context == NULL || resources == NULL ||
	    operations->version != HAKIOS_PCIE_KONA_OPERATIONS_VERSION ||
	    operations->begin == NULL || operations->end == NULL || operations->read == NULL ||
	    operations->write == NULL || operations->config_drain == NULL || operations->delay_us == NULL) {
		return EINVAL;
	}
	if (resources->dbi.address != KONA_RC0_DBI_ADDRESS || resources->dbi.size != KONA_RC0_DBI_SIZE ||
	    resources->iatu.address != KONA_RC0_IATU_ADDRESS || resources->iatu.size != KONA_RC0_IATU_SIZE ||
	    resources->config.address != KONA_RC0_CONFIG_ADDRESS || resources->config.size != KONA_RC0_CONFIG_SIZE) {
		return ENOTSUP;
	}
	controller = kalloc_type(struct hakios_pcie_kona, Z_WAITOK | Z_ZERO);
	if (controller == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&controller->lock, &kona_lock_group, LCK_ATTR_NULL);
	controller->operations = *operations;
	controller->context = context;
	*result = controller;
	return 0;
}

int
hakios_pcie_kona_destroy(struct hakios_pcie_kona **controller)
{
	struct hakios_pcie_kona *state;

	if (controller == NULL || *controller == NULL) {
		return EINVAL;
	}
	state = *controller;
	lck_mtx_lock(&state->lock);
	if (state->guard != NULL || state->restore_pending || state->config_pending || state->ownership_unknown) {
		lck_mtx_unlock(&state->lock);
		return EBUSY;
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &kona_lock_group);
	kfree_type(struct hakios_pcie_kona, state);
	*controller = NULL;
	return 0;
}

int
hakios_pcie_kona_recover(struct hakios_pcie_kona *controller)
{
	int error;

	if (controller == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&controller->lock);
	error = kona_cleanup_locked(controller);
	if (error != 0) {
		controller->faulted = true;
	}
	lck_mtx_unlock(&controller->lock);
	return error;
}

int
hakios_pcie_kona_config_read(struct hakios_pcie_kona *controller, uint16_t requester_id,
    uint16_t offset, uint8_t width, uint32_t *value)
{
	if (value == NULL) {
		return EINVAL;
	}
	*value = 0;
	return kona_config_transfer(controller, requester_id, offset, width, false, value);
}

int
hakios_pcie_kona_config_write(struct hakios_pcie_kona *controller, uint16_t requester_id,
    uint16_t offset, uint8_t width, uint32_t value)
{
	return kona_config_transfer(controller, requester_id, offset, width, true, &value);
}

static int
kona_cold_owner_revoke_locked(struct hakios_pcie_kona_cold_owner *owner)
{
	int error;

	if (owner->ownership == NULL) {
		owner->active = false;
		owner->faulted = false;
		owner->ownership_unknown = false;
		return 0;
	}
	error = kona_error(owner->operations.revoke(owner->context, &owner->ownership));
	if (error != 0 || owner->ownership != NULL) {
		owner->faulted = true;
		if (owner->ownership == NULL) {
			owner->ownership_unknown = true;
		}
		return error != 0 ? error : EPROTO;
	}
	owner->active = false;
	owner->faulted = false;
	owner->ownership_unknown = false;
	return 0;
}

static int
kona_cold_owner_unwind_locked(struct hakios_pcie_kona_cold_owner *owner)
{
	uint32_t stage;
	int error;

	if (owner->ownership_unknown) {
		owner->faulted = true;
		return EIO;
	}
	while (owner->active_stages != 0) {
		stage = owner->active_stages - 1;
		void *cookie = owner->stage_cookie[stage];
		error = kona_error(owner->operations.stage(owner->context, owner->ownership,
		    (enum hakios_pcie_kona_cold_stage)stage, false, &cookie));
		if (error != 0 || cookie != NULL) {
			owner->faulted = true;
			return error != 0 ? error : EPROTO;
		}
		owner->stage_cookie[stage] = NULL;
		owner->active_stages--;
	}
	return kona_cold_owner_revoke_locked(owner);
}

int
hakios_pcie_kona_cold_owner_create(const struct hakios_pcie_kona_cold_owner_operations *operations,
    void *context, struct hakios_pcie_kona_cold_owner **result)
{
	struct hakios_pcie_kona_cold_owner *owner;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (operations == NULL || context == NULL ||
	    operations->version != HAKIOS_PCIE_KONA_COLD_OWNER_VERSION ||
	    operations->authorize == NULL || operations->revoke == NULL || operations->stage == NULL) {
		return EINVAL;
	}
	owner = kalloc_type(struct hakios_pcie_kona_cold_owner, Z_WAITOK | Z_ZERO);
	if (owner == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&owner->lock, &kona_lock_group, LCK_ATTR_NULL);
	owner->operations = *operations;
	owner->context = context;
	*result = owner;
	return 0;
}

int
hakios_pcie_kona_cold_owner_acquire(struct hakios_pcie_kona_cold_owner *owner)
{
	uint32_t stage;
	int error;
	int cleanup_error;

	if (owner == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&owner->lock);
	if (owner->faulted || owner->ownership != NULL || owner->active || owner->active_stages != 0) {
		error = owner->faulted ? EIO : EBUSY;
		goto out;
	}
	error = kona_error(owner->operations.authorize(owner->context, &owner->ownership));
	if (error != 0 || owner->ownership == NULL) {
		if (owner->ownership == NULL) {
			if (error == 0) {
				owner->ownership_unknown = true;
				owner->faulted = true;
				error = EPROTO;
			}
			goto out;
		}
		cleanup_error = kona_cold_owner_revoke_locked(owner);
		if (cleanup_error != 0) {
			error = cleanup_error;
		}
		goto out;
	}
	for (stage = 0; stage < HAKIOS_PCIE_KONA_COLD_STAGE_COUNT; stage++) {
		error = kona_error(owner->operations.stage(owner->context, owner->ownership,
		    (enum hakios_pcie_kona_cold_stage)stage, true, &owner->stage_cookie[stage]));
		if (error == 0 && owner->stage_cookie[stage] == NULL) {
			owner->ownership_unknown = true;
			owner->faulted = true;
			error = EPROTO;
			goto out;
		}
		if (error != 0) {
			if (owner->stage_cookie[stage] != NULL) {
				owner->active_stages = stage + 1;
			}
			cleanup_error = kona_cold_owner_unwind_locked(owner);
			if (cleanup_error != 0) {
				error = cleanup_error;
			}
			goto out;
		}
		owner->active_stages = stage + 1;
	}
	owner->active = true;
out:
	lck_mtx_unlock(&owner->lock);
	return error;
}

int
hakios_pcie_kona_cold_owner_release(struct hakios_pcie_kona_cold_owner *owner)
{
	int error;

	if (owner == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&owner->lock);
	if (owner->faulted) {
		error = EIO;
	} else if (!owner->active) {
		error = ENXIO;
	} else {
		error = kona_cold_owner_unwind_locked(owner);
	}
	lck_mtx_unlock(&owner->lock);
	return error;
}

int
hakios_pcie_kona_cold_owner_recover(struct hakios_pcie_kona_cold_owner *owner)
{
	int error;

	if (owner == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&owner->lock);
	if (owner->ownership == NULL && owner->active_stages == 0 && !owner->ownership_unknown) {
		error = 0;
	} else {
		error = kona_cold_owner_unwind_locked(owner);
	}
	lck_mtx_unlock(&owner->lock);
	return error;
}

int
hakios_pcie_kona_cold_owner_destroy(struct hakios_pcie_kona_cold_owner **owner)
{
	struct hakios_pcie_kona_cold_owner *state;

	if (owner == NULL || *owner == NULL) {
		return EINVAL;
	}
	state = *owner;
	lck_mtx_lock(&state->lock);
	if (state->ownership != NULL || state->active || state->active_stages != 0 ||
	    state->ownership_unknown || state->faulted) {
		lck_mtx_unlock(&state->lock);
		return EBUSY;
	}
	lck_mtx_unlock(&state->lock);
	lck_mtx_destroy(&state->lock, &kona_lock_group);
	kfree_type(struct hakios_pcie_kona_cold_owner, state);
	*owner = NULL;
	return 0;
}

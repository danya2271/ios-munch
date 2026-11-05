#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.pcie.h>

#define PCIE_CONFIG_SIZE 0x1000u
#define PCIE_COMMAND 0x04u
#define PCIE_COMMAND_MEMORY (1u << 1)
#define PCIE_COMMAND_MASTER (1u << 2)

static LCK_GRP_DECLARE(pcie_lock_group, "hakios-pcie");
static LCK_MTX_DECLARE(pcie_lock, &pcie_lock_group);

static struct hakios_pcie_rc0_provider pcie_provider;
static struct hakios_pcie_endpoint_resources pcie_resources;
static void *pcie_context;
static void *pcie_owner;
static hakios_pcie_lease_t pcie_lease;
static hakios_pcie_lease_t pcie_generation;
static uint32_t pcie_dependents;
static bool pcie_registered;
static bool pcie_faulted;

static int
pcie_error(int error)
{
	return error < 0 ? EIO : error;
}

static int
pcie_status_locked(hakios_pcie_lease_t lease)
{
	if (lease == 0 || lease != pcie_lease || pcie_owner == NULL) {
		return ESTALE;
	}
	return pcie_faulted ? EIO : 0;
}

static int
pcie_config_read_locked(uint16_t offset, uint8_t width, uint32_t *value)
{
	int error;

	*value = 0;
	error = pcie_error(pcie_provider.config_read(pcie_context, pcie_owner, offset, width, value));
	if (error == 0 && ((width == 1 && *value > 0xff) || (width == 2 && *value > 0xffff))) {
		error = EIO;
	}
	if (error != 0) {
		*value = 0;
		pcie_faulted = true;
	}
	return error;
}

static int
pcie_config_write_locked(uint16_t offset, uint8_t width, uint32_t value)
{
	int error;

	error = pcie_error(pcie_provider.config_write(pcie_context, pcie_owner, offset, width, value));
	if (error != 0) {
		pcie_faulted = true;
	}
	return error;
}

static int
pcie_release_locked(void)
{
	int error = pcie_error(pcie_provider.release(pcie_context, pcie_owner));

	if (error != 0) {
		pcie_faulted = true;
		return error;
	}
	pcie_owner = NULL;
	pcie_lease = 0;
	pcie_faulted = false;
	bzero(&pcie_resources, sizeof(pcie_resources));
	return 0;
}

int
hakios_pcie_rc0_register(const struct hakios_pcie_rc0_provider *provider, void *context)
{
	int error = 0;

	if (provider == NULL || context == NULL || provider->version != HAKIOS_PCIE_PROVIDER_VERSION ||
	    provider->acquire == NULL || provider->release == NULL || provider->config_read == NULL ||
	    provider->config_write == NULL || provider->access_begin == NULL || provider->access_end == NULL ||
	    provider->bar0_read32 == NULL || provider->bar0_write32 == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&pcie_lock);
	if (pcie_registered) {
		error = EBUSY;
	} else {
		pcie_provider = *provider;
		pcie_context = context;
		pcie_registered = true;
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_rc0_unregister(void *context)
{
	int error = 0;

	lck_mtx_lock(&pcie_lock);
	if (!pcie_registered || context != pcie_context) {
		error = ENODEV;
	} else if (pcie_owner != NULL || pcie_faulted) {
		error = EBUSY;
	} else {
		bzero(&pcie_provider, sizeof(pcie_provider));
		pcie_context = NULL;
		pcie_registered = false;
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_rc0_recover(void *context)
{
	int error;

	lck_mtx_lock(&pcie_lock);
	if (!pcie_registered || context != pcie_context) {
		error = ENODEV;
	} else if (pcie_lease != 0) {
		error = EBUSY;
	} else if (pcie_owner != NULL) {
		error = pcie_release_locked();
	} else {
		error = pcie_faulted ? EIO : 0;
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

static int
pcie_validate_locked(const struct hakios_pcie_endpoint_request *request)
{
	uint32_t identity, class_revision, bar0, upper = 0, command;
	int error;

	if (pcie_resources.domain != request->domain || pcie_resources.requester_id != request->requester_id ||
	    pcie_resources.stream_id != request->stream_id || pcie_resources.bar0_address != request->bar0_address ||
	    pcie_resources.bar0_size != request->bar0_size) {
		return ENODEV;
	}
	if ((error = pcie_config_read_locked(0x00, 4, &identity)) != 0 ||
	    (error = pcie_config_read_locked(0x08, 4, &class_revision)) != 0 ||
	    (error = pcie_config_read_locked(0x10, 4, &bar0)) != 0 ||
	    (error = pcie_config_read_locked(PCIE_COMMAND, 2, &command)) != 0) {
		return error;
	}
	if ((identity & 0xffff) != request->vendor || (identity >> 16) != request->device ||
	    (class_revision & 0xff) != request->revision || (class_revision >> 8) != request->class_code ||
	    (bar0 & 1) != 0 || ((bar0 & 6) != 0 && (bar0 & 6) != 4)) {
		return ENODEV;
	}
	if ((bar0 & 6) == 4 && (error = pcie_config_read_locked(0x14, 4, &upper)) != 0) {
		return error;
	}
	if ((((uint64_t)upper << 32) | (bar0 & 0xfffffff0u)) != request->bar0_address) {
		return ENODEV;
	}
	if ((command & PCIE_COMMAND_MASTER) != 0) {
		return EBUSY;
	}
	if ((command & PCIE_COMMAND_MEMORY) == 0) {
		error = pcie_error(pcie_provider.config_write(pcie_context, pcie_owner,
		    PCIE_COMMAND, 2, command | PCIE_COMMAND_MEMORY));
		if (error != 0) {
			pcie_faulted = true;
			return error;
		}
	}
	if ((error = pcie_config_read_locked(PCIE_COMMAND, 2, &command)) != 0) {
		return error;
	}
	return (command & (PCIE_COMMAND_MASTER | PCIE_COMMAND_MEMORY)) == PCIE_COMMAND_MEMORY ? 0 : EIO;
}

int
hakios_pcie_endpoint_acquire(const struct hakios_pcie_endpoint_request *request, hakios_pcie_lease_t *lease)
{
	int error, release_error;

	if (lease == NULL) {
		return EINVAL;
	}
	*lease = 0;
	if (request == NULL || request->domain != 0 || request->requester_id == 0 ||
	    request->class_code > 0xffffff || request->bar0_size < 4 || request->bar0_address == 0 ||
	    (request->bar0_size & (request->bar0_size - 1)) != 0 ||
	    (request->bar0_address & (request->bar0_size - 1)) != 0 ||
	    request->bar0_address > UINT64_MAX - (request->bar0_size - 1)) {
		return EINVAL;
	}
	lck_mtx_lock(&pcie_lock);
	if (!pcie_registered) {
		error = ENOTSUP;
		goto out;
	}
	if (pcie_faulted || pcie_owner != NULL) {
		error = pcie_faulted ? EIO : EBUSY;
		goto out;
	}
	if (pcie_generation == UINT64_MAX) {
		error = EOVERFLOW;
		goto out;
	}
	bzero(&pcie_resources, sizeof(pcie_resources));
	error = pcie_error(pcie_provider.acquire(pcie_context, request, &pcie_resources, &pcie_owner));
	if (pcie_owner == NULL) {
		if (error == 0) {
			pcie_faulted = true;
			error = EIO;
		}
		goto out;
	}
	if (error == 0) {
		error = pcie_validate_locked(request);
	}
	if (error != 0) {
		release_error = pcie_release_locked();
		if (release_error != 0) {
			error = release_error;
		}
		goto out;
	}
	pcie_lease = ++pcie_generation;
	*lease = pcie_lease;
out:
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_endpoint_release(hakios_pcie_lease_t lease)
{
	int error;

	lck_mtx_lock(&pcie_lock);
	if (lease == 0 || lease != pcie_lease || pcie_owner == NULL) {
		error = ESTALE;
	} else if (pcie_dependents != 0) {
		error = EBUSY;
	} else {
		error = pcie_release_locked();
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_endpoint_hold(hakios_pcie_lease_t lease)
{
	lck_mtx_lock(&pcie_lock);
	int error = pcie_status_locked(lease);
	if (error == 0) {
		if (pcie_dependents == UINT32_MAX) {
			error = EOVERFLOW;
		} else {
			pcie_dependents++;
		}
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_endpoint_drop(hakios_pcie_lease_t lease)
{
	lck_mtx_lock(&pcie_lock);
	int error = lease == 0 || lease != pcie_lease || pcie_owner == NULL ? ESTALE : pcie_dependents == 0 ? EINVAL : 0;
	if (error == 0) {
		pcie_dependents--;
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_endpoint_status(hakios_pcie_lease_t lease)
{
	int error;

	lck_mtx_lock(&pcie_lock);
	error = pcie_status_locked(lease);
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_endpoint_resources(hakios_pcie_lease_t lease, struct hakios_pcie_endpoint_resources *resources)
{
	if (resources == NULL) {
		return EINVAL;
	}
	bzero(resources, sizeof(*resources));
	lck_mtx_lock(&pcie_lock);
	int error = pcie_status_locked(lease);
	if (error == 0) {
		*resources = pcie_resources;
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_config_read(hakios_pcie_lease_t lease, uint16_t offset, uint8_t width, uint32_t *value)
{
	int error;

	if (value == NULL) {
		return EINVAL;
	}
	*value = 0;
	if ((width != 1 && width != 2 && width != 4) || (offset & (width - 1)) != 0 ||
	    offset > PCIE_CONFIG_SIZE - width) {
		return EINVAL;
	}
	lck_mtx_lock(&pcie_lock);
	if ((error = pcie_status_locked(lease)) == 0) {
		error = pcie_config_read_locked(offset, width, value);
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_config_write(hakios_pcie_lease_t lease, uint16_t offset, uint8_t width, uint32_t value)
{
	int error;

	if ((width != 1 && width != 2 && width != 4) || (offset & (width - 1)) != 0 ||
	    offset < 0x40 || offset > 0x100 - width ||
	    (width == 1 && value > UINT8_MAX) || (width == 2 && value > UINT16_MAX)) {
		return EINVAL;
	}
	lck_mtx_lock(&pcie_lock);
	if ((error = pcie_status_locked(lease)) == 0) {
		error = pcie_config_write_locked(offset, width, value);
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

int
hakios_pcie_bar0_transfer(hakios_pcie_lease_t lease, struct hakios_pcie_bar_operation *operations,
    uint32_t count, bool wake_required)
{
	int error, end_error;

	if (operations == NULL || count == 0 || count > HAKIOS_PCIE_BAR_OPERATIONS_MAX) {
		return EINVAL;
	}
	for (uint32_t index = 0; index < count; index++) {
		if (operations[index].type == HAKIOS_PCIE_BAR_READ32) {
			operations[index].value = 0;
		}
	}
	lck_mtx_lock(&pcie_lock);
	if ((error = pcie_status_locked(lease)) != 0) {
		goto out;
	}
	for (uint32_t index = 0; index < count; index++) {
		const struct hakios_pcie_bar_operation *operation = &operations[index];

		if ((operation->offset & 3) != 0 || operation->offset > pcie_resources.bar0_size - 4 ||
		    (operation->type != HAKIOS_PCIE_BAR_READ32 && operation->type != HAKIOS_PCIE_BAR_WRITE32 &&
		    operation->type != HAKIOS_PCIE_BAR_VERIFY32) ||
		    (operation->type == HAKIOS_PCIE_BAR_VERIFY32 ? operation->mask == 0 : operation->mask != 0)) {
			error = EINVAL;
			goto out;
		}
	}
	error = pcie_error(pcie_provider.access_begin(pcie_context, pcie_owner, wake_required));
	if (error != 0) {
		if (error != ENOTSUP) {
			pcie_faulted = true;
		}
		goto out;
	}
	for (uint32_t index = 0; index < count; index++) {
		struct hakios_pcie_bar_operation *operation = &operations[index];
		uint32_t observed = 0;

		if (operation->type == HAKIOS_PCIE_BAR_WRITE32) {
			error = pcie_error(pcie_provider.bar0_write32(pcie_context, pcie_owner, operation->offset, operation->value));
		} else {
			error = pcie_error(pcie_provider.bar0_read32(pcie_context, pcie_owner, operation->offset, &observed));
			if (error == 0 && operation->type == HAKIOS_PCIE_BAR_VERIFY32 &&
			    (observed & operation->mask) != (operation->value & operation->mask)) {
				error = EIO;
			}
			if (error == 0 && operation->type == HAKIOS_PCIE_BAR_READ32) {
				operation->value = observed;
			}
		}
		if (error != 0) {
			pcie_faulted = true;
			break;
		}
	}
	end_error = pcie_error(pcie_provider.access_end(pcie_context, pcie_owner));
	if (end_error != 0) {
		pcie_faulted = true;
		if (error == 0) {
			error = end_error;
		}
	}
out:
	if (error != 0) {
		for (uint32_t index = 0; index < count; index++) {
			if (operations[index].type == HAKIOS_PCIE_BAR_READ32) {
				operations[index].value = 0;
			}
		}
	}
	lck_mtx_unlock(&pcie_lock);
	return error;
}

#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.pcie.h>
#include <sys/hakios.pcie.kona.platform.h>

#define KONA_RC0_DBI_ADDRESS 0x60000000ull
#define KONA_RC0_DBI_SIZE 0xf1dull
#define KONA_RC0_IATU_ADDRESS 0x60001000ull
#define KONA_RC0_IATU_SIZE 0x1000ull
#define KONA_RC0_CONFIG_ADDRESS 0x60100000ull
#define KONA_RC0_CONFIG_SIZE 0x100000ull

#define KONA_QCA_DOMAIN 0u
#define KONA_QCA_REQUESTER_ID 0x0100u
#define KONA_QCA_STREAM_ID 0x1c01u
#define KONA_QCA_VENDOR 0x17cbu
#define KONA_QCA_DEVICE 0x1101u
#define KONA_QCA_CLASS 0x028000u
#define KONA_QCA_REVISION 0x01u
#define KONA_QCA_BAR0_ADDRESS 0x60300000ull
#define KONA_QCA_BAR0_SIZE 0x00100000ull

static LCK_GRP_DECLARE(kona_platform_lock_group, "hakios-pcie-kona-platform");

struct hakios_pcie_kona_platform {
	lck_mtx_t lock;
	struct hakios_pcie_kona_platform_operations operations;
	void *cold_context;
	void *config_context;
	void *bar_context;
	struct hakios_pcie_kona_cold_owner *cold;
	struct hakios_pcie_kona *config;
	bool active;
	bool registered;
};

static int
kona_platform_error(int error)
{
	return error < 0 ? EIO : error;
}

static bool
kona_platform_active_owner(struct hakios_pcie_kona_platform *platform)
{
	return platform != NULL && platform->active;
}

static bool
kona_platform_owner(struct hakios_pcie_kona_platform *platform, void *owner)
{
	return kona_platform_active_owner(platform) && owner == platform;
}

static int
kona_platform_acquire(void *context, const struct hakios_pcie_endpoint_request *request,
    struct hakios_pcie_endpoint_resources *resources, void **owner)
{
	struct hakios_pcie_kona_platform *platform = context;

	if (!kona_platform_active_owner(platform) || request == NULL || resources == NULL || owner == NULL) {
		return ENODEV;
	}
	if (request->domain != KONA_QCA_DOMAIN || request->requester_id != KONA_QCA_REQUESTER_ID ||
	    request->stream_id != KONA_QCA_STREAM_ID || request->vendor != KONA_QCA_VENDOR ||
	    request->device != KONA_QCA_DEVICE || request->class_code != KONA_QCA_CLASS ||
	    request->revision != KONA_QCA_REVISION || request->bar0_address != KONA_QCA_BAR0_ADDRESS ||
	    request->bar0_size != KONA_QCA_BAR0_SIZE) {
		return ENODEV;
	}
	*resources = (struct hakios_pcie_endpoint_resources) {
		.domain = KONA_QCA_DOMAIN,
		.requester_id = KONA_QCA_REQUESTER_ID,
		.stream_id = KONA_QCA_STREAM_ID,
		.bar0_address = KONA_QCA_BAR0_ADDRESS,
		.bar0_size = KONA_QCA_BAR0_SIZE,
	};
	*owner = platform;
	return 0;
}

static int
kona_platform_release(void *context, void *owner)
{
	struct hakios_pcie_kona_platform *platform = context;

	if (!kona_platform_owner(platform, owner)) {
		return ESTALE;
	}
	return 0;
}

static int
kona_platform_config_read(void *context, void *owner, uint16_t offset, uint8_t width,
    uint32_t *value)
{
	struct hakios_pcie_kona_platform *platform = context;

	if (!kona_platform_owner(platform, owner)) {
		return ESTALE;
	}
	return hakios_pcie_kona_config_read(platform->config, KONA_QCA_REQUESTER_ID, offset, width, value);
}

static int
kona_platform_config_write(void *context, void *owner, uint16_t offset, uint8_t width,
    uint32_t value)
{
	struct hakios_pcie_kona_platform *platform = context;

	if (!kona_platform_owner(platform, owner)) {
		return ESTALE;
	}
	return hakios_pcie_kona_config_write(platform->config, KONA_QCA_REQUESTER_ID, offset, width, value);
}

static int
kona_platform_access_begin(void *context, void *owner, bool wake_required)
{
	struct hakios_pcie_kona_platform *platform = context;

	if (!kona_platform_owner(platform, owner)) {
		return ESTALE;
	}
	return kona_platform_error(platform->operations.bar_begin(platform->bar_context, owner, wake_required));
}

static int
kona_platform_access_end(void *context, void *owner)
{
	struct hakios_pcie_kona_platform *platform = context;

	if (!kona_platform_owner(platform, owner)) {
		return ESTALE;
	}
	return kona_platform_error(platform->operations.bar_end(platform->bar_context, owner));
}

static int
kona_platform_bar_read32(void *context, void *owner, uint32_t offset, uint32_t *value)
{
	struct hakios_pcie_kona_platform *platform = context;

	if (!kona_platform_owner(platform, owner)) {
		return ESTALE;
	}
	return kona_platform_error(platform->operations.bar_read32(platform->bar_context, owner, offset, value));
}

static int
kona_platform_bar_write32(void *context, void *owner, uint32_t offset, uint32_t value)
{
	struct hakios_pcie_kona_platform *platform = context;

	if (!kona_platform_owner(platform, owner)) {
		return ESTALE;
	}
	return kona_platform_error(platform->operations.bar_write32(platform->bar_context, owner, offset, value));
}

static const struct hakios_pcie_rc0_provider kona_platform_provider = {
	.version = HAKIOS_PCIE_PROVIDER_VERSION,
	.acquire = kona_platform_acquire,
	.release = kona_platform_release,
	.config_read = kona_platform_config_read,
	.config_write = kona_platform_config_write,
	.access_begin = kona_platform_access_begin,
	.access_end = kona_platform_access_end,
	.bar0_read32 = kona_platform_bar_read32,
	.bar0_write32 = kona_platform_bar_write32,
};

static int
kona_platform_config_create(struct hakios_pcie_kona_platform *platform)
{
	const struct hakios_pcie_kona_resources resources = {
		.dbi = { KONA_RC0_DBI_ADDRESS, KONA_RC0_DBI_SIZE },
		.iatu = { KONA_RC0_IATU_ADDRESS, KONA_RC0_IATU_SIZE },
		.config = { KONA_RC0_CONFIG_ADDRESS, KONA_RC0_CONFIG_SIZE },
	};

	return hakios_pcie_kona_create(&platform->operations.config, platform->config_context,
	    &resources, &platform->config);
}

int
hakios_pcie_kona_platform_create(
    const struct hakios_pcie_kona_platform_operations *operations, void *context,
    struct hakios_pcie_kona_platform **result)
{
	struct hakios_pcie_kona_platform *platform;
	int error;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (operations == NULL || context == NULL ||
	    operations->version != HAKIOS_PCIE_KONA_PLATFORM_VERSION ||
	    operations->cold.version != HAKIOS_PCIE_KONA_COLD_OWNER_VERSION ||
	    operations->config.version != HAKIOS_PCIE_KONA_OPERATIONS_VERSION ||
	    operations->bar_begin == NULL || operations->bar_end == NULL ||
	    operations->bar_read32 == NULL || operations->bar_write32 == NULL) {
		return EINVAL;
	}
	platform = kalloc_type(struct hakios_pcie_kona_platform, Z_WAITOK | Z_ZERO);
	if (platform == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&platform->lock, &kona_platform_lock_group, LCK_ATTR_NULL);
	platform->operations = *operations;
	platform->cold_context = operations->cold_context != NULL ? operations->cold_context : context;
	platform->config_context = operations->config_context != NULL ? operations->config_context : context;
	platform->bar_context = operations->bar_context != NULL ? operations->bar_context : context;
	error = hakios_pcie_kona_cold_owner_create(&operations->cold, platform->cold_context, &platform->cold);
	if (error == 0) {
		error = kona_platform_config_create(platform);
	}
	if (error != 0) {
		(void)hakios_pcie_kona_cold_owner_destroy(&platform->cold);
		lck_mtx_destroy(&platform->lock, &kona_platform_lock_group);
		kfree_type(struct hakios_pcie_kona_platform, platform);
		return error;
	}
	*result = platform;
	return 0;
}

int
hakios_pcie_kona_platform_activate(struct hakios_pcie_kona_platform *platform)
{
	int error;

	if (platform == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&platform->lock);
	if (platform->active || platform->registered) {
		error = EBUSY;
		goto out;
	}
	if (platform->config == NULL) {
		error = kona_platform_config_create(platform);
		if (error != 0) {
			goto out;
		}
	}
	error = hakios_pcie_kona_cold_owner_acquire(platform->cold);
	if (error != 0) {
		int cleanup_error = hakios_pcie_kona_cold_owner_recover(platform->cold);
		if (cleanup_error != 0) {
			platform->active = true;
			error = cleanup_error;
		}
		goto out;
	}
	platform->active = true;
	error = hakios_pcie_rc0_register(&kona_platform_provider, platform);
	if (error != 0) {
		int cleanup_error = hakios_pcie_kona_cold_owner_recover(platform->cold);
		if (cleanup_error == 0) {
			platform->active = false;
		} else {
			error = cleanup_error;
		}
		goto out;
	}
	platform->registered = true;
out:
	lck_mtx_unlock(&platform->lock);
	return error;
}

int
hakios_pcie_kona_platform_deactivate(struct hakios_pcie_kona_platform *platform)
{
	int error;

	if (platform == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&platform->lock);
	if (!platform->registered && !platform->active) {
		error = ENXIO;
		goto out;
	}
	if (platform->registered) {
		error = hakios_pcie_rc0_unregister(platform);
		if (error != 0) {
			goto out;
		}
		platform->registered = false;
	}
	if (platform->active) {
		if (platform->config != NULL) {
			error = hakios_pcie_kona_recover(platform->config);
			if (error != 0) {
				goto out;
			}
			error = hakios_pcie_kona_destroy(&platform->config);
			if (error != 0) {
				goto out;
			}
		}
		error = hakios_pcie_kona_cold_owner_recover(platform->cold);
		if (error != 0) {
			goto out;
		}
		platform->active = false;
	}
	error = 0;
out:
	lck_mtx_unlock(&platform->lock);
	return error;
}

int
hakios_pcie_kona_platform_destroy(struct hakios_pcie_kona_platform **platform)
{
	struct hakios_pcie_kona_platform *state;
	int error;

	if (platform == NULL || *platform == NULL) {
		return EINVAL;
	}
	state = *platform;
	error = hakios_pcie_kona_platform_deactivate(state);
	if (error != ENXIO && error != 0) {
		return error;
	}
	if (state->config != NULL) {
		error = hakios_pcie_kona_destroy(&state->config);
		if (error != 0) {
			return error;
		}
	}
	error = hakios_pcie_kona_cold_owner_destroy(&state->cold);
	if (error != 0) {
		return error;
	}
	lck_mtx_destroy(&state->lock, &kona_platform_lock_group);
	kfree_type(struct hakios_pcie_kona_platform, state);
	*platform = NULL;
	return 0;
}

bool
hakios_pcie_kona_platform_active(struct hakios_pcie_kona_platform *platform)
{
	bool active;

	if (platform == NULL) {
		return false;
	}
	lck_mtx_lock(&platform->lock);
	active = platform->active && platform->registered;
	lck_mtx_unlock(&platform->lock);
	return active;
}

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/vm_param.h>
#include <sys/errno.h>
#include <sys/hakios.mmio.h>
#include <sys/hakios.pcie.kona.mmio.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t physical, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t address, vm_size_t size);

#define KONA_MMIO_HOST_ADDRESS 0x60000000ull
#define KONA_MMIO_HOST_SIZE 0x2000u
#define KONA_MMIO_DBI_SIZE 0xf1du
#define KONA_MMIO_ELBI_OFFSET 0xf20u
#define KONA_MMIO_ELBI_SIZE 0xa8u
#define KONA_MMIO_IATU_OFFSET 0x1000u
#define KONA_MMIO_IATU_SIZE 0x1000u
#define KONA_MMIO_CONFIG_ADDRESS 0x60100000ull
#define KONA_MMIO_CONFIG_SIZE 0x1000u
#define KONA_MMIO_PARF_ADDRESS 0x01c00000ull
#define KONA_MMIO_PARF_SIZE 0x3000u
#define KONA_MMIO_PHY_ADDRESS 0x01c06000ull
#define KONA_MMIO_PHY_SIZE 0x1000u
#define KONA_MMIO_BAR_ADDRESS 0x60300000ull
#define KONA_MMIO_BAR_SIZE 0x00100000u
#define KONA_MMIO_CONFIG_DRAIN 0x000u
#define KONA_MMIO_MAX_DELAY_US 10000u

static LCK_GRP_DECLARE(kona_mmio_lock_group, "hakios-pcie-kona-mmio");

struct hakios_pcie_kona_mmio {
	lck_mtx_t lock;
	volatile uint8_t *host;
	volatile uint8_t *config;
	volatile uint8_t *parf;
	volatile uint8_t *phy;
	volatile uint8_t *bar;
	void *ownership;
	void *active_guard;
	void *bar_guard;
	uintptr_t guard_generation;
	hakios_pcie_kona_wake_t wake;
	void *wake_context;
	bool faulted;
	bool destroying;
};

static int
kona_mmio_error(int error)
{
	return error < 0 ? EIO : error;
}

static void
kona_mmio_unmap(volatile uint8_t **mapping, uint32_t size)
{
	if (*mapping != NULL) {
		ml_io_unmap((vm_offset_t)*mapping, size);
		*mapping = NULL;
	}
}

static int
kona_mmio_map(uint64_t physical, uint32_t size, volatile uint8_t **mapping)
{
	vm_offset_t address = ml_io_map_unmappable((vm_offset_t)physical, size, VM_WIMG_IO);

	if (address == 0) {
		return ENOMEM;
	}
	*mapping = (volatile uint8_t *)address;
	return 0;
}

static int
kona_mmio_access_begin_locked(struct hakios_pcie_kona_mmio *mmio,
    void *ownership, void **guard)
{
	if (mmio->faulted) {
		return EIO;
	}
	if (mmio->destroying) {
		return ENXIO;
	}
	if (ownership == NULL || ownership != mmio->ownership) {
		return ESTALE;
	}
	if (mmio->active_guard != NULL) {
		return EBUSY;
	}
	if (mmio->guard_generation == UINTPTR_MAX) {
		return EOVERFLOW;
	}
	mmio->active_guard = (void *)++mmio->guard_generation;
	*guard = mmio->active_guard;
	return 0;
}

static int
kona_mmio_access_end_locked(struct hakios_pcie_kona_mmio *mmio,
    void *ownership, void **guard)
{
	if (ownership == NULL || ownership != mmio->ownership ||
	    guard == NULL || *guard == NULL || *guard != mmio->active_guard) {
		return ESTALE;
	}
	mmio->active_guard = NULL;
	mmio->bar_guard = NULL;
	*guard = NULL;
	return 0;
}

static int
kona_mmio_guard_locked(struct hakios_pcie_kona_mmio *mmio, void *ownership, void *guard)
{
	if (ownership != NULL && ownership != mmio->ownership) {
		return ESTALE;
	}
	if (mmio->ownership == NULL || guard == NULL || guard != mmio->active_guard) {
		return ESTALE;
	}
	return mmio->faulted ? EIO : 0;
}

static int
kona_mmio_region_locked(struct hakios_pcie_kona_mmio *mmio,
    enum hakios_pcie_kona_space space, uint32_t offset, uint8_t width,
    volatile uint8_t **base)
{
	uint32_t size;

	switch (space) {
	case HAKIOS_PCIE_KONA_DBI:
		*base = mmio->host;
		size = KONA_MMIO_DBI_SIZE;
		break;
	case HAKIOS_PCIE_KONA_IATU:
		*base = mmio->host + KONA_MMIO_IATU_OFFSET;
		size = KONA_MMIO_IATU_SIZE;
		break;
	case HAKIOS_PCIE_KONA_CONFIG:
		*base = mmio->config;
		size = KONA_MMIO_CONFIG_SIZE;
		break;
	case HAKIOS_PCIE_KONA_ELBI:
		*base = mmio->host + KONA_MMIO_ELBI_OFFSET;
		size = KONA_MMIO_ELBI_SIZE;
		break;
	case HAKIOS_PCIE_KONA_PARF:
		*base = mmio->parf;
		size = KONA_MMIO_PARF_SIZE;
		break;
	case HAKIOS_PCIE_KONA_PHY:
		*base = mmio->phy;
		size = KONA_MMIO_PHY_SIZE;
		break;
	default:
		return EINVAL;
	}
	if (*base == NULL || (width != 1 && width != 2 && width != 4) ||
	    (offset & (width - 1)) != 0 || (uint64_t)offset + width > size) {
		return EINVAL;
	}
	if (space != HAKIOS_PCIE_KONA_CONFIG && width != 4) {
		return EINVAL;
	}
	return 0;
}

static int
kona_mmio_read_locked(struct hakios_pcie_kona_mmio *mmio, void *ownership,
    void *guard, enum hakios_pcie_kona_space space, uint32_t offset,
    uint8_t width, uint32_t *value)
{
	volatile uint8_t *base;
	uint32_t result = 0;
	int error;

	*value = 0;
	error = kona_mmio_guard_locked(mmio, ownership, guard);
	if (error != 0) {
		return error;
	}
	error = kona_mmio_region_locked(mmio, space, offset, width, &base);
	if (error != 0) {
		return error;
	}
	switch (width) {
	case 1: {
		uint8_t byte = 0;
		error = kona_mmio_error(hakios_mmio_read8(base + offset, &byte));
		result = byte;
		break;
	}
	case 2: {
		uint16_t halfword = 0;
		error = kona_mmio_error(hakios_mmio_read16(base + offset, &halfword));
		result = halfword;
		break;
	}
	default:
		error = kona_mmio_error(hakios_mmio_read32(base + offset, &result));
		break;
	}
	if (error != 0) {
		mmio->faulted = true;
		return error;
	}
	*value = result;
	return 0;
}

static int
kona_mmio_write_locked(struct hakios_pcie_kona_mmio *mmio, void *ownership,
    void *guard, enum hakios_pcie_kona_space space, uint32_t offset,
    uint8_t width, uint32_t value)
{
	volatile uint8_t *base;
	int error;

	error = kona_mmio_guard_locked(mmio, ownership, guard);
	if (error != 0) {
		return error;
	}
	error = kona_mmio_region_locked(mmio, space, offset, width, &base);
	if (error != 0) {
		return error;
	}
	if ((width == 1 && value > UINT8_MAX) || (width == 2 && value > UINT16_MAX)) {
		return EINVAL;
	}
	switch (width) {
	case 1:
		error = kona_mmio_error(hakios_mmio_write8(base + offset, (uint8_t)value));
		break;
	case 2:
		error = kona_mmio_error(hakios_mmio_write16(base + offset, (uint16_t)value));
		break;
	default:
		error = kona_mmio_error(hakios_mmio_write32(base + offset, value));
		break;
	}
	if (error != 0) {
		mmio->faulted = true;
	}
	return error;
}

int
hakios_pcie_kona_mmio_create(struct hakios_pcie_kona_mmio **result)
{
	struct hakios_pcie_kona_mmio *mmio;
	int error;

	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	mmio = kalloc_type(struct hakios_pcie_kona_mmio, Z_WAITOK | Z_ZERO);
	if (mmio == NULL) {
		return ENOMEM;
	}
	lck_mtx_init(&mmio->lock, &kona_mmio_lock_group, LCK_ATTR_NULL);
	error = kona_mmio_map(KONA_MMIO_HOST_ADDRESS, KONA_MMIO_HOST_SIZE, &mmio->host);
	if (error == 0) {
		error = kona_mmio_map(KONA_MMIO_CONFIG_ADDRESS, KONA_MMIO_CONFIG_SIZE, &mmio->config);
	}
	if (error == 0) {
		error = kona_mmio_map(KONA_MMIO_PARF_ADDRESS, KONA_MMIO_PARF_SIZE, &mmio->parf);
	}
	if (error == 0) {
		error = kona_mmio_map(KONA_MMIO_PHY_ADDRESS, KONA_MMIO_PHY_SIZE, &mmio->phy);
	}
	if (error != 0) {
		kona_mmio_unmap(&mmio->phy, KONA_MMIO_PHY_SIZE);
		kona_mmio_unmap(&mmio->parf, KONA_MMIO_PARF_SIZE);
		kona_mmio_unmap(&mmio->config, KONA_MMIO_CONFIG_SIZE);
		kona_mmio_unmap(&mmio->host, KONA_MMIO_HOST_SIZE);
		lck_mtx_destroy(&mmio->lock, &kona_mmio_lock_group);
		kfree_type(struct hakios_pcie_kona_mmio, mmio);
		return error;
	}
	*result = mmio;
	return 0;
}

int
hakios_pcie_kona_mmio_destroy(struct hakios_pcie_kona_mmio **mmio)
{
	struct hakios_pcie_kona_mmio *state;

	if (mmio == NULL || *mmio == NULL) {
		return EINVAL;
	}
	state = *mmio;
	lck_mtx_lock(&state->lock);
	if (state->ownership != NULL || state->active_guard != NULL || state->faulted) {
		lck_mtx_unlock(&state->lock);
		return EBUSY;
	}
	state->destroying = true;
	lck_mtx_unlock(&state->lock);
	kona_mmio_unmap(&state->bar, KONA_MMIO_BAR_SIZE);
	kona_mmio_unmap(&state->phy, KONA_MMIO_PHY_SIZE);
	kona_mmio_unmap(&state->parf, KONA_MMIO_PARF_SIZE);
	kona_mmio_unmap(&state->config, KONA_MMIO_CONFIG_SIZE);
	kona_mmio_unmap(&state->host, KONA_MMIO_HOST_SIZE);
	lck_mtx_destroy(&state->lock, &kona_mmio_lock_group);
	kfree_type(struct hakios_pcie_kona_mmio, state);
	*mmio = NULL;
	return 0;
}

int
hakios_pcie_kona_mmio_authorize(struct hakios_pcie_kona_mmio *mmio, void *ownership)
{
	int error = 0;

	if (mmio == NULL || ownership == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	if (mmio->faulted) {
		error = EIO;
	} else if (mmio->destroying) {
		error = ENXIO;
	} else if (mmio->ownership != NULL || mmio->active_guard != NULL) {
		error = EBUSY;
	} else {
		mmio->ownership = ownership;
	}
	lck_mtx_unlock(&mmio->lock);
	return error;
}

int
hakios_pcie_kona_mmio_revoke(struct hakios_pcie_kona_mmio *mmio, void **ownership)
{
	int error = 0;

	if (mmio == NULL || ownership == NULL || *ownership == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	if (mmio->ownership != *ownership) {
		error = ESTALE;
	} else if (mmio->active_guard != NULL) {
		error = EBUSY;
	} else if (mmio->faulted) {
		error = EIO;
	} else {
		mmio->ownership = NULL;
		*ownership = NULL;
	}
	lck_mtx_unlock(&mmio->lock);
	return error;
}

int
hakios_pcie_kona_mmio_set_wake(struct hakios_pcie_kona_mmio *mmio,
    hakios_pcie_kona_wake_t wake, void *context)
{
	int error = 0;

	if (mmio == NULL || (wake == NULL && context != NULL)) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	if (mmio->destroying) {
		error = ENXIO;
	} else if (mmio->ownership != NULL || mmio->active_guard != NULL || mmio->faulted) {
		error = EBUSY;
	} else {
		mmio->wake = wake;
		mmio->wake_context = context;
	}
	lck_mtx_unlock(&mmio->lock);
	return error;
}

int
hakios_pcie_kona_mmio_access_begin(struct hakios_pcie_kona_mmio *mmio,
    void *ownership, void **guard)
{
	int error;

	if (mmio == NULL || guard == NULL) {
		return EINVAL;
	}
	*guard = NULL;
	lck_mtx_lock(&mmio->lock);
	error = kona_mmio_access_begin_locked(mmio, ownership, guard);
	lck_mtx_unlock(&mmio->lock);
	return error;
}

int
hakios_pcie_kona_mmio_access_end(struct hakios_pcie_kona_mmio *mmio,
    void *ownership, void **guard)
{
	int error;

	if (mmio == NULL || guard == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	error = kona_mmio_access_end_locked(mmio, ownership, guard);
	lck_mtx_unlock(&mmio->lock);
	return error;
}

int
hakios_pcie_kona_mmio_read(struct hakios_pcie_kona_mmio *mmio, void *ownership,
    void *guard, enum hakios_pcie_kona_space space, uint32_t offset,
    uint8_t width, uint32_t *value)
{
	int error;

	if (value == NULL) {
		return EINVAL;
	}
	*value = 0;
	if (mmio == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	error = kona_mmio_read_locked(mmio, ownership, guard, space, offset, width, value);
	lck_mtx_unlock(&mmio->lock);
	return error;
}

int
hakios_pcie_kona_mmio_write(struct hakios_pcie_kona_mmio *mmio, void *ownership,
    void *guard, enum hakios_pcie_kona_space space, uint32_t offset,
    uint8_t width, uint32_t value)
{
	int error;

	if (mmio == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	error = kona_mmio_write_locked(mmio, ownership, guard, space, offset, width, value);
	lck_mtx_unlock(&mmio->lock);
	return error;
}

static int
kona_mmio_config_begin(void *context, void **guard)
{
	struct hakios_pcie_kona_mmio *mmio = context;
	void *ownership;

	lck_mtx_lock(&mmio->lock);
	ownership = mmio->ownership;
	lck_mtx_unlock(&mmio->lock);
	if (ownership == NULL) {
		return ENOTSUP;
	}
	return hakios_pcie_kona_mmio_access_begin(mmio, ownership, guard);
}

static int
kona_mmio_config_end(void *context, void *guard)
{
	struct hakios_pcie_kona_mmio *mmio = context;
	void *ownership;
	void *local_guard = guard;

	lck_mtx_lock(&mmio->lock);
	ownership = mmio->ownership;
	lck_mtx_unlock(&mmio->lock);
	return hakios_pcie_kona_mmio_access_end(mmio, ownership, &local_guard);
}

static int
kona_mmio_config_read(void *context, void *guard, enum hakios_pcie_kona_space space,
    uint32_t offset, uint8_t width, uint32_t *value)
{
	return hakios_pcie_kona_mmio_read(context, NULL, guard, space, offset, width, value);
}

static int
kona_mmio_config_write(void *context, void *guard, enum hakios_pcie_kona_space space,
    uint32_t offset, uint8_t width, uint32_t value)
{
	return hakios_pcie_kona_mmio_write(context, NULL, guard, space, offset, width, value);
}

static int
kona_mmio_config_drain(void *context, void *guard)
{
	struct hakios_pcie_kona_mmio *mmio = context;
	uint32_t identity;
	int error = kona_mmio_config_read(mmio, guard, HAKIOS_PCIE_KONA_CONFIG,
	    KONA_MMIO_CONFIG_DRAIN, 4, &identity);

	if (error != 0) {
		return error;
	}
	if (identity == UINT32_MAX) {
		lck_mtx_lock(&mmio->lock);
		mmio->faulted = true;
		lck_mtx_unlock(&mmio->lock);
		return ENXIO;
	}
	return 0;
}

static int
kona_mmio_delay(void *context, void *guard, uint32_t microseconds)
{
	struct hakios_pcie_kona_mmio *mmio = context;
	int error;

	if (microseconds == 0 || microseconds > KONA_MMIO_MAX_DELAY_US) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	error = kona_mmio_guard_locked(mmio, NULL, guard);
	if (error == 0) {
		delay(microseconds);
	}
	lck_mtx_unlock(&mmio->lock);
	return error;
}

static int
kona_mmio_bar_begin(void *context, void *owner, bool wake_required)
{
	struct hakios_pcie_kona_mmio *mmio = context;
	void *ownership;
	void *guard = NULL;
	hakios_pcie_kona_wake_t wake;
	void *wake_context;
	int error;

	if (owner == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	ownership = mmio->ownership;
	wake = mmio->wake;
	wake_context = mmio->wake_context;
	lck_mtx_unlock(&mmio->lock);
	if (ownership == NULL) {
		return ENOTSUP;
	}
	error = hakios_pcie_kona_mmio_access_begin(mmio, ownership, &guard);
	if (error != 0) {
		return error;
	}
	if (wake_required) {
		if (wake == NULL) {
			error = ENOTSUP;
		} else {
			error = kona_mmio_error(wake(wake_context, ownership));
			if (error != 0) {
				lck_mtx_lock(&mmio->lock);
				mmio->faulted = true;
				lck_mtx_unlock(&mmio->lock);
			}
		}
	}
	if (error == 0) {
		lck_mtx_lock(&mmio->lock);
		if (mmio->bar == NULL) {
			error = kona_mmio_map(KONA_MMIO_BAR_ADDRESS, KONA_MMIO_BAR_SIZE, &mmio->bar);
		}
		if (error == 0) {
			mmio->bar_guard = guard;
		}
		lck_mtx_unlock(&mmio->lock);
	}
	if (error != 0) {
		(void)hakios_pcie_kona_mmio_access_end(mmio, ownership, &guard);
	}
	return error;
}

static int
kona_mmio_bar_end(void *context, void *owner)
{
	struct hakios_pcie_kona_mmio *mmio = context;
	void *ownership;
	void *guard;

	if (owner == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	ownership = mmio->ownership;
	guard = mmio->bar_guard;
	lck_mtx_unlock(&mmio->lock);
	if (guard == NULL) {
		return ESTALE;
	}
	return hakios_pcie_kona_mmio_access_end(mmio, ownership, &guard);
}

static int
kona_mmio_bar_read32(void *context, void *owner, uint32_t offset, uint32_t *value)
{
	struct hakios_pcie_kona_mmio *mmio = context;
	int error;

	if (owner == NULL || value == NULL || (offset & 3u) != 0 ||
	    (uint64_t)offset + 4 > KONA_MMIO_BAR_SIZE) {
		return EINVAL;
	}
	*value = 0;
	lck_mtx_lock(&mmio->lock);
	error = kona_mmio_guard_locked(mmio, NULL, mmio->bar_guard);
	if (error == 0 && mmio->bar == NULL) {
		error = ENXIO;
	}
	if (error == 0) {
		error = kona_mmio_error(hakios_mmio_read32(mmio->bar + offset, value));
		if (error != 0) {
			mmio->faulted = true;
		}
	}
	lck_mtx_unlock(&mmio->lock);
	return error;
}

static int
kona_mmio_bar_write32(void *context, void *owner, uint32_t offset, uint32_t value)
{
	struct hakios_pcie_kona_mmio *mmio = context;
	int error;

	if (owner == NULL || (offset & 3u) != 0 ||
	    (uint64_t)offset + 4 > KONA_MMIO_BAR_SIZE) {
		return EINVAL;
	}
	lck_mtx_lock(&mmio->lock);
	error = kona_mmio_guard_locked(mmio, NULL, mmio->bar_guard);
	if (error == 0 && mmio->bar == NULL) {
		error = ENXIO;
	}
	if (error == 0) {
		error = kona_mmio_error(hakios_mmio_write32(mmio->bar + offset, value));
		if (error != 0) {
			mmio->faulted = true;
		}
	}
	lck_mtx_unlock(&mmio->lock);
	return error;
}

int
hakios_pcie_kona_mmio_bind(struct hakios_pcie_kona_mmio *mmio,
    struct hakios_pcie_kona_platform_operations *operations)
{
	if (mmio == NULL || operations == NULL ||
	    operations->version != HAKIOS_PCIE_KONA_PLATFORM_VERSION) {
		return EINVAL;
	}
	operations->config = (struct hakios_pcie_kona_operations) {
		.version = HAKIOS_PCIE_KONA_OPERATIONS_VERSION,
		.begin = kona_mmio_config_begin,
		.end = kona_mmio_config_end,
		.read = kona_mmio_config_read,
		.write = kona_mmio_config_write,
		.config_drain = kona_mmio_config_drain,
		.delay_us = kona_mmio_delay,
	};
	operations->config_context = mmio;
	operations->bar_context = mmio;
	operations->bar_begin = kona_mmio_bar_begin;
	operations->bar_end = kona_mmio_bar_end;
	operations->bar_read32 = kona_mmio_bar_read32;
	operations->bar_write32 = kona_mmio_bar_write32;
	return 0;
}

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/vm_param.h>
#include <sys/hakios.pcie.kona.mmio.h>
#include <sys/hakios.pcie.kona.sid.h>
#include <vm/memory_types.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;

#define HOST_ADDRESS 0x60000000u
#define HOST_SIZE 0x2000u
#define CONFIG_ADDRESS 0x60100000u
#define CONFIG_SIZE 0x1000u
#define PARF_ADDRESS 0x01c00000u
#define PARF_SIZE 0x3000u
#define PHY_ADDRESS 0x01c06000u
#define PHY_SIZE 0x1000u
#define BAR_ADDRESS 0x60300000u
#define BAR_SIZE 0x100000u

struct mapping {
	uint64_t physical;
	uint32_t size;
	void *memory;
	bool mapped;
};

static uint8_t host[HOST_SIZE];
static uint8_t config[CONFIG_SIZE];
static uint8_t parf[PARF_SIZE];
static uint8_t phy[PHY_SIZE];
static uint8_t bar[BAR_SIZE];
static struct mapping mappings[] = {
	{ HOST_ADDRESS, HOST_SIZE, host, false },
	{ CONFIG_ADDRESS, CONFIG_SIZE, config, false },
	{ PARF_ADDRESS, PARF_SIZE, parf, false },
	{ PHY_ADDRESS, PHY_SIZE, phy, false },
	{ BAR_ADDRESS, BAR_SIZE, bar, false },
};
static uint64_t fail_map_address;
static unsigned map_calls;
static unsigned unmap_calls;
static unsigned read8_calls;
static unsigned read16_calls;
static unsigned read32_calls;
static unsigned write8_calls;
static unsigned write16_calls;
static unsigned write32_calls;
static unsigned wake_calls;

static struct mapping *
mapping_for(uint64_t physical)
{
	for (unsigned index = 0; index < sizeof(mappings) / sizeof(mappings[0]); index++) {
		if (mappings[index].physical == physical) {
			return &mappings[index];
		}
	}
	return NULL;
}

vm_offset_t
ml_io_map_unmappable(vm_offset_t physical, vm_size_t size, unsigned int flags)
{
	struct mapping *mapping = mapping_for(physical);

	assert(mapping != NULL && size == mapping->size && flags == VM_WIMG_IO);
	map_calls++;
	if (physical == fail_map_address) {
		return 0;
	}
	assert(!mapping->mapped);
	mapping->mapped = true;
	return (vm_offset_t)mapping->memory;
}

void
ml_io_unmap(vm_offset_t address, vm_size_t size)
{
	for (unsigned index = 0; index < sizeof(mappings) / sizeof(mappings[0]); index++) {
		if ((vm_offset_t)mappings[index].memory == address) {
			assert(mappings[index].mapped && mappings[index].size == size);
			mappings[index].mapped = false;
			unmap_calls++;
			return;
		}
	}
	assert(false);
}

void
delay(int usec)
{
	assert(usec > 0 && usec <= 10000);
}

static uint32_t
load_le(const volatile void *address, unsigned width)
{
	const volatile uint8_t *bytes = address;
	uint32_t value = 0;

	for (unsigned index = 0; index < width; index++) {
		value |= (uint32_t)bytes[index] << (index * 8);
	}
	return value;
}

static void
store_le(volatile void *address, unsigned width, uint32_t value)
{
	volatile uint8_t *bytes = address;

	for (unsigned index = 0; index < width; index++) {
		bytes[index] = (uint8_t)(value >> (index * 8));
	}
}

int
hakios_mmio_read8(const volatile void *address, uint8_t *value)
{
	read8_calls++;
	*value = (uint8_t)load_le(address, 1);
	return 0;
}

int
hakios_mmio_read16(const volatile void *address, uint16_t *value)
{
	read16_calls++;
	*value = (uint16_t)load_le(address, 2);
	return 0;
}

int
hakios_mmio_read32(const volatile void *address, uint32_t *value)
{
	read32_calls++;
	*value = load_le(address, 4);
	return 0;
}

int
hakios_mmio_write8(volatile void *address, uint8_t value)
{
	write8_calls++;
	store_le(address, 1, value);
	return 0;
}

int
hakios_mmio_write16(volatile void *address, uint16_t value)
{
	write16_calls++;
	store_le(address, 2, value);
	return 0;
}

int
hakios_mmio_write32(volatile void *address, uint32_t value)
{
	write32_calls++;
	store_le(address, 4, value);
	return 0;
}

static int
wake_endpoint(void *context, void *ownership)
{
	assert(context == &wake_calls && ownership == &map_calls);
	wake_calls++;
	return 0;
}

static void
set_word(uint8_t *base, uint32_t offset, uint32_t value)
{
	store_le(base + offset, 4, value);
}

static uint32_t
get_word(const uint8_t *base, uint32_t offset)
{
	return load_le(base + offset, 4);
}

static void
test_mapped_kona_backend(void)
{
	const struct hakios_pcie_kona_resources resources = {
		.dbi = { HOST_ADDRESS, 0xf1d },
		.iatu = { HOST_ADDRESS + 0x1000, 0x1000 },
		.config = { CONFIG_ADDRESS, 0x100000 },
	};
	struct hakios_pcie_kona_platform_operations operations = {
		.version = HAKIOS_PCIE_KONA_PLATFORM_VERSION,
	};
	struct hakios_pcie_kona_mmio *mmio = NULL;
	struct hakios_pcie_kona_sid *sid = NULL;
	struct hakios_pcie_kona *config_engine = NULL;
	void *ownership = &map_calls;
	void *wrong_ownership = &unmap_calls;
	void *sid_cookie = NULL;
	void *retry_cookie = NULL;
	void *guard = NULL;
	void *other_guard = NULL;
	uint32_t value = UINT32_MAX;
	uint32_t saved_region[7];
	uint32_t saved_sid_table[256];

	memset(host, 0, sizeof(host));
	memset(config, 0, sizeof(config));
	memset(parf, 0, sizeof(parf));
	memset(phy, 0, sizeof(phy));
	memset(bar, 0, sizeof(bar));
	for (unsigned index = 0; index < sizeof(mappings) / sizeof(mappings[0]); index++) {
		mappings[index].mapped = false;
	}
	map_calls = unmap_calls = 0;
	read8_calls = read16_calls = read32_calls = 0;
	write8_calls = write16_calls = write32_calls = 0;
	wake_calls = 0;

	assert(hakios_pcie_kona_mmio_create(&mmio) == 0 && mmio != NULL);
	assert(map_calls == 4 && unmap_calls == 0);
	assert(hakios_pcie_kona_mmio_bind(mmio, &operations) == 0);
	assert(operations.config_context == mmio && operations.bar_context == mmio);
	assert(operations.config.version == HAKIOS_PCIE_KONA_OPERATIONS_VERSION);
	assert(operations.config.begin(operations.config_context, &guard) == ENOTSUP);
	assert(guard == NULL);
	assert(operations.bar_begin(operations.bar_context, &operations, false) == ENOTSUP);
	assert(map_calls == 4 && read8_calls + read16_calls + read32_calls == 0);

	assert(hakios_pcie_kona_mmio_authorize(mmio, ownership) == 0);
	assert(operations.bar_begin(operations.bar_context, &operations, true) == ENOTSUP);
	assert(map_calls == 4);
	assert(hakios_pcie_kona_mmio_revoke(mmio, &ownership) == 0);
	assert(ownership == NULL);
	assert(hakios_pcie_kona_mmio_set_wake(mmio, wake_endpoint, &wake_calls) == 0);
	ownership = &map_calls;
	assert(hakios_pcie_kona_mmio_access_begin(mmio, wrong_ownership, &guard) == ESTALE);
	assert(guard == NULL);
	assert(hakios_pcie_kona_mmio_authorize(mmio, ownership) == 0);
	assert(hakios_pcie_kona_mmio_authorize(mmio, wrong_ownership) == EBUSY);
	for (unsigned index = 0; index < 256; index++) {
		saved_sid_table[index] = 0xa5000000u | index * 0x101u;
		set_word(parf, 0x2000 + index * 4, saved_sid_table[index]);
	}
	assert(hakios_pcie_kona_sid_create(mmio, &sid) == 0);
	assert(hakios_pcie_kona_sid_apply(sid, ownership, &sid_cookie) == 0);
	assert(sid_cookie == sid);
	assert(get_word(parf, 0x2000 + 0x15 * 4) == 0x01000100u);
	assert(hakios_pcie_kona_sid_apply(sid, ownership, &retry_cookie) == EBUSY);
	assert(retry_cookie == NULL);
	assert(hakios_pcie_kona_sid_restore(sid, wrong_ownership, &sid_cookie) == ESTALE);
	assert(sid_cookie == sid);
	assert(hakios_pcie_kona_sid_restore(sid, ownership, &sid_cookie) == 0);
	assert(sid_cookie == NULL);
	for (unsigned index = 0; index < 256; index++) {
		assert(get_word(parf, 0x2000 + index * 4) == saved_sid_table[index]);
	}
	assert(hakios_pcie_kona_sid_destroy(&sid) == 0);
	assert(hakios_pcie_kona_mmio_access_begin(mmio, ownership, &guard) == 0);
	assert(guard != NULL);
	assert(hakios_pcie_kona_mmio_access_begin(mmio, ownership, &other_guard) == EBUSY);
	assert(other_guard == NULL);
	assert(hakios_pcie_kona_mmio_revoke(mmio, &ownership) == EBUSY);
	assert(ownership == &map_calls);
	assert(hakios_pcie_kona_mmio_read(mmio, ownership, guard, HAKIOS_PCIE_KONA_CONFIG,
	    1, 2, &value) == EINVAL);
	assert(hakios_pcie_kona_mmio_read(mmio, ownership, guard, HAKIOS_PCIE_KONA_CONFIG,
	    CONFIG_SIZE - 1, 2, &value) == EINVAL);
	assert(hakios_pcie_kona_mmio_read(mmio, ownership, guard, HAKIOS_PCIE_KONA_DBI,
	    0, 1, &value) == EINVAL);
	assert(hakios_pcie_kona_mmio_write(mmio, ownership, guard, HAKIOS_PCIE_KONA_CONFIG,
	    6, 1, 0x5a) == 0);
	assert(hakios_pcie_kona_mmio_read(mmio, ownership, guard, HAKIOS_PCIE_KONA_CONFIG,
	    6, 1, &value) == 0 && value == 0x5a);
	assert(hakios_pcie_kona_mmio_write(mmio, ownership, guard, HAKIOS_PCIE_KONA_CONFIG,
	    0, 1, 0x100) == EINVAL);
	assert(hakios_pcie_kona_mmio_access_end(mmio, ownership, &guard) == 0);

	set_word(host, 0x80, 1u << 29);
	for (unsigned index = 0; index < 7; index++) {
		saved_region[index] = 0x100u + index * 4;
		set_word(host, 0x1000 + index * 4, saved_region[index]);
	}
	set_word(config, 0, 0x110117cb);
	set_word(config, 4, 0xa5a50001);
	assert(hakios_pcie_kona_create(&operations.config, operations.config_context,
	    &resources, &config_engine) == 0);
	value = 0;
	assert(hakios_pcie_kona_config_read(config_engine, 0x100, 0, 4, &value) == 0);
	assert(value == 0x110117cb);
	assert(hakios_pcie_kona_config_write(config_engine, 0x100, 4, 2, 0x1234) == 0);
	assert(hakios_pcie_kona_config_read(config_engine, 0x100, 4, 2, &value) == 0);
	assert(value == 0x1234);
	for (unsigned index = 0; index < 7; index++) {
		assert(get_word(host, 0x1000 + index * 4) == saved_region[index]);
	}
	assert(read8_calls != 0 && read16_calls != 0 && read32_calls != 0);
	assert(write8_calls != 0 && write16_calls != 0 && write32_calls != 0);
	assert(hakios_pcie_kona_destroy(&config_engine) == 0);

	fail_map_address = BAR_ADDRESS;
	assert(operations.bar_begin(operations.bar_context, &operations, false) == ENOMEM);
	assert(!mappings[4].mapped && map_calls == 5);
	fail_map_address = 0;
	assert(operations.bar_begin(operations.bar_context, &operations, true) == 0);
	assert(wake_calls == 1 && map_calls == 6 && mappings[4].mapped);
	set_word(bar, 0x100, 0xdecafbad);
	value = 0;
	assert(operations.bar_read32(operations.bar_context, &operations, 0x100, &value) == 0);
	assert(value == 0xdecafbad);
	assert(operations.bar_write32(operations.bar_context, &operations, 0x104, 0x12345678) == 0);
	assert(get_word(bar, 0x104) == 0x12345678);
	assert(operations.bar_read32(operations.bar_context, &operations, BAR_SIZE - 2, &value) == EINVAL);
	assert(operations.bar_end(operations.bar_context, &operations) == 0);

	assert(hakios_pcie_kona_mmio_revoke(mmio, &ownership) == 0);
	assert(ownership == NULL);
	assert(hakios_pcie_kona_mmio_destroy(&mmio) == 0 && mmio == NULL);
	assert(unmap_calls == 5);
	puts("kona-mmio-test: PASS (owned mappings, guard lifecycle, exact-width CFG, iATU restore, lazy BAR and wake gate)");
}

int
main(void)
{
	test_mapped_kona_backend();
	return 0;
}

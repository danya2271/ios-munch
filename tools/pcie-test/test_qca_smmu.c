#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kern/locks.h>
#include <mach/vm_param.h>
#include <sys/hakios.qca6390.h>

#define SMMU_BYTES 0x100000u
#define SCR0 0x000u
#define IDR0 0x020u
#define IDR1 0x024u
#define SMR0 0x800u
#define S2CR0 0xc00u
#define CBAR(context) (0x1000u + 4u * (context))
#define CBA2R(context) (0x1800u + 4u * (context))
#define CB_SCTLR 0x000u
#define CB_TTBR0 0x020u
#define CB_TCR 0x030u
#define CB_MAIR0 0x038u
#define CB_FSR 0x058u
#define CB_TLBSTATUS 0x7f4u

static uint8_t smmu[SMMU_BYTES];
static uint8_t host[0x2000], parf[0x3000], phy[0x1000], tlmm[0x100000], gcc[0x90000], config[0x1000];
static int64_t fail_offset = -1;
static unsigned map_count;
static unsigned unmap_count;
static unsigned read_count;
static unsigned config_map_count;
static unsigned endpoint_identity_reads;
static bool read_cbar;
static bool read_cba2r;
static bool read_context;

LCK_GRP_DECLARE(smmu_test_group, "qca-smmu-test");
LCK_MTX_DECLARE(hakios_smmu_lock, &smmu_test_group);

static void
put32(uint32_t offset, uint32_t value)
{
	assert((uint64_t)offset + sizeof(value) <= sizeof(smmu));
	memcpy(smmu + offset, &value, sizeof(value));
}

static void
put_region32(uint8_t *region, size_t size, uint32_t offset, uint32_t value)
{
	assert((uint64_t)offset + sizeof(value) <= size);
	memcpy(region + offset, &value, sizeof(value));
}

static uint32_t
get32(const volatile void *address)
{
	uint32_t value;
	memcpy(&value, (const void *)address, sizeof(value));
	return value;
}

vm_offset_t
ml_io_map_unmappable(vm_offset_t physical, vm_size_t size, unsigned int flags)
{
	assert(flags == 7u);
	map_count++;
	if (physical == 0x15000000u && size == SMMU_BYTES) return (vm_offset_t)smmu;
	if (physical == 0x60000000u && size == sizeof(host)) return (vm_offset_t)host;
	if (physical == 0x01c00000u && size == sizeof(parf)) return (vm_offset_t)parf;
	if (physical == 0x01c06000u && size == sizeof(phy)) return (vm_offset_t)phy;
	if (physical == 0x0f100000u && size == sizeof(tlmm)) return (vm_offset_t)tlmm;
	if (physical == 0x00100000u && size == sizeof(gcc)) return (vm_offset_t)gcc;
	if (physical == 0x60100000u && size == sizeof(config)) {
		config_map_count++;
		return (vm_offset_t)config;
	}
	assert(!"unexpected diagnostic mapping");
	return 0;
}

void
ml_io_unmap(vm_offset_t address, vm_size_t size)
{
	assert((address == (vm_offset_t)smmu && size == sizeof(smmu)) ||
	    (address == (vm_offset_t)host && size == sizeof(host)) ||
	    (address == (vm_offset_t)parf && size == sizeof(parf)) ||
	    (address == (vm_offset_t)phy && size == sizeof(phy)) ||
	    (address == (vm_offset_t)tlmm && size == sizeof(tlmm)) ||
	    (address == (vm_offset_t)gcc && size == sizeof(gcc)) ||
	    (address == (vm_offset_t)config && size == sizeof(config)));
	unmap_count++;
}

int
hakios_mmio_read32(const volatile void *address, uint32_t *value)
{
	uintptr_t pointer = (uintptr_t)address, base;
	size_t size;
	if (pointer >= (uintptr_t)smmu && pointer < (uintptr_t)smmu + sizeof(smmu)) {
		base = (uintptr_t)smmu; size = sizeof(smmu);
	} else if (pointer >= (uintptr_t)host && pointer < (uintptr_t)host + sizeof(host)) {
		base = (uintptr_t)host; size = sizeof(host);
	} else if (pointer >= (uintptr_t)parf && pointer < (uintptr_t)parf + sizeof(parf)) {
		base = (uintptr_t)parf; size = sizeof(parf);
	} else if (pointer >= (uintptr_t)phy && pointer < (uintptr_t)phy + sizeof(phy)) {
		base = (uintptr_t)phy; size = sizeof(phy);
	} else if (pointer >= (uintptr_t)tlmm && pointer < (uintptr_t)tlmm + sizeof(tlmm)) {
		base = (uintptr_t)tlmm; size = sizeof(tlmm);
	} else if (pointer >= (uintptr_t)gcc && pointer < (uintptr_t)gcc + sizeof(gcc)) {
		base = (uintptr_t)gcc; size = sizeof(gcc);
	} else if (pointer >= (uintptr_t)config && pointer < (uintptr_t)config + sizeof(config)) {
		base = (uintptr_t)config; size = sizeof(config);
	} else {
		assert(!"MMIO read outside mapped regions");
		return -EFAULT;
	}
	uintptr_t offset = pointer - base;
	assert(value != NULL && offset + sizeof(*value) <= size);
	if (base == (uintptr_t)smmu && (int64_t)offset == fail_offset) return -EFAULT;
	read_count++;
	if (offset == CBAR(4)) read_cbar = true;
	if (offset == CBA2R(4)) read_cba2r = true;
	if (offset == ((128u + 4u) * 4096u) + CB_TTBR0) read_context = true;
	if (base == (uintptr_t)config && offset == 0) endpoint_identity_reads++;
	*value = get32(address);
	return 0;
}

bool
hakios_qca6390_platform_matches(void)
{
	return true;
}

static void
reset_model(void)
{
	memset(smmu, 0, sizeof(smmu));
	memset(host, 0, sizeof(host));
	memset(parf, 0, sizeof(parf));
	memset(phy, 0, sizeof(phy));
	memset(tlmm, 0, sizeof(tlmm));
	memset(gcc, 0, sizeof(gcc));
	memset(config, 0, sizeof(config));
	fail_offset = -1;
	map_count = unmap_count = read_count = 0;
	config_map_count = endpoint_identity_reads = 0;
	read_cbar = read_cba2r = read_context = false;
}

static void
configure_translation(uint32_t scr0, uint32_t idr0, uint32_t idr1,
    uint32_t smr, uint32_t s2cr, uint32_t page_size, uint32_t register_pages, uint32_t context)
{
	uint32_t context_base = (register_pages + context) * page_size;
	put32(SCR0, scr0);
	put32(IDR0, idr0);
	put32(IDR1, idr1);
	put32(SMR0, smr);
	put32(S2CR0, s2cr);
	put32(page_size + 4u * context, 0x00031f00u);
	put32(page_size + 0x800u + 4u * context, 1u);
	put32(context_base + CB_SCTLR, 1u);
	put32(context_base + CB_TTBR0, 0x12345000u);
	put32(context_base + CB_TTBR0 + 4, 0u);
	put32(context_base + CB_TCR, 0x123u);
	put32(context_base + CB_MAIR0, 0xf404ff44u);
	put32(context_base + CB_FSR, 0u);
	put32(context_base + CB_TLBSTATUS, 0u);
}

int
main(void)
{
	reset_model();
	configure_translation(0, 1, (6u << 28) | 8u, 0x80001c01u, 4u,
	    4096u, 128u, 4u);
	hakios_qca6390_smmu_report();
	assert(map_count == 1 && unmap_count == 1 && read_count > 10);
	assert(read_cbar && read_cba2r && read_context);

	reset_model();
	put32(SCR0, 0);
	put32(IDR0, 1);
	put32(IDR1, (1u << 31) | (6u << 28) | 8u);
	put32(SMR0, 0x80001c01u);
	put32(S2CR0, 4u);
	hakios_qca6390_smmu_report();
	assert(map_count == 1 && unmap_count == 1 && !read_cbar && !read_cba2r && !read_context);

	reset_model();
	configure_translation(0, 1, (6u << 28) | 8u, 0x80001234u,
	    (1u << 16) | 4u, 4096u, 128u, 4u);
	hakios_qca6390_smmu_report();
	assert(map_count == 1 && unmap_count == 1 && !read_context);

	reset_model();
	configure_translation(1u << 3, (1u << 8) | 1u, (6u << 28) | 8u,
	    0x00001c01u, (1u << 10) | 4u, 4096u, 128u, 4u);
	hakios_qca6390_smmu_report();
	assert(map_count == 1 && unmap_count == 1 && read_cbar && read_cba2r && read_context);

	reset_model();
	configure_translation(0, 1, (6u << 28) | 8u, 0x80001c01u, 4u,
	    4096u, 128u, 4u);
	fail_offset = IDR1;
	hakios_qca6390_smmu_report();
	assert(map_count == 1 && unmap_count == 1);

	reset_model();
	put32(SCR0, UINT32_MAX);
	put32(IDR0, UINT32_MAX);
	put32(IDR1, UINT32_MAX);
	hakios_qca6390_smmu_report();
	assert(map_count == 1 && unmap_count == 1 && read_count == 3);

	reset_model();
	put_region32(host, sizeof(host), 0x80, 1u << 29);
	put_region32(host, sizeof(host), 0xf28, 0x400u);
	put_region32(host, sizeof(host), 0x000, 4u);
	put_region32(host, sizeof(host), 0x004, 0x80000000u);
	put_region32(host, sizeof(host), 0x008, 0x60100000u);
	put_region32(host, sizeof(host), 0x00c, 0u);
	put_region32(host, sizeof(host), 0x010, 0x601fffffu);
	put_region32(host, sizeof(host), 0x014, 0x01000000u);
	put_region32(host, sizeof(host), 0x018, 0u);
	put_region32(parf, sizeof(parf), 0x2000 + 0x15u * 4, 0x01000100u);
	put_region32(config, sizeof(config), 0, 0x110117cbu);
	put_region32(config, sizeof(config), 8, 0x02800001u);
	hakios_qca6390_rc0_report();
	assert(config_map_count == 1 && endpoint_identity_reads == 1 && map_count == unmap_count);

	reset_model();
	put_region32(host, sizeof(host), 0x80, 1u << 29);
	put_region32(host, sizeof(host), 0xf28, 0x400u);
	put_region32(host, sizeof(host), 0x004, 0x80000000u);
	put_region32(host, sizeof(host), 0x008, 0x60100000u);
	put_region32(host, sizeof(host), 0x010, 0x601fffffu);
	put_region32(host, sizeof(host), 0x014, 0x01000000u);
	hakios_qca6390_rc0_report();
	assert(config_map_count == 0 && endpoint_identity_reads == 0 && map_count == unmap_count);

	printf("qca-smmu-test: PASS (read-only SID/context snapshot, abort cleanup, guarded CFG0; no writes)\n");
	return 0;
}

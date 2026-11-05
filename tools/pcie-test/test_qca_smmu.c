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
static int64_t fail_offset = -1;
static unsigned map_count;
static unsigned unmap_count;
static unsigned read_count;
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
	assert(physical == 0x15000000u);
	assert(size == SMMU_BYTES);
	assert(flags == 7u);
	map_count++;
	return (vm_offset_t)smmu;
}

void
ml_io_unmap(vm_offset_t address, vm_size_t size)
{
	assert(address == (vm_offset_t)smmu);
	assert(size == SMMU_BYTES);
	unmap_count++;
}

int
hakios_mmio_read32(const volatile void *address, uint32_t *value)
{
	uintptr_t offset = (uintptr_t)address - (uintptr_t)smmu;
	assert(value != NULL && offset + sizeof(*value) <= sizeof(smmu));
	if ((int64_t)offset == fail_offset) return -EFAULT;
	read_count++;
	if (offset == CBAR(4)) read_cbar = true;
	if (offset == CBA2R(4)) read_cba2r = true;
	if (offset == ((128u + 4u) * 4096u) + CB_TTBR0) read_context = true;
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
	fail_offset = -1;
	map_count = unmap_count = read_count = 0;
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

	printf("qca-smmu-test: PASS (SID match, bypass, extended SID, context offsets, abort cleanup; read-only)\n");
	return 0;
}

#include <kern/locks.h>
#include <mach/vm_param.h>
#include <sys/errno.h>
#include <sys/hakios.mmio.h>
#include <sys/hakios.qca6390.h>
#include <sys/systm.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t physical, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t address, vm_size_t size);
extern lck_mtx_t hakios_smmu_lock;

#define QCA_SMMU_PHYS 0x15000000ull
#define QCA_SMMU_SIZE 0x00100000u
#define QCA_SMMU_SCR0 0x000u
#define QCA_SMMU_IDR0 0x020u
#define QCA_SMMU_IDR1 0x024u
#define QCA_SMMU_SMR(index) (0x800u + 4u * (index))
#define QCA_SMMU_S2CR(index) (0xc00u + 4u * (index))
#define QCA_SMMU_CBAR(index) (4u * (index))
#define QCA_SMMU_CBA2R(index) (0x800u + 4u * (index))
#define QCA_SMMU_CB_SCTLR 0x000u
#define QCA_SMMU_CB_TCR2 0x010u
#define QCA_SMMU_CB_TTBR0 0x020u
#define QCA_SMMU_CB_TTBR1 0x028u
#define QCA_SMMU_CB_TCR 0x030u
#define QCA_SMMU_CB_MAIR0 0x038u
#define QCA_SMMU_CB_MAIR1 0x03cu
#define QCA_SMMU_CB_FSR 0x058u
#define QCA_SMMU_CB_FAR 0x060u
#define QCA_SMMU_CB_FSYNR0 0x068u
#define QCA_SMMU_CB_FSYNR1 0x06cu
#define QCA_SMMU_CB_TLBSTATUS 0x7f4u
#define QCA_SMMU_SGFSR 0x048u
#define QCA_SMMU_SGFSYNR0 0x050u
#define QCA_SMMU_SGFSYNR1 0x054u
#define QCA_SMMU_EXIDS (1u << 3)
#define QCA_SMMU_IDR0_EXIDS (1u << 8)
#define QCA_SMMU_S2CR_EXIDVALID (1u << 10)
#define QCA_SMMU_CB_OFFSET 0x1000u

static int
qca_smmu_read32(volatile uint8_t *base, uint32_t offset, uint32_t *value)
{
	if (base == NULL || value == NULL || (offset & 3) != 0 ||
	    (uint64_t)offset + sizeof(*value) > QCA_SMMU_SIZE) {
		return EINVAL;
	}
	int error = hakios_mmio_read32(base + offset, value);
	return error < 0 ? EIO : error;
}

static int
qca_smmu_context_report(volatile uint8_t *base, uint32_t page_size, uint32_t register_pages,
    uint32_t context)
{
	uint64_t global_offset = page_size;
	uint64_t context_offset = ((uint64_t)register_pages + context) * page_size;
	uint32_t cbar, cba2r, sctlr, tcr2, ttbr0_low, ttbr0_high, ttbr1_low, ttbr1_high;
	uint32_t tcr, mair0, mair1, fsr, far_low, far_high, fsynr0, fsynr1, tlbstatus;
	int error;

	if (context_offset > QCA_SMMU_SIZE - QCA_SMMU_CB_OFFSET ||
	    global_offset > QCA_SMMU_SIZE - QCA_SMMU_CB_OFFSET) {
		printf("qca6390: SMMU CB%u register windows exceed the device-tree aperture\n", context);
		return EOVERFLOW;
	}
#define QCA_SMMU_READ(offset, output) do { \
	error = qca_smmu_read32(base, (uint32_t)(offset), &(output)); \
	if (error != 0) return error; \
} while (0)
	QCA_SMMU_READ(global_offset + QCA_SMMU_CBAR(context), cbar);
	QCA_SMMU_READ(global_offset + QCA_SMMU_CBA2R(context), cba2r);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_SCTLR, sctlr);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_TCR2, tcr2);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_TTBR0, ttbr0_low);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_TTBR0 + 4, ttbr0_high);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_TTBR1, ttbr1_low);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_TTBR1 + 4, ttbr1_high);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_TCR, tcr);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_MAIR0, mair0);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_MAIR1, mair1);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_FSR, fsr);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_FAR, far_low);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_FAR + 4, far_high);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_FSYNR0, fsynr0);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_FSYNR1, fsynr1);
	QCA_SMMU_READ(context_offset + QCA_SMMU_CB_TLBSTATUS, tlbstatus);
#undef QCA_SMMU_READ

	printf("qca6390: SMMU CB%u CBAR=%#x CBA2R=%#x SCTLR=%#x TCR2=%#x TTBR0=%#x%08x TTBR1=%#x%08x TCR=%#x MAIR0=%#x MAIR1=%#x\n",
	    context, cbar, cba2r, sctlr, tcr2, ttbr0_high, ttbr0_low, ttbr1_high, ttbr1_low, tcr, mair0, mair1);
	printf("qca6390: SMMU CB%u FSR=%#x FAR=%#x%08x FSYNR0=%#x FSYNR1=%#x TLBSTATUS=%#x\n",
	    context, fsr, far_high, far_low, fsynr0, fsynr1, tlbstatus);
	return 0;
}

void
hakios_qca6390_smmu_report(void)
{
	vm_offset_t mapping;
	volatile uint8_t *base;
	uint32_t scr0, idr0, idr1, match_count, context_count;
	uint32_t page_size, register_pages, stream_mask, matching = 0, translations = 0;
	uint32_t sgfsr, sgfsynr0, sgfsynr1;
	bool geometry_matches;
	int error = 0;

	if (!hakios_qca6390_platform_matches()) {
		printf("qca6390: SMMU diagnostic skipped: not Munch\n");
		return;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	mapping = ml_io_map_unmappable((vm_offset_t)QCA_SMMU_PHYS, QCA_SMMU_SIZE, VM_WIMG_IO);
	if (mapping == 0) {
		printf("qca6390: read-only SMMU diagnostic: MMIO mapping unavailable\n");
		goto out;
	}
	base = (volatile uint8_t *)mapping;
	error = qca_smmu_read32(base, QCA_SMMU_SCR0, &scr0);
	if (error == 0) error = qca_smmu_read32(base, QCA_SMMU_IDR0, &idr0);
	if (error == 0) error = qca_smmu_read32(base, QCA_SMMU_IDR1, &idr1);
	if (error != 0) {
		printf("qca6390: read-only SMMU identification read failed (%d); no writes attempted\n", error);
		goto unmap;
	}
	if (scr0 == UINT32_MAX || idr0 == UINT32_MAX || idr1 == UINT32_MAX) {
		printf("qca6390: SMMU identification returned all ones; snapshot rejected\n");
		goto unmap;
	}

	match_count = idr0 & 0xff;
	context_count = idr1 & 0xff;
	page_size = (idr1 & (1u << 31)) != 0 ? 0x10000u : 0x1000u;
	register_pages = 2u << ((idr1 >> 28) & 7);
	stream_mask = (scr0 & QCA_SMMU_EXIDS) != 0 ? 0xffffu : 0x7fffu;
	geometry_matches = (uint64_t)2 * register_pages * page_size == QCA_SMMU_SIZE;
	printf("qca6390: read-only SMMU RID=%#x SID=%#x SCR0=%#x IDR0=%#x IDR1=%#x SMRs=%u CBs=%u page=%#x register-pages=%u aperture-match=%u\n",
	    0x0100u, 0x1c01u, scr0, idr0, idr1, match_count, context_count, page_size, register_pages,
	    geometry_matches);
	if (!geometry_matches) {
		printf("qca6390: SMMU IDR geometry spans %#llx bytes, but device tree describes %#x; CB offsets will not be trusted\n",
		    (unsigned long long)2 * register_pages * page_size, QCA_SMMU_SIZE);
	}
	error = qca_smmu_read32(base, QCA_SMMU_SGFSR, &sgfsr);
	if (error == 0) error = qca_smmu_read32(base, QCA_SMMU_SGFSYNR0, &sgfsynr0);
	if (error == 0) error = qca_smmu_read32(base, QCA_SMMU_SGFSYNR1, &sgfsynr1);
	if (error == 0) {
		printf("qca6390: SMMU global fault state SGFSR=%#x SGFSYNR0=%#x SGFSYNR1=%#x\n",
		    sgfsr, sgfsynr0, sgfsynr1);
	} else {
		printf("qca6390: SMMU global fault registers unavailable (%d)\n", error);
	}
	error = 0;
	if ((scr0 & 1u) != 0) {
		printf("qca6390: SMMU reports global configuration disabled; no writes attempted\n");
		goto unmap;
	}
	if ((scr0 & QCA_SMMU_EXIDS) != 0 && (idr0 & QCA_SMMU_IDR0_EXIDS) == 0) {
		printf("qca6390: inconsistent extended-SID capability; stream scan skipped\n");
		goto unmap;
	}

	for (uint32_t index = 0; index < match_count; index++) {
		uint32_t smr, s2cr;
		error = qca_smmu_read32(base, QCA_SMMU_SMR(index), &smr);
		if (error == 0) error = qca_smmu_read32(base, QCA_SMMU_S2CR(index), &s2cr);
		if (error != 0) {
			printf("qca6390: SMMU match%u read failed (%d); remaining scan stopped\n", index, error);
			break;
		}
		bool active = (scr0 & QCA_SMMU_EXIDS) != 0 ?
		    (s2cr & QCA_SMMU_S2CR_EXIDVALID) != 0 : (smr & (1u << 31)) != 0;
		uint32_t sid = smr & stream_mask;
		uint32_t mask = (smr >> 16) & stream_mask;
		if (((sid ^ 0x1c01u) & ~mask & stream_mask) != 0) continue;
		matching++;
		uint32_t type = (s2cr >> 16) & 3u;
		uint32_t context = s2cr & 0xffu;
		printf("qca6390: SMMU match%u active=%u SMR=%#x S2CR=%#x covers-SID=1 type=%u CB=%u\n",
		    index, active, smr, s2cr, type, context);
		if (!active || type != 0 || context >= context_count || !geometry_matches) continue;
		translations++;
		error = qca_smmu_context_report(base, page_size, register_pages, context);
		if (error != 0) {
			printf("qca6390: SMMU CB%u snapshot incomplete (%d)\n", context, error);
		}
	}
	printf("qca6390: SMMU snapshot complete: matching=%u active-translation=%u; read-only, no writes attempted\n",
	    matching, translations);
	if (matching == 0) {
		printf("qca6390: no SMMU stream match covers SID %#x\n", 0x1c01u);
	}
unmap:
	ml_io_unmap(mapping, QCA_SMMU_SIZE);
out:
	lck_mtx_unlock(&hakios_smmu_lock);
}

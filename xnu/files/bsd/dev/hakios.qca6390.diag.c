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

struct qca_diag_region {
	uint64_t physical;
	vm_size_t size;
	vm_offset_t mapping;
};

enum qca_diag_region_index {
	QCA_DIAG_HOST,
	QCA_DIAG_PARF,
	QCA_DIAG_PHY,
	QCA_DIAG_TLMM,
	QCA_DIAG_GCC,
	QCA_DIAG_CONFIG,
	QCA_DIAG_REGION_COUNT,
};

static LCK_GRP_DECLARE(qca_diag_lock_group, "qca6390-diagnostic");
static LCK_MTX_DECLARE(qca_diag_lock, &qca_diag_lock_group);

static struct qca_diag_region qca_diag_regions[QCA_DIAG_REGION_COUNT] = {
	[QCA_DIAG_HOST] = {.physical = 0x60000000ull, .size = 0x2000, .mapping = 0},
	[QCA_DIAG_PARF] = {.physical = 0x01c00000ull, .size = 0x3000, .mapping = 0},
	[QCA_DIAG_PHY] = {.physical = 0x01c06000ull, .size = 0x1000, .mapping = 0},
	[QCA_DIAG_TLMM] = {.physical = 0x0f100000ull, .size = 0x100000, .mapping = 0},
	[QCA_DIAG_GCC] = {.physical = 0x00100000ull, .size = 0x90000, .mapping = 0},
	[QCA_DIAG_CONFIG] = {.physical = 0x60100000ull, .size = 0x1000, .mapping = 0},
};

static int
qca_diag_read32(volatile uint8_t *base, vm_size_t size, uint32_t offset, uint32_t *value)
{
	if (base == NULL || value == NULL || (offset & 3) != 0 ||
	    (uint64_t)offset + sizeof(*value) > size) {
		return EINVAL;
	}
	int error = hakios_mmio_read32(base + offset, value);
	return error < 0 ? EIO : error;
}

static bool
qca_diag_read(struct qca_diag_region *region, const char *name, uint32_t offset, uint32_t *value)
{
	int error = region->mapping == 0 ? ENXIO :
	    qca_diag_read32((volatile uint8_t *)region->mapping, region->size, offset, value);
	if (error != 0) {
		printf("qca6390: RC0 %s[%#x] unavailable (%d)\n", name, offset, error);
		return false;
	}
	return true;
}

static uint8_t
qca_diag_sid_hash(uint16_t requester_id)
{
	uint8_t bytes[2] = {(uint8_t)(requester_id >> 8), (uint8_t)requester_id};
	uint8_t crc = 0;

	for (uint32_t byte = 0; byte < sizeof(bytes); byte++) {
		crc ^= bytes[byte];
		for (uint32_t bit = 0; bit < 8; bit++) {
			crc = (crc & 0x80u) != 0 ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static void
qca_diag_rc0_map_all(void)
{
	for (uint32_t index = 0; index < QCA_DIAG_REGION_COUNT; index++) {
		struct qca_diag_region *region = &qca_diag_regions[index];
		if (index == QCA_DIAG_CONFIG) continue;
		region->mapping = ml_io_map_unmappable((vm_offset_t)region->physical, region->size, VM_WIMG_IO);
	}
}

static bool
qca_diag_rc0_map_one(uint32_t index)
{
	struct qca_diag_region *region;

	if (index >= QCA_DIAG_REGION_COUNT || index == QCA_DIAG_CONFIG) {
		return false;
	}
	region = &qca_diag_regions[index];
	region->mapping = ml_io_map_unmappable((vm_offset_t)region->physical, region->size, VM_WIMG_IO);
	return region->mapping != 0;
}

static void
qca_diag_rc0_unmap_all(void)
{
	for (uint32_t index = 0; index < QCA_DIAG_REGION_COUNT; index++) {
		struct qca_diag_region *region = &qca_diag_regions[index];
		if (region->mapping != 0) {
			if (index == QCA_DIAG_PHY) {
				printf("qca6390: RC0 PHY mapping retained; teardown is not safe on this Kona path\n");
				continue;
			}
			ml_io_unmap(region->mapping, region->size);
			region->mapping = 0;
		}
	}
}

void
hakios_qca6390_rc0_report_stage(uint32_t stage)
{
	uint32_t dbi_link = 0, elbi_status = 0, ltssm = 0, phy_control = 0;
	uint32_t phy_status = 0, phy_power = 0, device_type = 0, iatu[7] = {0};
	uint32_t clock_control = 0, wifi_ref_clock = 0, phy_refgen = 0;
	uint32_t sid_entry = 0, gpio_cfg = 0, gpio_io = 0;
	bool dbi_read, elbi_read, iatu_read = true, qca_cfg0 = false;
	bool retain_mappings = stage == 8;

	if (!hakios_qca6390_platform_matches()) {
		printf("qca6390: RC0 diagnostic skipped: not Munch\n");
		return;
	}
	printf("qca6390: RC0 diagnostic begin; read-only\n");
	lck_mtx_lock(&qca_diag_lock);
	printf("qca6390: RC0 diagnostic lock acquired\n");
	if (stage >= 1 && stage <= 5) {
		uint32_t index = stage - 1;
		bool mapped = qca_diag_rc0_map_one(index);
		printf("qca6390: RC0 stage %u map-only region=%u physical=%#llx size=%#x mapped=%u\n",
		    stage, index, (unsigned long long)qca_diag_regions[index].physical,
		    qca_diag_regions[index].size, mapped);
		qca_diag_rc0_unmap_all();
		lck_mtx_unlock(&qca_diag_lock);
		return;
	}
	if (stage == 6) {
		bool mapped = qca_diag_rc0_map_one(QCA_DIAG_PHY);
		printf("qca6390: RC0 stage 6 PHY map-hold physical=%#llx size=%#x mapped=%u; no unmap\n",
		    (unsigned long long)qca_diag_regions[QCA_DIAG_PHY].physical,
		    qca_diag_regions[QCA_DIAG_PHY].size, mapped);
		lck_mtx_unlock(&qca_diag_lock);
		return;
	}
	if (stage == 7) {
		qca_diag_rc0_map_all();
		printf("qca6390: RC0 stage 7 map-all-hold host=%u PARF=%u PHY=%u TLMM=%u GCC=%u; no reads/unmap\n",
		    qca_diag_regions[QCA_DIAG_HOST].mapping != 0,
		    qca_diag_regions[QCA_DIAG_PARF].mapping != 0,
		    qca_diag_regions[QCA_DIAG_PHY].mapping != 0,
		    qca_diag_regions[QCA_DIAG_TLMM].mapping != 0,
		    qca_diag_regions[QCA_DIAG_GCC].mapping != 0);
		lck_mtx_unlock(&qca_diag_lock);
		return;
	}
	qca_diag_rc0_map_all();
	printf("qca6390: RC0 MMIO mappings host=%u PARF=%u PHY=%u TLMM=%u GCC=%u; CFG0 deferred\n",
	    qca_diag_regions[QCA_DIAG_HOST].mapping != 0,
	    qca_diag_regions[QCA_DIAG_PARF].mapping != 0,
	    qca_diag_regions[QCA_DIAG_PHY].mapping != 0,
	    qca_diag_regions[QCA_DIAG_TLMM].mapping != 0,
	    qca_diag_regions[QCA_DIAG_GCC].mapping != 0);
	dbi_read = qca_diag_read(&qca_diag_regions[QCA_DIAG_HOST], "DBI", 0x80, &dbi_link);
	elbi_read = qca_diag_read(&qca_diag_regions[QCA_DIAG_HOST], "ELBI", 0xf28, &elbi_status);
	qca_diag_read(&qca_diag_regions[QCA_DIAG_PARF], "PARF", 0x1b0, &ltssm);
	qca_diag_read(&qca_diag_regions[QCA_DIAG_PARF], "PARF", 0x40, &phy_control);
	qca_diag_read(&qca_diag_regions[QCA_DIAG_PARF], "PARF", 0x1000, &device_type);
	qca_diag_read(&qca_diag_regions[QCA_DIAG_PHY], "PHY", 0x814, &phy_status);
	qca_diag_read(&qca_diag_regions[QCA_DIAG_PHY], "PHY", 0x840, &phy_power);
	printf("qca6390: RC0 read-only DBI_LINK=%#x ELBI_SYS_STTS=%#x PARF_LTSSM=%#x PHY_CTRL=%#x DEVICE_TYPE=%#x PHY_STATUS=%#x PHY_POWER=%#x\n",
	    dbi_link, elbi_status, ltssm, phy_control, device_type, phy_status, phy_power);
	if (dbi_read && elbi_read) {
		printf("qca6390: RC0 link snapshot DBI-up=%u ELBI-up=%u LTSSM-state=%#x\n",
		    (dbi_link & (1u << 29)) != 0, (elbi_status & 0x400u) != 0, (ltssm >> 12) & 0x3fu);
	}

	for (uint32_t index = 0; index < 7; index++) {
		if (!qca_diag_read(&qca_diag_regions[QCA_DIAG_HOST], "iATU0", index * 4, &iatu[index])) {
			iatu_read = false;
			break;
		}
	}
	if (iatu_read) {
		qca_cfg0 = iatu[0] == 4u && iatu[1] == 0x80000000u &&
		    iatu[2] == 0x60100000u && iatu[3] == 0 &&
		    iatu[4] == 0x601fffffu && iatu[5] == 0x01000000u && iatu[6] == 0;
		printf("qca6390: RC0 iATU0 CTRL1=%#x CTRL2=%#x BASE=%#x:%08x LIMIT=%#x TARGET=%#x:%08x QCA-CFG0=%u\n",
		    iatu[0], iatu[1], iatu[3], iatu[2], iatu[4], iatu[6], iatu[5], qca_cfg0);
	}

	if (qca_diag_read(&qca_diag_regions[QCA_DIAG_PARF], "BDF-to-SID", 0x2000, &sid_entry)) {
		uint32_t hash = qca_diag_sid_hash(0x0100u);
		bool found = false;
		bool sid_matches = false;
		for (uint32_t hop = 0; hop < 256; hop++) {
			if (!qca_diag_read(&qca_diag_regions[QCA_DIAG_PARF], "BDF-to-SID",
			    0x2000 + hash * 4, &sid_entry)) {
				break;
			}
			uint16_t bdf = (uint16_t)(sid_entry >> 16);
			uint32_t sid = 0x1c00u + ((sid_entry >> 8) & 0xffu);
			printf("qca6390: RC0 SID-table hash=%#x entry=%#x BDF=%#x SID=%#x next=%#x\n",
			    hash, sid_entry, bdf, sid, sid_entry & 0xffu);
			if (sid_entry == 0 || bdf == 0x0100u) {
				found = bdf == 0x0100u;
				sid_matches = found && sid == 0x1c01u;
				break;
			}
			hash = sid_entry & 0xffu;
			if (hash == 0) break;
		}
		printf("qca6390: RC0 RID 0x100 SID-table %s\n",
		    sid_matches ? "matches SID 0x1c01" : found ? "entry maps to a different SID" :
		    "not present in its CRC8 chain");
	}

	if (qca_diag_read(&qca_diag_regions[QCA_DIAG_TLMM], "TLMM GPIO79 CFG", 79u * 0x1000u, &gpio_cfg) &&
	    qca_diag_read(&qca_diag_regions[QCA_DIAG_TLMM], "TLMM GPIO79 IO", 79u * 0x1000u + 4, &gpio_io)) {
		printf("qca6390: RC0 PERST GPIO79 CFG=%#x IO=%#x (raw level/polarity not inferred)\n", gpio_cfg, gpio_io);
	}
	if (qca_diag_read(&qca_diag_regions[QCA_DIAG_TLMM], "TLMM GPIO81 CFG", 81u * 0x1000u, &gpio_cfg) &&
	    qca_diag_read(&qca_diag_regions[QCA_DIAG_TLMM], "TLMM GPIO81 IO", 81u * 0x1000u + 4, &gpio_io)) {
		printf("qca6390: RC0 WAKE GPIO81 CFG=%#x IO=%#x (raw)\n", gpio_cfg, gpio_io);
	}
	uint32_t core_reset = 0, phy_reset = 0;
	if (qca_diag_read(&qca_diag_regions[QCA_DIAG_GCC], "GCC PCIE clocks", 0x52008, &clock_control) &&
	    qca_diag_read(&qca_diag_regions[QCA_DIAG_GCC], "GCC WIFI refclk", 0x8c004, &wifi_ref_clock) &&
	    qca_diag_read(&qca_diag_regions[QCA_DIAG_GCC], "GCC PHY refgen", 0x6f02c, &phy_refgen) &&
	    qca_diag_read(&qca_diag_regions[QCA_DIAG_GCC], "GCC CORE reset", 0x6b000, &core_reset) &&
	    qca_diag_read(&qca_diag_regions[QCA_DIAG_GCC], "GCC PHY reset", 0x6c01c, &phy_reset)) {
		printf("qca6390: RC0 GCC clocks 0x52008=%#x WIFI_REF=%#x PHY_REFGEN=%#x resets CORE=%#x PHY=%#x (raw)\n",
		    clock_control, wifi_ref_clock, phy_refgen, core_reset, phy_reset);
	}

	if (qca_cfg0 && dbi_read && (dbi_link & (1u << 29)) != 0) {
		qca_diag_regions[QCA_DIAG_CONFIG].mapping =
		    ml_io_map_unmappable((vm_offset_t)qca_diag_regions[QCA_DIAG_CONFIG].physical,
		    qca_diag_regions[QCA_DIAG_CONFIG].size, VM_WIMG_IO);
		uint32_t identity, class_revision;
		if (qca_diag_read(&qca_diag_regions[QCA_DIAG_CONFIG], "CFG0 endpoint", 0, &identity) &&
		    qca_diag_read(&qca_diag_regions[QCA_DIAG_CONFIG], "CFG0 endpoint", 8, &class_revision)) {
			printf("qca6390: RC0 existing CFG0 identity=%#x class/revision=%#x expected-QCA=%u\n",
			    identity, class_revision, identity == 0x110117cbu && class_revision == 0x02800001u);
		}
	} else {
		printf("qca6390: RC0 CFG0 endpoint read skipped: no verified pre-existing QCA CFG0 iATU/link window\n");
	}
	if (retain_mappings) {
		printf("qca6390: RC0 stage 8 mappings retained after reads; no teardown\n");
	} else {
		qca_diag_rc0_unmap_all();
	}
	lck_mtx_unlock(&qca_diag_lock);
}

void
hakios_qca6390_rc0_report(void)
{
	hakios_qca6390_rc0_report_stage(0);
}
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

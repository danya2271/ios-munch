#include <kern/kalloc.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/vm_param.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.pcie.h>
#include <sys/hakios.qca6390.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern lck_mtx_t hakios_smmu_lock;

static LCK_GRP_DECLARE(qca6390_lock_group, "hakios-qca6390");
static LCK_MTX_DECLARE(qca6390_pcie_lock, &qca6390_lock_group);

#define QCA6390_VENDOR                 0x17cb
#define QCA6390_DEVICE                 0x1101
#define QCA6390_REVISION               0x01
#define QCA6390_CLASS                  0x028000
#define QCA6390_BAR0                   0x60300000ull

#define QCA6390_BAR0_SIZE              0x00100000u
#define QCA6390_REQUESTER_ID           0x0100u
#define QCA6390_WINDOW_REGISTER        0x310cu
#define QCA6390_WINDOW_ENABLE          0x40000000u
#define QCA6390_WINDOW_START           0x80000u
#define QCA6390_WINDOW_SHIFT           19u
#define QCA6390_WINDOW_MASK            0x3fu
#define QCA6390_REGISTER_LIMIT         0x02000000u
#define QCA6390_ACCESS_ALWAYS_LIMIT    0x0fe0u

#define QCA6390_SMMU_PHYS              0x15000000ull
#define QCA6390_STREAM_ID              0x1c01u
#define QCA6390_IOVA_BASE              0x100000000ull
#define QCA6390_IOVA_SIZE              0x04000000ull
#define QCA6390_TABLE_COUNT            36u
#define QCA6390_PAGE_SIZE              0x1000ull
#define QCA6390_PTE_ADDRESS            0x0000fffffffff000ull
#define QCA6390_PTE_NORMAL             (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)

#define SMMU_SCR0                      0x000
#define SMMU_IDR0                      0x020
#define SMMU_IDR1                      0x024
#define SMMU_IDR0_EXIDS                (1u << 8)
#define SMMU_SCR0_EXIDENABLE           (1u << 3)
#define SMMU_SMR(n)                    (0x800 + 4 * (n))
#define SMMU_S2CR(n)                   (0xc00 + 4 * (n))
#define SMMU_CBAR(n)                   (4 * (n))
#define SMMU_CBA2R(n)                  (0x800 + 4 * (n))
#define SMMU_CB_SCTLR                  0x00
#define SMMU_CB_TCR2                   0x10
#define SMMU_CB_TTBR0                  0x20
#define SMMU_CB_TTBR1                  0x28
#define SMMU_CB_TCR                    0x30
#define SMMU_CB_MAIR0                  0x38
#define SMMU_CB_MAIR1                  0x3c
#define SMMU_CB_TLBIASID               0x610
#define SMMU_CB_TLBSYNC                0x7f0
#define SMMU_CB_TLBSTATUS              0x7f4
#define SMMU_CBAR_STAGE_1              (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_SCTLR_ON                  (1u | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS               (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2                      (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0                     0xf404ff44u
#define SMMU_S2CR_VALID                (1u << 10)

struct qca6390_smmu_state {
	volatile uint32_t *base;
	volatile uint32_t *second;
	volatile uint32_t *context_regs;
	vm_offset_t tables;
	uint32_t table_count;
	uint32_t match;
	uint32_t context;
	uint32_t old_smr;
	uint32_t old_s2cr;
	uint32_t old_cbar;
	uint32_t old_cba2r;
	uint32_t old_sctlr;
	uint32_t old_tcr;
	uint32_t old_tcr2;
	uint32_t old_ttbr0[2];
	uint32_t old_ttbr1[2];
	uint32_t old_mair[2];
	uint32_t page_count;
	uint64_t next_iova;
	bool wide_sid;
	bool found;
	bool active;
};

static struct qca6390_smmu_state qca_smmu;
static hakios_pcie_lease_t qca_endpoint;
static bool qca_endpoint_owned;
static struct hakios_qca6390_dma_provider qca_dma_provider;
static void *qca_dma_context;
static bool qca_dma_registered;
static struct hakios_qca6390_msi_provider qca_msi_provider;
static void *qca_msi_context;
static bool qca_msi_registered;
static void *qca_msi_handle;
static struct hakios_qca6390_msi_info qca_msi_info;
static uint16_t qca_msi_capability;

static const struct hakios_pcie_endpoint_request qca_endpoint_request = {
	.domain = 0,
	.requester_id = QCA6390_REQUESTER_ID,
	.stream_id = QCA6390_STREAM_ID,
	.vendor = QCA6390_VENDOR,
	.device = QCA6390_DEVICE,
	.class_code = QCA6390_CLASS,
	.revision = QCA6390_REVISION,
	.bar0_address = QCA6390_BAR0,
	.bar0_size = QCA6390_BAR0_SIZE,
};

int
hakios_qca6390_dma_register(const struct hakios_qca6390_dma_provider *provider, void *context)
{
	if (provider == NULL || context == NULL || provider->version != HAKIOS_QCA6390_DMA_PROVIDER_VERSION ||
	    provider->status == NULL || provider->map == NULL || provider->stop == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	int error = qca_dma_registered ? EBUSY : 0;
	if (error == 0) {
		qca_dma_provider = *provider;
		qca_dma_context = context;
		qca_dma_registered = true;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_dma_unregister(void *context)
{
	lck_mtx_lock(&qca6390_pcie_lock);
	int error = !qca_dma_registered || context != qca_dma_context ? ENODEV :
	    qca_endpoint != 0 ? EBUSY : 0;
	if (error == 0) {
		bzero(&qca_dma_provider, sizeof(qca_dma_provider));
		qca_dma_context = NULL;
		qca_dma_registered = false;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_msi_register(const struct hakios_qca6390_msi_provider *provider, void *context)
{
	if (provider == NULL || context == NULL || provider->version != HAKIOS_QCA6390_MSI_PROVIDER_VERSION ||
	    provider->allocate == NULL || provider->attach == NULL || provider->enable == NULL ||
	    provider->detach_all == NULL || provider->count == NULL || provider->release == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	int error = qca_msi_registered ? EBUSY : 0;
	if (error == 0) {
		qca_msi_provider = *provider;
		qca_msi_context = context;
		qca_msi_registered = true;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_msi_unregister(void *context)
{
	lck_mtx_lock(&qca6390_pcie_lock);
	int error = !qca_msi_registered || context != qca_msi_context ? ENODEV :
	    qca_endpoint != 0 || qca_msi_handle != NULL ? EBUSY : 0;
	if (error == 0) {
		bzero(&qca_msi_provider, sizeof(qca_msi_provider));
		qca_msi_context = NULL;
		qca_msi_registered = false;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

bool
hakios_qca6390_platform_matches(void)
{
	static const char device_type[] = "munch";
	static const uint64_t expected_ranges[] = { 0, 0x00400000ull, 0x3fc00000ull };
	DTEntry arm_io;
	const void *value;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/arm-io", &arm_io) != kSuccess ||
	    SecureDTGetProperty(arm_io, "device_type", &value, &size) != kSuccess ||
	    size != sizeof(device_type) || memcmp(value, device_type, sizeof(device_type)) != 0 ||
	    SecureDTGetProperty(arm_io, "ranges", &value, &size) != kSuccess ||
	    size != sizeof(expected_ranges)) {
		return false;
	}
	return memcmp(value, expected_ranges, sizeof(expected_ranges)) == 0;
}

static int
qca_provider_error(int error)
{
	return error < 0 ? EIO : error;
}

int
hakios_qca6390_preflight(uint32_t required, uint32_t *missing)
{
	const uint32_t known_requirements = HAKIOS_QCA6390_RC0_HANDOFF |
	    HAKIOS_QCA6390_DMA_DOMAIN | HAKIOS_QCA6390_MSI_ROUTES;

	if (missing == NULL) {
		return EINVAL;
	}
	*missing = required;
	if (required == 0 || (required & ~known_requirements) != 0) {
		return EINVAL;
	}
	if (!hakios_qca6390_platform_matches()) {
		return ENODEV;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_endpoint_owned && hakios_pcie_endpoint_status(qca_endpoint) == 0) {
		*missing &= ~HAKIOS_QCA6390_RC0_HANDOFF;
	}
	if ((*missing & HAKIOS_QCA6390_DMA_DOMAIN) != 0 && qca_dma_registered && qca_endpoint_owned &&
	    qca_dma_provider.status(qca_dma_context, qca_endpoint) == 0) {
		*missing &= ~HAKIOS_QCA6390_DMA_DOMAIN;
	}
	if ((*missing & HAKIOS_QCA6390_MSI_ROUTES) != 0 && qca_msi_handle != NULL) {
		*missing &= ~HAKIOS_QCA6390_MSI_ROUTES;
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return *missing == 0 ? 0 : ENOTSUP;
}

static int
qca_require(uint32_t required)
{
	uint32_t missing;

	return hakios_qca6390_preflight(required, &missing);
}

static inline uint32_t
qca_rd(volatile uint32_t *base, uint32_t offset)
{
	return base[offset / sizeof(uint32_t)];
}

static inline void
qca_wr(volatile uint32_t *base, uint32_t offset, uint32_t value)
{
	base[offset / sizeof(uint32_t)] = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static int
qca_config_find_msi_locked(uint16_t *cap)
{
	uint32_t value, status;
	uint16_t next;
	bool visited[64] = { false };

	if (cap == NULL || hakios_pcie_config_read(qca_endpoint, 0x04, 4, &status) != 0 ||
	    ((status >> 16) & (1u << 4)) == 0 || hakios_pcie_config_read(qca_endpoint, 0x34, 4, &value) != 0) {
		return EIO;
	}
	next = value & 0xff;
	while (next != 0) {
		uint32_t header;
		unsigned index;

		if (next < 0x40 || next > 0xfc || (next & 3) != 0) {
			return EINVAL;
		}
		index = (next - 0x40) / 4;
		if (visited[index]) {
			return EINVAL;
		}
		visited[index] = true;
		if (hakios_pcie_config_read(qca_endpoint, next, 4, &header) != 0) {
			return EIO;
		}
		if ((header & 0xff) == 0x05) {
			*cap = next;
			return 0;
		}
		next = (header >> 8) & 0xff;
	}
	return ENODEV;
}

static volatile uint32_t *
qca_map_registers(uint64_t physical, uint32_t size)
{
	vm_offset_t mapped = ml_io_map_unmappable((vm_offset_t)physical, size, VM_WIMG_IO);

	return mapped == 0 ? NULL : (volatile uint32_t *)mapped;
}

static int
qca_release_locked(void)
{
	int error;

	if (qca_endpoint == 0) {
		return 0;
	}
	if (qca_msi_handle != NULL) {
		void *handle = qca_msi_handle;
		error = qca_provider_error(qca_msi_provider.release(qca_msi_context, &handle));
		if (error != 0 || handle != NULL) {
			return error != 0 ? error : EIO;
		}
		qca_msi_handle = NULL;
		bzero(&qca_msi_info, sizeof(qca_msi_info));
	}
	if (qca_dma_registered) {
		error = qca_provider_error(qca_dma_provider.stop(qca_dma_context, qca_endpoint));
		if (error != 0) {
			return error;
		}
	}
	error = hakios_pcie_endpoint_release(qca_endpoint);
	if (error == 0) {
		qca_endpoint = 0;
		qca_endpoint_owned = false;
	}
	return error;
}

static int
qca_claim_locked(void)
{
	uint16_t msi_cap;
	int error, release_error;

	if (qca_endpoint != 0) {
		error = hakios_pcie_endpoint_status(qca_endpoint);
		if (error != 0) {
			return error;
		}
		return qca_endpoint_owned ? 0 : EBUSY;
	}
	error = hakios_pcie_endpoint_acquire(&qca_endpoint_request, &qca_endpoint);
	if (error != 0) {
		return error;
	}
	if ((error = qca_config_find_msi_locked(&msi_cap)) != 0) {
		release_error = qca_release_locked();
		return release_error != 0 ? release_error : error;
	}
	qca_endpoint_owned = true;
	qca_msi_capability = msi_cap;
	printf("qca6390: owned RC0 endpoint %04x:%04x rev %02x, SID %#x, MSI cap %#x; bus mastering disabled\n",
	    QCA6390_VENDOR, QCA6390_DEVICE, QCA6390_REVISION, QCA6390_STREAM_ID, msi_cap);
	return 0;
}

int
hakios_qca6390_probe(void)
{
	bool already_claimed;
	int error, release_error;

	if (!hakios_qca6390_platform_matches()) {
		return ENODEV;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	already_claimed = qca_endpoint != 0;
	error = qca_claim_locked();
	if (!already_claimed && error == 0 && qca_endpoint != 0) {
		release_error = qca_release_locked();
		if (release_error != 0) {
			error = release_error;
		}
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	if (error == ENOTSUP) {
		printf("qca6390: RC0 ownership acquisition unsupported; probe refused\n");
	}
	return error;
}

int
hakios_qca6390_claim(void)
{
	int error;

	if (!hakios_qca6390_platform_matches()) {
		return ENODEV;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	error = qca_claim_locked();
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_release(void)
{
	int error;

	lck_mtx_lock(&qca6390_pcie_lock);
	error = qca_release_locked();
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

static int
qca_register_access(uint32_t offset, uint32_t *value, bool write)
{
	struct hakios_pcie_bar_operation operations[3] = { 0 };
	uint32_t data_index = 0;
	int error;

	if (value == NULL || (offset & 3) != 0 || offset >= QCA6390_REGISTER_LIMIT) {
		return EINVAL;
	}
	if (offset >= QCA6390_WINDOW_START) {
		uint32_t selector = QCA6390_WINDOW_ENABLE | ((offset >> QCA6390_WINDOW_SHIFT) & QCA6390_WINDOW_MASK);

		operations[0] = (struct hakios_pcie_bar_operation) {
			.type = HAKIOS_PCIE_BAR_WRITE32, .offset = QCA6390_WINDOW_REGISTER, .value = selector,
		};
		operations[1] = (struct hakios_pcie_bar_operation) {
			.type = HAKIOS_PCIE_BAR_VERIFY32, .offset = QCA6390_WINDOW_REGISTER, .value = selector,
			.mask = QCA6390_WINDOW_ENABLE | QCA6390_WINDOW_MASK,
		};
		data_index = 2;
	}
	operations[data_index] = (struct hakios_pcie_bar_operation) {
		.type = write ? HAKIOS_PCIE_BAR_WRITE32 : HAKIOS_PCIE_BAR_READ32,
		.offset = data_index == 0 ? offset : QCA6390_WINDOW_START + (offset & (QCA6390_WINDOW_START - 1)),
		.value = write ? *value : 0,
	};
	lck_mtx_lock(&qca6390_pcie_lock);
	if (!qca_endpoint_owned) {
		error = ENXIO;
	} else {
		error = hakios_pcie_bar0_transfer(qca_endpoint, operations, data_index + 1,
		    offset >= QCA6390_ACCESS_ALWAYS_LIMIT);
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	if (error == 0 && !write) {
		*value = operations[data_index].value;
	}
	return error;
}

int
hakios_qca6390_read32(uint32_t offset, uint32_t *value)
{
	if (value != NULL) {
		*value = 0;
	}
	return qca_register_access(offset, value, false);
}

int
hakios_qca6390_write32(uint32_t offset, uint32_t value)
{
	return qca_register_access(offset, &value, true);
}

static volatile uint32_t *
qca_context_map(uint32_t context, uint32_t page_size, uint32_t pages)
{
	uint64_t offset = (uint64_t)(pages + context) * page_size;

	return qca_map_registers(QCA6390_SMMU_PHYS + offset, QCA6390_PAGE_SIZE);
}

static uint64_t *
qca_table(uint32_t index)
{
	return (uint64_t *)(qca_smmu.tables + QCA6390_PAGE_SIZE * index);
}

static uint64_t *
qca_next_table(uint64_t *entry)
{
	if (*entry & 1) {
		for (uint32_t i = 0; i < qca_smmu.table_count; i++) {
			if (kvtophys((vm_offset_t)qca_table(i)) == (*entry & QCA6390_PTE_ADDRESS)) {
				return qca_table(i);
			}
		}
		return NULL;
	}
	if (qca_smmu.table_count == QCA6390_TABLE_COUNT) {
		return NULL;
	}
	uint64_t *table = qca_table(qca_smmu.table_count++);
	*entry = kvtophys((vm_offset_t)table) | 3;
	return table;
}

static bool
qca_map_pages(uint64_t iova, uint64_t physical, uint64_t size, uint64_t attributes)
{
	if (qca_smmu.tables == 0) {
		vm_size_t bytes = round_page(QCA6390_PAGE_SIZE * QCA6390_TABLE_COUNT);

		if (kmem_alloc(kernel_map, &qca_smmu.tables, bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return false;
		}
		qca_smmu.table_count = 1;
	}
	for (uint64_t offset = 0; offset < size; offset += QCA6390_PAGE_SIZE) {
		uint64_t at = iova + offset;
		uint64_t *level2 = qca_next_table(&qca_table(0)[at >> 30 & 0x1ff]);
		uint64_t *level3 = level2 == NULL ? NULL : qca_next_table(&level2[at >> 21 & 0x1ff]);

		if (level3 == NULL || physical + offset > QCA6390_PTE_ADDRESS) {
			return false;
		}
		level3[at >> 12 & 0x1ff] = (physical + offset) | attributes;
	}
	for (vm_offset_t line = qca_smmu.tables; line < qca_smmu.tables + QCA6390_PAGE_SIZE * QCA6390_TABLE_COUNT; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line));
	}
	__asm__ volatile("dsb sy" ::: "memory");
	return true;
}

static bool
qca_smmu_find(void)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts;
	uint32_t requested_mask = 0;

	qca_smmu.base = qca_map_registers(QCA6390_SMMU_PHYS, 0x2000);
	if (qca_smmu.base == NULL) {
		return false;
	}
	scr0 = qca_rd(qca_smmu.base, SMMU_SCR0);
	idr0 = qca_rd(qca_smmu.base, SMMU_IDR0);
	idr1 = qca_rd(qca_smmu.base, SMMU_IDR1);
	if (scr0 & 1) {
		printf("qca6390: apps SMMU is disabled\n");
		return false;
	}
	qca_smmu.wide_sid = (scr0 & SMMU_SCR0_EXIDENABLE) != 0;
	if (qca_smmu.wide_sid && (idr0 & SMMU_IDR0_EXIDS) == 0) {
		printf("qca6390: SMMU enables extended stream IDs without advertising EXIDS (IDR0 %#x, SCR0 %#x)\n", idr0, scr0);
		return false;
	}
	if (!qca_smmu.wide_sid && QCA6390_STREAM_ID > 0x7fff) {
		return false;
	}
	if ((idr1 & (1u << 31)) != 0 || (2u << (idr1 >> 28 & 7)) * 2u * QCA6390_PAGE_SIZE != 0x100000) {
		printf("qca6390: unsupported apps SMMU page geometry (IDR1 %#x)\n", idr1);
		return false;
	}
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	qca_smmu.match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = qca_rd(qca_smmu.base, SMMU_SMR(n));
		uint32_t s2cr = qca_rd(qca_smmu.base, SMMU_S2CR(n));
		bool active = qca_smmu.wide_sid ? (s2cr & SMMU_S2CR_VALID) != 0 : (smr >> 31) != 0;

		if (!active) {
			if (qca_smmu.match == ~0u) {
				qca_smmu.match = n;
				qca_smmu.old_smr = smr;
				qca_smmu.old_s2cr = s2cr;
			}
			continue;
		}
		uint32_t width_mask = qca_smmu.wide_sid ? 0xffffu : 0x7fffu;
		uint32_t stream = smr & width_mask;
		uint32_t mask = (smr >> 16) & width_mask;

		if (((stream ^ QCA6390_STREAM_ID) & ~(mask | requested_mask)) == 0) {
			printf("qca6390: active SMMU match %u overlaps endpoint SID %#x (SMR %#x, S2CR %#x); refusing takeover\n",
			    n, QCA6390_STREAM_ID, smr, s2cr);
			return false;
		}
		used[s2cr & 0xff] = true;
	}
	qca_smmu.context = 0;
	while (qca_smmu.context < contexts && used[qca_smmu.context]) {
		qca_smmu.context++;
	}
	if (qca_smmu.match == ~0u || qca_smmu.context >= contexts) {
		printf("qca6390: no unclaimed SMMU match/context pair for SID %#x\n", QCA6390_STREAM_ID);
		return false;
	}
	qca_smmu.second = qca_map_registers(QCA6390_SMMU_PHYS + 0x1000, 0x1000);
	qca_smmu.page_count = 2u << (idr1 >> 28 & 7);
	qca_smmu.context_regs = qca_context_map(qca_smmu.context, 0x1000, qca_smmu.page_count);
	if (qca_smmu.second == NULL || qca_smmu.context_regs == NULL) {
		return false;
	}
	qca_smmu.old_cbar = qca_rd(qca_smmu.second, SMMU_CBAR(qca_smmu.context));
	qca_smmu.old_cba2r = qca_rd(qca_smmu.second, SMMU_CBA2R(qca_smmu.context));
	qca_smmu.old_sctlr = qca_rd(qca_smmu.context_regs, SMMU_CB_SCTLR);
	qca_smmu.old_tcr2 = qca_rd(qca_smmu.context_regs, SMMU_CB_TCR2);
	qca_smmu.old_ttbr0[0] = qca_rd(qca_smmu.context_regs, SMMU_CB_TTBR0);
	qca_smmu.old_ttbr0[1] = qca_rd(qca_smmu.context_regs, SMMU_CB_TTBR0 + 4);
	qca_smmu.old_ttbr1[0] = qca_rd(qca_smmu.context_regs, SMMU_CB_TTBR1);
	qca_smmu.old_ttbr1[1] = qca_rd(qca_smmu.context_regs, SMMU_CB_TTBR1 + 4);
	qca_smmu.old_tcr = qca_rd(qca_smmu.context_regs, SMMU_CB_TCR);
	qca_smmu.old_mair[0] = qca_rd(qca_smmu.context_regs, SMMU_CB_MAIR0);
	qca_smmu.old_mair[1] = qca_rd(qca_smmu.context_regs, SMMU_CB_MAIR1);
	if (qca_smmu.old_sctlr & 1) {
		printf("qca6390: SMMU context %u is active; leaving it alone\n", qca_smmu.context);
		return false;
	}
	qca_smmu.next_iova = QCA6390_IOVA_BASE;
	qca_smmu.found = true;
	return true;
}

static bool
qca_smmu_enable(void)
{
	uint32_t smr = SMMU_SMR(qca_smmu.match), s2cr = SMMU_S2CR(qca_smmu.match);
	uint32_t stream_mask = qca_smmu.wide_sid ? 0xffffu : 0x7fffu;
	uint32_t smr_value = (QCA6390_STREAM_ID & stream_mask) | (qca_smmu.wide_sid ? 0 : 1u << 31);
	uint32_t s2cr_value = qca_smmu.context | (qca_smmu.wide_sid ? SMMU_S2CR_VALID : 0);
	uint64_t root = kvtophys(qca_smmu.tables) | (uint64_t)qca_smmu.context << 48;

	qca_wr(qca_smmu.context_regs, SMMU_CB_SCTLR, 0);
	qca_wr(qca_smmu.second, SMMU_CBA2R(qca_smmu.context), 1);
	qca_wr(qca_smmu.second, SMMU_CBAR(qca_smmu.context), SMMU_CBAR_STAGE_1);
	qca_wr(qca_smmu.context_regs, SMMU_CB_TCR2, SMMU_TCR2);
	qca_wr(qca_smmu.context_regs, SMMU_CB_TCR, SMMU_TCR_39_BITS);
	qca_wr(qca_smmu.context_regs, SMMU_CB_TTBR0, (uint32_t)root);
	qca_wr(qca_smmu.context_regs, SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	qca_wr(qca_smmu.context_regs, SMMU_CB_TTBR1, 0);
	qca_wr(qca_smmu.context_regs, SMMU_CB_TTBR1 + 4, 0);
	qca_wr(qca_smmu.context_regs, SMMU_CB_MAIR0, SMMU_MAIR0);
	qca_wr(qca_smmu.context_regs, SMMU_CB_MAIR1, 0);
	qca_wr(qca_smmu.context_regs, SMMU_CB_TLBIASID, qca_smmu.context);
	qca_wr(qca_smmu.context_regs, SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; (qca_rd(qca_smmu.context_regs, SMMU_CB_TLBSTATUS) & 1) != 0; ms++) {
		if (ms == 100) {
			return false;
		}
		delay(1);
	}
	qca_wr(qca_smmu.context_regs, SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	qca_wr(qca_smmu.base, s2cr, s2cr_value);
	qca_wr(qca_smmu.base, smr, smr_value);
	if (qca_rd(qca_smmu.base, smr) != smr_value ||
	    (qca_rd(qca_smmu.base, s2cr) & (0x300ff | SMMU_S2CR_VALID)) != s2cr_value ||
	    (qca_rd(qca_smmu.context_regs, SMMU_CB_SCTLR) & 1) == 0 ||
	    qca_rd(qca_smmu.context_regs, SMMU_CB_TTBR0) != (uint32_t)root ||
	    qca_rd(qca_smmu.context_regs, SMMU_CB_TTBR0 + 4) != (uint32_t)(root >> 32) ||
	    qca_rd(qca_smmu.second, SMMU_CBAR(qca_smmu.context)) != SMMU_CBAR_STAGE_1 ||
	    qca_rd(qca_smmu.second, SMMU_CBA2R(qca_smmu.context)) != 1) {
		return false;
	}
	qca_smmu.active = true;
	return true;
}

static bool
qca_smmu_invalidate(void)
{
	qca_wr(qca_smmu.context_regs, SMMU_CB_TLBIASID, qca_smmu.context);
	qca_wr(qca_smmu.context_regs, SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; ms < 100; ms++) {
		if ((qca_rd(qca_smmu.context_regs, SMMU_CB_TLBSTATUS) & 1) == 0) {
			return true;
		}
		delay(1);
	}
	return false;
}

static void
qca_smmu_restore(void)
{
	if (!qca_smmu.found || qca_smmu.base == NULL || qca_smmu.match == ~0u) {
		return;
	}
	if (qca_smmu.wide_sid) {
		qca_wr(qca_smmu.base, SMMU_S2CR(qca_smmu.match), qca_smmu.old_s2cr & ~SMMU_S2CR_VALID);
	} else {
		qca_wr(qca_smmu.base, SMMU_SMR(qca_smmu.match), qca_smmu.old_smr & ~(1u << 31));
	}
	if (qca_smmu.context_regs != NULL) {
		qca_wr(qca_smmu.context_regs, SMMU_CB_SCTLR, 0);
		qca_wr(qca_smmu.context_regs, SMMU_CB_TLBIASID, qca_smmu.context);
		qca_wr(qca_smmu.context_regs, SMMU_CB_TLBSYNC, 1);
		for (unsigned ms = 0; (qca_rd(qca_smmu.context_regs, SMMU_CB_TLBSTATUS) & 1) != 0 && ms < 100; ms++) {
			delay(1);
		}
	}
	if (qca_smmu.context_regs != NULL) {
		qca_wr(qca_smmu.context_regs, SMMU_CB_TCR2, qca_smmu.old_tcr2);
		qca_wr(qca_smmu.context_regs, SMMU_CB_TTBR0, qca_smmu.old_ttbr0[0]);
		qca_wr(qca_smmu.context_regs, SMMU_CB_TTBR0 + 4, qca_smmu.old_ttbr0[1]);
		qca_wr(qca_smmu.context_regs, SMMU_CB_TTBR1, qca_smmu.old_ttbr1[0]);
		qca_wr(qca_smmu.context_regs, SMMU_CB_TTBR1 + 4, qca_smmu.old_ttbr1[1]);
		qca_wr(qca_smmu.context_regs, SMMU_CB_TCR, qca_smmu.old_tcr);
		qca_wr(qca_smmu.context_regs, SMMU_CB_MAIR0, qca_smmu.old_mair[0]);
		qca_wr(qca_smmu.context_regs, SMMU_CB_MAIR1, qca_smmu.old_mair[1]);
	}
	if (qca_smmu.second != NULL) {
		qca_wr(qca_smmu.second, SMMU_CBAR(qca_smmu.context), qca_smmu.old_cbar);
		qca_wr(qca_smmu.second, SMMU_CBA2R(qca_smmu.context), qca_smmu.old_cba2r);
	}
	if (qca_smmu.context_regs != NULL) {
		qca_wr(qca_smmu.context_regs, SMMU_CB_SCTLR, qca_smmu.old_sctlr);
	}
	qca_wr(qca_smmu.base, SMMU_S2CR(qca_smmu.match), qca_smmu.old_s2cr);
	qca_wr(qca_smmu.base, SMMU_SMR(qca_smmu.match), qca_smmu.old_smr);
	qca_smmu.active = false;
}

static bool
qca_dma_map(uint64_t physical, uint64_t size, uint64_t attributes, uint64_t *iova)
{
	uint64_t rounded;
	bool result = false;

	if (iova != NULL) {
		*iova = 0;
	}
	if (qca_require(HAKIOS_QCA6390_RC0_HANDOFF | HAKIOS_QCA6390_DMA_DOMAIN) != 0) {
		return false;
	}
	if (!qca_endpoint_owned || !hakios_qca6390_platform_matches() || iova == NULL || size == 0 || size > QCA6390_IOVA_SIZE ||
	    size > UINT64_MAX - (QCA6390_PAGE_SIZE - 1) || (physical & (QCA6390_PAGE_SIZE - 1)) != 0 ||
	    physical > QCA6390_PTE_ADDRESS || size - 1 > QCA6390_PTE_ADDRESS - physical) {
		return false;
	}
	rounded = (size + QCA6390_PAGE_SIZE - 1) & ~(QCA6390_PAGE_SIZE - 1);
	lck_mtx_lock(&hakios_smmu_lock);
	if ((!qca_smmu.found && !qca_smmu_find()) || qca_smmu.next_iova + rounded > QCA6390_IOVA_BASE + QCA6390_IOVA_SIZE ||
	    !qca_map_pages(qca_smmu.next_iova, physical, rounded, attributes)) {
		goto out;
	}
	if (!qca_smmu.active) {
		if (!qca_smmu_enable()) {
			qca_smmu_restore();
			goto out;
		}
	} else if (!qca_smmu_invalidate()) {
		printf("qca6390: SMMU context %u TLB invalidation timed out; DMA mapping refused\n", qca_smmu.context);
		goto out;
	}
	*iova = qca_smmu.next_iova;
	qca_smmu.next_iova += rounded;
	result = true;
out:
	lck_mtx_unlock(&hakios_smmu_lock);
	return result;
}

bool
hakios_qca6390_dma_map(uint64_t physical, uint64_t size, uint64_t *iova)
{
	int error;

	if (iova != NULL) {
		*iova = 0;
	}
	if (iova == NULL || !hakios_qca6390_platform_matches()) {
		return false;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	error = qca_claim_locked();
	if (error == 0 && (!qca_dma_registered ||
	    qca_dma_provider.status(qca_dma_context, qca_endpoint) != 0)) {
		error = ENOTSUP;
	}
	if (error == 0) {
		error = qca_provider_error(qca_dma_provider.map(qca_dma_context, qca_endpoint, physical, size, iova));
		if (error == 0 && *iova == 0) {
			error = EPROTO;
		}
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error == 0;
}

bool
hakios_qca6390_msi(uint64_t *address, uint32_t *data, uint32_t *first_intid)
{
	int error;

	if (address != NULL) {
		*address = 0;
	}
	if (data != NULL) {
		*data = 0;
	}
	if (first_intid != NULL) {
		*first_intid = 0;
	}
	if (!hakios_qca6390_platform_matches()) {
		return false;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	error = qca_claim_locked();
	if (error == 0 && qca_msi_handle == NULL) {
		if (!qca_msi_registered) {
			error = ENOTSUP;
		} else {
			struct hakios_qca6390_msi_info info = { 0 };
			void *handle = NULL;
				error = qca_provider_error(qca_msi_provider.allocate(qca_msi_context, qca_endpoint, qca_msi_capability,
				    &info, &handle));
			if (error != 0 || handle == NULL || info.address == 0 || info.data == 0 ||
			    info.data > UINT16_MAX - (HAKIOS_MSI_VECTORS - 1) || (info.data & (HAKIOS_MSI_VECTORS - 1)) != 0 ||
			    info.first_intid < 32 || info.first_intid > 1019 - (HAKIOS_MSI_VECTORS - 1)) {
				if (handle != NULL) {
					int cleanup = qca_provider_error(qca_msi_provider.release(qca_msi_context, &handle));
					if (error == 0 && cleanup != 0) {
						error = cleanup;
					}
				}
				if (error == 0) {
					error = EPROTO;
				}
			}
			if (error == 0) {
				qca_msi_info = info;
				qca_msi_handle = handle;
			}
		}
	}
	if (error == 0) {
		if (address != NULL) {
			*address = qca_msi_info.address;
		}
		if (data != NULL) {
			*data = qca_msi_info.data;
		}
		if (first_intid != NULL) {
			*first_intid = qca_msi_info.first_intid;
		}
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error == 0;
}

int
hakios_qca6390_irq_attach(uint32_t vector, hakios_qca6390_irq_handler_t handler, void *context)
{
	int error;

	if (vector >= HAKIOS_MSI_VECTORS || handler == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_msi_handle == NULL || !qca_msi_registered) {
		error = ENXIO;
	} else {
		error = qca_provider_error(qca_msi_provider.attach(qca_msi_context, qca_msi_handle, vector, handler, context));
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_irq_enable(uint32_t vector, bool enable)
{
	int error;

	if (vector >= HAKIOS_MSI_VECTORS) {
		return EINVAL;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_msi_handle == NULL || !qca_msi_registered) {
		error = ENXIO;
	} else {
		error = qca_provider_error(qca_msi_provider.enable(qca_msi_context, qca_msi_handle, vector, enable));
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

int
hakios_qca6390_irq_detach_all(void)
{
	int error;

	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_msi_handle == NULL || !qca_msi_registered) {
		error = 0;
	} else {
		error = qca_provider_error(qca_msi_provider.detach_all(qca_msi_context, qca_msi_handle));
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return error;
}

uint64_t
hakios_qca6390_irq_count(uint32_t vector)
{
	uint64_t count = 0;

	if (vector >= HAKIOS_MSI_VECTORS) {
		return 0;
	}
	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_msi_handle != NULL && qca_msi_registered) {
		count = qca_msi_provider.count(qca_msi_context, qca_msi_handle, vector);
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
	return count;
}

void
hakios_qca6390_dma_stop(void)
{
	lck_mtx_lock(&qca6390_pcie_lock);
	if (qca_dma_registered && qca_endpoint != 0) {
		(void)qca_provider_error(qca_dma_provider.stop(qca_dma_context, qca_endpoint));
	}
	lck_mtx_unlock(&qca6390_pcie_lock);
}

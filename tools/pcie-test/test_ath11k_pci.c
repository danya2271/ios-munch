#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include <lx_qca_pci.h>

#ifndef PCIE_TEST_RUNTIME
struct hakios_dma_arena *hakios_dma_arena_get(void *device) { (void)device; assert(false); return NULL; }
void hakios_dma_arena_put(struct hakios_dma_arena *arena) { (void)arena; assert(false); }
int hakios_dma_arena_status(struct hakios_dma_arena *arena, struct hakios_dma_arena_status *status)
{ (void)arena; (void)status; assert(false); return EIO; }
#endif

enum fail_site {
	FAIL_NONE, FAIL_PREPARE, FAIL_ALLOC, FAIL_RESOURCES, FAIL_ID, FAIL_REVISION, FAIL_SUBSYSTEM,
	FAIL_BAR, FAIL_WORKER, FAIL_IPCR, FAIL_MSI, FAIL_PRE_INIT, FAIL_HAL, FAIL_CE, FAIL_BIND,
	FAIL_ATTACH, FAIL_CORE, FAIL_CLOSE, FAIL_FENCE, FAIL_UNBIND, FAIL_DETACH, FAIL_RELEASE,
};

struct hakios_msi { unsigned cookie; };
struct hakios_ipcr { unsigned cookie; };
struct hakios_mhi_worker { unsigned cookie; };
struct hakios_dma_endpoint { unsigned cookie; };
struct hakios_msi_allocation { unsigned cookie; };
struct lx_qca_hif { unsigned cookie; };

static struct {
	struct device device;
	struct hakios_msi msi;
	struct hakios_ipcr ipcr;
	struct hakios_mhi_worker worker;
	struct hakios_dma_endpoint dma;
	struct hakios_msi_allocation routes;
	struct lx_qca_hif hif;
	struct service_to_pipe services[2];
	struct ath11k_hw_ring_mask rings;
	void *pci_owner;
	struct hakios_pcie_endpoint_resources endpoint;
	struct hakios_mhi_worker_status worker_status;
	struct hakios_ipcr_status ipcr_status;
	struct ath11k_base *base;
	struct qmi_handle *binding;
	enum fail_site fail_site;
	int fail_error;
	unsigned fail_msi;
	unsigned wrong_msi;
	uint32_t identity, revision, version, instance;
	unsigned allocations, frees, core_deinit, special_deinit, cancelled, releases;
	bool fail_state_alloc, prepared, closed, fenced, released, irqs, ce, hal;
	bool bad_fence_worker, bad_fence_ipcr;
	bool refuse_fence;
	bool hif_closed;
} model;

static int
failure(enum fail_site site)
{
	if (model.fail_site != site) {
		return 0;
	}
	model.fail_site = FAIL_NONE;
	return model.fail_error;
}

static void
model_init(void)
{
	memset(&model, 0, sizeof(model));
	model.endpoint = (struct hakios_pcie_endpoint_resources) {
		.domain = 0, .requester_id = 0x100, .stream_id = 0x1c01,
		.bar0_address = 0x60300000, .bar0_size = 0x100000,
	};
	model.worker_status.epoch = 19;
	model.ipcr_status.epoch = 19;
	model.identity = 0x110117cb;
	model.revision = 0x02800001;
	model.version = 2u << 8;
	model.instance = 1;
	model.fail_error = EIO;
	model.wrong_msi = UINT32_MAX;
	model.pci_owner = &model;
}

void *
kzalloc(size_t size, unsigned flags)
{
	(void)flags;
	if (model.fail_state_alloc) {
		return NULL;
	}
	void *memory = calloc(1, size);
	assert(memory != NULL);
	model.allocations++;
	return memory;
}

void
kfree(void *memory)
{
	if (memory != NULL) {
		assert(model.fenced || !model.prepared);
		model.frees++;
		free(memory);
	}
}

static int
prepare(void *owner, struct device *device, struct lx_qca_pci_resources *resources)
{
	assert(owner == &model && device == &model.device && !model.prepared);
	model.prepared = true;
	*resources = (struct lx_qca_pci_resources) {
		.endpoint = 1, .msi = &model.msi, .ipcr = &model.ipcr, .worker = &model.worker,
		.dma = &model.dma, .routes = &model.routes, .hif = &model.hif,
	};
	return failure(FAIL_PREPARE);
}

int
hakios_pcie_endpoint_resources(hakios_pcie_lease_t lease, struct hakios_pcie_endpoint_resources *resources)
{
	assert(lease == 1 && model.prepared);
	*resources = model.endpoint;
	return failure(FAIL_RESOURCES);
}

int
hakios_pcie_config_read(hakios_pcie_lease_t lease, uint16_t offset, uint8_t width, uint32_t *value)
{
	assert(lease == 1 && width == 4 && model.prepared);
	enum fail_site site = offset == 0 ? FAIL_ID : offset == 8 ? FAIL_REVISION : FAIL_SUBSYSTEM;
	assert(offset == 0 || offset == 8 || offset == 0x2c);
	*value = offset == 0 ? model.identity : offset == 8 ? model.revision : 0x123417cb;
	return failure(site);
}

int
hakios_pcie_bar0_transfer(hakios_pcie_lease_t lease, struct hakios_pcie_bar_operation *batch, uint32_t count, bool wake)
{
	assert(lease == 1 && count == 1 && !wake);
	assert(batch->type == HAKIOS_PCIE_BAR_READ32 && batch->offset == 0x224);
	batch->value = model.version;
	return failure(FAIL_BAR);
}

int
hakios_mhi_worker_status(struct hakios_mhi_worker *worker, struct hakios_mhi_worker_status *status)
{
	assert(worker == &model.worker);
	*status = model.worker_status;
	return failure(FAIL_WORKER);
}

int
hakios_ipcr_status(struct hakios_ipcr *ipcr, struct hakios_ipcr_status *status)
{
	assert(ipcr == &model.ipcr);
	*status = model.ipcr_status;
	return failure(FAIL_IPCR);
}

int
hakios_msi_message(struct hakios_msi *msi, enum hakios_msi_user user, uint32_t index, struct hakios_msi_message *message)
{
	const unsigned bases[] = {0, 3, 13, 14}, sizes[] = {3, 10, 1, 18};
	assert(msi == &model.msi && user < 4 && index < sizes[user]);
	unsigned vector = bases[user] + index;
	*message = (struct hakios_msi_message) {.address = 0x123401000ULL, .data = 0x400 + vector, .intid = 0x100 + vector};
	if (vector == model.wrong_msi) {
		message->address += 4;
	}
	return vector == model.fail_msi ? failure(FAIL_MSI) : 0;
}

int
hakios_dma_endpoint_status(struct hakios_dma_endpoint *endpoint, struct hakios_dma_endpoint_status *status)
{
	assert(endpoint == &model.dma);
	*status = (struct hakios_dma_endpoint_status) {.endpoint = 1, .closed = model.fenced, .fenced = model.fenced};
	return 0;
}

int
hakios_msi_allocation_config(struct hakios_msi_allocation *allocation, hakios_pcie_lease_t endpoint,
    uint16_t capability, struct hakios_msi_config *config)
{
	assert(allocation == &model.routes && endpoint == 1);
	*config = (struct hakios_msi_config) {.doorbell_iova = 0x123401000ULL, .data = 0x400, .capability = capability};
	for (unsigned vector = 0; vector < 32; vector++) { config->intids[vector] = 0x100 + vector; }
	return 0;
}

struct ath11k_base *
ath11k_core_alloc(struct device *device, size_t private_size, int bus)
{
	assert(device == &model.device && bus == ATH11K_BUS_PCI && private_size == sizeof(void *));
	if (failure(FAIL_ALLOC) != 0) {
		return NULL;
	}
	model.base = kzalloc(sizeof(*model.base) + private_size, GFP_KERNEL);
	model.base->hif.bus = bus;
	model.base->qmi.event_work.tag = 0;
	model.base->reset_work.tag = 1;
	model.base->dump_work.tag = 2;
	model.base->restart_work.tag = 3;
	model.base->update_11d_work.tag = 4;
	return model.base;
}

int
ath11k_core_pre_init(struct ath11k_base *base)
{
	assert(base == model.base && lx_qca_pci_owner(base) == model.pci_owner);
	assert(base->hw_rev == ATH11K_HW_QCA6390_HW20 && base->fw_mode == ATH11K_FIRMWARE_MODE_NORMAL);
	assert(base->id.vendor == 0x17cb && base->id.device == 0x1101);
	assert(base->id.subsystem_vendor == 0x17cb && base->id.subsystem_device == 0x1234);
	assert(base->pci.msi.config->total_vectors == 32 && base->pci.msi.config->total_users == 4);
	assert(base->pci.msi.config->users[3].base_vector == 14 && base->pci.msi.config->users[3].num_vectors == 18);
	assert(base->pci.msi.addr_lo == 0x23401000 && base->pci.msi.addr_hi == 1 && base->pci.msi.ep_base_data == 0x400);
	assert(test_bit(ATH11K_FLAG_MULTI_MSI_VECTORS, &base->dev_flags));
	base->hw_params.qmi_service_ins_id = model.instance;
	base->hw_params.target_ce_count = 9;
	base->hw_params.svc_to_ce_map_len = 2;
	base->hw_params.target_ce_config = &model.ce;
	base->hw_params.svc_to_ce_map = model.services;
	base->hw_params.ring_mask = &model.rings;
	return -failure(FAIL_PRE_INIT);
}

int
ath11k_hal_srng_init(struct ath11k_base *base)
{
	base->hal.srng_config = kzalloc(8, GFP_KERNEL);
	int error = failure(FAIL_HAL);
	model.hal = error == 0;
	return -error;
}

void
ath11k_hal_srng_deinit(struct ath11k_base *base)
{
	assert(model.hal && model.fenced);
	model.hal = false;
	kfree(base->hal.srng_config);
	base->hal.srng_config = NULL;
}

int
ath11k_ce_alloc_pipes(struct ath11k_base *base)
{
	assert(base == model.base && model.hal);
	int error = failure(FAIL_CE);
	model.ce = error == 0;
	return -error;
}

void
ath11k_ce_free_pipes(struct ath11k_base *base)
{
	assert(base == model.base && model.ce && model.fenced && !model.irqs);
	model.ce = false;
}

int
hakios_ipcr_qmi_bind(struct hakios_ipcr *ipcr, struct qmi_handle *qmi)
{
	assert(ipcr == &model.ipcr && qmi == &model.base->qmi.handle && model.binding == NULL);
	int error = failure(FAIL_BIND);
	if (error == 0) {
		model.binding = qmi;
	}
	return error;
}

int
hakios_ipcr_qmi_unbind(struct hakios_ipcr *ipcr, struct qmi_handle *qmi)
{
	assert(ipcr == &model.ipcr && qmi == model.binding && qmi->sock == NULL && model.fenced);
	int error = failure(FAIL_UNBIND);
	if (error == 0) {
		model.binding = NULL;
	}
	return error;
}

static int
attach(void *owner, struct ath11k_base *base)
{
	assert(owner == &model && base == model.base && model.binding != NULL);
	model.irqs = true;
	return failure(FAIL_ATTACH);
}

int
ath11k_core_init(struct ath11k_base *base)
{
	assert(base == model.base && model.irqs && model.binding == &base->qmi.handle);
	assert(base->qmi.service_ins_id == 1 && base->qmi.ce_cfg.tgt_ce_len == 9 && base->qmi.ce_cfg.svc_to_ce_map_len == 2);
	assert(base->qmi.ce_cfg.tgt_ce == &model.ce && base->qmi.ce_cfg.svc_to_ce_map == model.services);
	base->qmi.handle.sock = kzalloc(8, GFP_KERNEL);
	return -failure(FAIL_CORE);
}

static int
close_admission(void *owner)
{
	assert(owner == &model && model.prepared);
	if (model.base != NULL) {
		assert(test_bit(ATH11K_FLAG_UNREGISTERING, &model.base->dev_flags));
	}
	model.closed = true;
	return failure(FAIL_CLOSE);
}

static int
fence(void *owner)
{
	assert(owner == &model && model.closed && model.hif_closed);
	if (model.refuse_fence) {
		return ETIMEDOUT;
	}
	model.worker_status.quiet = true;
	model.worker_status.irq_quiesced = !model.bad_fence_worker;
	model.ipcr_status.stopped = true;
	model.ipcr_status.quiesced = !model.bad_fence_ipcr;
	model.fenced = true;
	return failure(FAIL_FENCE);
}

#ifndef PCIE_TEST_RUNTIME
int
lx_qca_hif_close(struct lx_qca_hif *hif)
{
	assert(hif == &model.hif && model.closed);
	model.hif_closed = true;
	return 0;
}
#endif

bool
cancel_work_sync(struct work_struct *work)
{
	assert(model.closed && model.fenced && work->tag < 5);
	model.cancelled |= 1u << work->tag;
	return false;
}

int
del_timer_sync(struct timer_list *timer)
{
	assert(timer == &model.base->rx_replenish_retry && model.fenced);
	return 0;
}

void
qmi_handle_release(struct qmi_handle *qmi)
{
	assert(qmi == model.binding && model.fenced);
	kfree(qmi->sock);
	qmi->sock = NULL;
}

void
ath11k_core_deinit(struct ath11k_base *base)
{
	assert(base == model.base && model.fenced && model.cancelled == 31 && model.core_deinit == 0);
	model.core_deinit++;
	qmi_handle_release(&base->qmi.handle);
}

void
ath11k_debugfs_soc_destroy(struct ath11k_base *base)
{
	assert(base == model.base && model.fenced && model.cancelled == 31);
}

void
ath11k_qmi_deinit_service(struct ath11k_base *base)
{
	assert(base == model.base && model.special_deinit == 0);
	model.special_deinit++;
	qmi_handle_release(&base->qmi.handle);
}

void
ath11k_core_pm_notifier_unregister(struct ath11k_base *base)
{
	assert(base == model.base && model.special_deinit == 1);
}

void
ath11k_fw_destroy(struct ath11k_base *base)
{
	assert(base == model.base && model.fenced && !model.irqs);
}

void
ath11k_core_free(struct ath11k_base *base)
{
	assert(base == model.base && !model.hal && !model.ce && !model.irqs && model.binding == NULL);
	kfree(base);
	model.base = NULL;
}

static int
detach(void *owner)
{
	assert(owner == &model && model.fenced && model.binding == NULL);
	model.irqs = false;
	return failure(FAIL_DETACH);
}

static int
release(void *owner)
{
	assert(owner == &model && model.base == NULL && model.binding == NULL && !model.irqs && model.fenced);
	model.releases++;
	model.released = true;
	return failure(FAIL_RELEASE);
}

static uint32_t read32(struct ath11k_base *base, uint32_t address) { (void)base; (void)address; abort(); }
static void write32(struct ath11k_base *base, uint32_t address, uint32_t value) { (void)base; (void)address; (void)value; abort(); }
static int read_block(struct ath11k_base *base, void *bytes, uint32_t start, uint32_t end)
{ (void)base; (void)bytes; (void)start; (void)end; abort(); }
static void hif_void(struct ath11k_base *base) { (void)base; abort(); }
static int hif_int(struct ath11k_base *base) { (void)base; abort(); }
static void power_down(struct ath11k_base *base, bool suspend) { assert(base == model.base && model.fenced && !suspend); }
static int map_pipe(struct ath11k_base *base, uint16_t service, uint8_t *uplink, uint8_t *downlink)
{ (void)base; (void)service; (void)uplink; (void)downlink; abort(); }
static int msi_vector(struct ath11k_base *base, char *name, int *count, uint32_t *data, uint32_t *vector)
{ (void)base; (void)name; (void)count; (void)data; (void)vector; abort(); }
static void msi_address(struct ath11k_base *base, uint32_t *low, uint32_t *high)
{ (void)base; (void)low; (void)high; abort(); }
static void ce_msi(struct ath11k_base *base, uint32_t ce_id, uint32_t *index)
{ (void)base; (void)ce_id; (void)index; abort(); }

static const struct ath11k_hif_ops hif = {
	.read32 = read32, .write32 = write32, .read = read_block,
	.irq_enable = hif_void, .irq_disable = hif_void, .start = hif_int, .stop = hif_void,
	.power_up = hif_int, .power_down = power_down, .suspend = hif_int, .resume = hif_int,
	.map_service_to_pipe = map_pipe, .get_user_msi_vector = msi_vector, .get_msi_address = msi_address,
	.ce_irq_enable = hif_void, .ce_irq_disable = hif_void, .get_ce_msi_idx = ce_msi,
};
static const struct lx_qca_pci_operations operations = {
	.version = LX_QCA_PCI_VERSION, .prepare = prepare, .attach_irqs = attach, .close_admission = close_admission,
	.fence = fence, .detach_irqs = detach, .release = release,
};

static void
check_clean(struct lx_qca_pci *state)
{
	assert(state == NULL && model.base == NULL && model.binding == NULL && model.released);
	assert(!model.hal && !model.ce && !model.irqs && model.allocations == model.frees);
}

static void
test_success(void)
{
	for (unsigned special = 0; special < 2; special++) {
		model_init();
		struct lx_qca_pci *state;
		assert(lx_qca_pci_probe(&model.device, &hif, &operations, &model, &state) == 0 && state != NULL);
		assert(lx_qca_pci_owner(model.base) == &model && !model.closed);
		if (special != 0) {
			set_bit(ATH11K_FLAG_QMI_FAIL, &model.base->dev_flags);
		}
		assert(lx_qca_pci_remove(&state) == 0);
		assert(model.core_deinit == 1 - special && model.special_deinit == special);
		check_clean(state);
	}
}

static void
test_probe_failures(void)
{
	unsigned injected = 0;
	for (unsigned site = FAIL_PREPARE; site <= FAIL_CORE; site++) {
		model_init();
		model.fail_site = site;
		struct lx_qca_pci *state;
		assert(lx_qca_pci_probe(&model.device, &hif, &operations, &model, &state) < 0);
		assert(model.fail_site == FAIL_NONE);
		check_clean(state);
		injected++;
	}
	for (unsigned vector = 0; vector < 32; vector++) {
		model_init();
		model.fail_site = FAIL_MSI;
		model.fail_msi = vector;
		struct lx_qca_pci *state;
		assert(lx_qca_pci_probe(&model.device, &hif, &operations, &model, &state) == -EIO);
		check_clean(state);
		injected++;
	}
	for (unsigned partial = 0; partial < 2; partial++) {
		model_init();
		model.fail_site = partial == 0 ? FAIL_PREPARE : FAIL_CORE;
		model.refuse_fence = true;
		struct lx_qca_pci *state;
		assert(lx_qca_pci_probe(&model.device, &hif, &operations, &model, &state) == -ETIMEDOUT && state != NULL);
		assert(model.frees == 0 && !model.fenced && model.cancelled == 0);
		model.refuse_fence = false;
		assert(lx_qca_pci_remove(&state) == 0);
		check_clean(state);
		injected++;
	}
	for (unsigned site = FAIL_CLOSE; site <= FAIL_RELEASE; site++) {
		for (unsigned negative = 0; negative < 2; negative++) {
			model_init();
			struct lx_qca_pci *state;
			assert(lx_qca_pci_probe(&model.device, &hif, &operations, &model, &state) == 0);
			model.fail_site = site;
			model.fail_error = negative != 0 ? -ETIMEDOUT : ETIMEDOUT;
			assert(lx_qca_pci_remove(&state) == (negative != 0 ? -EIO : -ETIMEDOUT) && state != NULL);
			if (site <= FAIL_FENCE) {
				assert(model.frees == 0 && model.core_deinit == 0 && model.cancelled == 0);
			}
			assert(lx_qca_pci_remove(&state) == 0);
			assert(model.core_deinit == 1);
			check_clean(state);
			injected++;
		}
	}
	printf("ath11k-pci lifecycle failures: %u\n", injected);
}

static void
test_rejections(void)
{
	for (unsigned scenario = 0; scenario < 13; scenario++) {
		model_init();
		switch (scenario) {
		case 0: model.endpoint.domain = 1; break;
		case 1: model.endpoint.requester_id = 0; break;
		case 2: model.endpoint.stream_id = 0x1c00; break;
		case 3: model.endpoint.bar0_address++; break;
		case 4: model.endpoint.bar0_size--; break;
		case 5: model.identity++; break;
		case 6: model.revision++; break;
		case 7: model.version = 3u << 8; break;
		case 8: model.worker_status.epoch++; break;
		case 9: model.worker_status.fault = EIO; break;
		case 10: model.ipcr_status.stopped = true; break;
		case 11: model.instance = 2; break;
		case 12: model.wrong_msi = 20; break;
		}
		struct lx_qca_pci *state;
		assert(lx_qca_pci_probe(&model.device, &hif, &operations, &model, &state) < 0);
		check_clean(state);
	}
	for (unsigned bad_ipcr = 0; bad_ipcr < 2; bad_ipcr++) {
		model_init();
		struct lx_qca_pci *state;
		assert(lx_qca_pci_probe(&model.device, &hif, &operations, &model, &state) == 0);
		model.bad_fence_worker = bad_ipcr == 0;
		model.bad_fence_ipcr = bad_ipcr != 0;
		assert(lx_qca_pci_remove(&state) == -EBUSY && state != NULL && model.frees == 0);
		model.bad_fence_worker = model.bad_fence_ipcr = false;
		assert(lx_qca_pci_remove(&state) == 0);
		check_clean(state);
	}
	model_init();
	struct lx_qca_pci *state;
	struct ath11k_hif_ops missing_hif = hif;
	missing_hif.power_up = NULL;
	assert(lx_qca_pci_probe(&model.device, &missing_hif, &operations, &model, &state) == -EINVAL && state == NULL);
	struct lx_qca_pci_operations missing_owner = operations;
	missing_owner.close_admission = NULL;
	assert(lx_qca_pci_probe(&model.device, &hif, &missing_owner, &model, &state) == -EINVAL && state == NULL);
	assert(!model.prepared && model.allocations == 0);
	model.fail_state_alloc = true;
	assert(lx_qca_pci_probe(&model.device, &hif, &operations, &model, &state) == -ENOMEM && state == NULL);
	assert(lx_qca_pci_remove(&state) == -EINVAL && lx_qca_pci_owner(NULL) == NULL);
}

int
main(void)
{
	test_success();
	test_probe_failures();
	test_rejections();
	puts("ath11k-pci-test: PASS (actual adapter, mocked driver/native owner, fenced rollback and retry)");
	return 0;
}

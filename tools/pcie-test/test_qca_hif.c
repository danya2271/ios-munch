#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include "core.h"
#include <lx_qca_hif.h>

int hakios_dma_arena_status(struct hakios_dma_arena *arena, struct hakios_dma_arena_status *status)
{ (void)arena; (void)status; assert(false); return EIO; }

struct hakios_msi { unsigned cookie; };
struct hakios_dma_endpoint { unsigned cookie; };

static struct {
	struct lx_qca_hif *hif;
	struct ath11k_base base;
	struct hakios_msi msi;
	struct hakios_dma_endpoint dma;
	struct ath11k_hw_ring_mask rings;
	struct service_to_pipe services[2];
	uint32_t enabled, last_ce, last_group, selector, offset, written;
	unsigned registers, faults, posts, cleaned, powers, drains;
	int fail_register, fail_irq, fail_drain, fail_power, dp_done;
	int dma_fault;
	bool wake, closed;
	atomic_bool block_power, entered_power, release_power, active_power, close_entered, close_done;
} model;

void *kzalloc(size_t size, unsigned flags) { (void)flags; return calloc(1, size); }
void kfree(void *memory) { free(memory); }

int
lx_qca_pci_resources(struct ath11k_base *base, struct lx_qca_pci_resources *resources)
{
	assert(base == &model.base);
	*resources = (struct lx_qca_pci_resources) {.endpoint = 7, .hif = model.hif, .msi = &model.msi, .dma = &model.dma};
	return 0;
}

int
hakios_pcie_bar0_transfer(hakios_pcie_lease_t endpoint, struct hakios_pcie_bar_operation *batch,
    uint32_t count, bool wake)
{
	assert(endpoint == 7 && !model.closed && (count == 1 || count == 3));
	model.registers++;
	model.wake = wake;
	if (model.fail_register != 0) {
		int error = model.fail_register;
		model.fail_register = 0;
		return error;
	}
	if (count == 3) {
		assert(batch[0].type == HAKIOS_PCIE_BAR_WRITE32 && batch[0].offset == 0x310c);
		assert(batch[1].type == HAKIOS_PCIE_BAR_VERIFY32 && batch[1].value == batch[0].value && batch[1].mask == 0x4000003f);
		model.selector = batch[0].value;
	}
	model.offset = batch[count - 1].offset;
	model.written = batch[count - 1].value;
	if (batch[count - 1].type == HAKIOS_PCIE_BAR_READ32) { batch[count - 1].value = 0x12345678; }
	return 0;
}

int
hakios_dma_endpoint_status(struct hakios_dma_endpoint *endpoint, struct hakios_dma_endpoint_status *status)
{
	assert(endpoint == &model.dma);
	*status = (struct hakios_dma_endpoint_status) {.endpoint = 7, .fault = model.dma_fault};
	return 0;
}

int
hakios_msi_message(struct hakios_msi *msi, enum hakios_msi_user user, uint32_t index, struct hakios_msi_message *message)
{
	const uint32_t starts[] = {0, 3, 13, 14};
	assert(msi == &model.msi && user < 4 && index == 0);
	*message = (struct hakios_msi_message) {.address = 0x12000040, .data = 0x400 + starts[user], .intid = 800 + starts[user]};
	return 0;
}

uint32_t ath11k_ce_get_attr_flags(struct ath11k_base *base, uint32_t ce)
{ assert(base == &model.base); return ce == 1 ? CE_ATTR_DIS_INTR : 0; }
void ath11k_ce_rx_post_buf(struct ath11k_base *base)
{ assert(base == &model.base); model.posts++; }
void ath11k_ce_cleanup_pipes(struct ath11k_base *base)
{ assert(base == &model.base && (model.enabled & 0x1ff8) == 0); model.cleaned++; }
void ath11k_ce_per_engine_service(struct ath11k_base *base, uint16_t ce)
{
	assert(base == &model.base);
	model.last_ce = ce;
	assert(base->hif.ops->read32(base, 0x224) == 0x12345678);
}
int ath11k_dp_service_srng(struct ath11k_base *base, struct ath11k_ext_irq_grp *group, int budget)
{ assert(base == &model.base && group->ab == base && budget > 0); model.last_group = group->grp_id; return model.dp_done; }

static int power(void *owner, enum lx_qca_power operation)
{
	assert(owner == &model && operation <= LX_QCA_POWER_RESUME);
	model.powers++;
	if (atomic_load(&model.block_power)) {
		atomic_store(&model.active_power, true);
		atomic_store(&model.entered_power, true);
		while (!atomic_load(&model.release_power)) { sched_yield(); }
		atomic_store(&model.active_power, false);
	}
	int error = model.fail_power;
	model.fail_power = 0;
	return error;
}
static int irqs(void *owner, uint32_t mask, bool enable)
{
	assert(owner == &model);
	assert(!atomic_load(&model.active_power));
	int error = model.fail_irq;
	model.fail_irq = 0;
	if (error == 0) { model.enabled = enable ? model.enabled | mask : model.enabled & ~mask; }
	return error;
}
static int drain(void *owner, uint32_t mask)
{
	assert(owner == &model && (model.enabled & mask) == 0);
	model.drains++;
	int error = model.fail_drain;
	model.fail_drain = 0;
	return error;
}
static void fault(void *owner, int error)
{ assert(owner == &model && error > 0); model.faults++; }

static const struct lx_qca_hif_operations native_ops = {
	.version = LX_QCA_HIF_VERSION, .power = power, .irqs = irqs, .drain = drain, .fault = fault,
};

static void
model_init(void)
{
	memset(&model, 0, sizeof(model));
	atomic_init(&model.block_power, false);
	atomic_init(&model.entered_power, false);
	atomic_init(&model.release_power, false);
	atomic_init(&model.active_power, false);
	atomic_init(&model.close_entered, false);
	atomic_init(&model.close_done, false);
	model.base.hif.ops = lx_qca_hif_operations();
	model.base.hif.bus = ATH11K_BUS_PCI;
	model.base.hw_rev = ATH11K_HW_QCA6390_HW20;
	model.base.hw_params.ce_count = 9;
	model.rings.rx[2] = 1;
	model.rings.tx[10] = 1;
	model.base.hw_params.ring_mask = &model.rings;
	model.services[0] = (struct service_to_pipe) {42, PIPEDIR_OUT, 0};
	model.services[1] = (struct service_to_pipe) {42, PIPEDIR_IN, 2};
	model.base.hw_params.svc_to_ce_map = model.services;
	model.base.hw_params.svc_to_ce_map_len = 2;
	assert(lx_qca_hif_create(&native_ops, &model, &model.hif) == 0);
	assert(lx_qca_hif_status(model.hif) == -ENOTCONN);
	assert(lx_qca_hif_bind(model.hif, &model.base) == 0);
}

static void
test_hif(void)
{
	model_init();
	struct ath11k_base *base = &model.base;
	const struct ath11k_hif_ops *ops = base->hif.ops;
	assert(ops->read32(base, 0x224) == 0x12345678 && !model.wake);
	ops->write32(base, 0x1b23454, 0xabcdef);
	assert(model.selector == (0x40000000 | (0x1b23454 >> 19)) && model.offset == (0x80000 | (0x1b23454 & 0x7ffff)));
	assert(model.written == 0xabcdef && model.wake);
	uint8_t buffer[9];
	memset(buffer, 0xff, sizeof(buffer));
	assert(ops->read(base, buffer + 1, 0x220, 0x224) == 0 && buffer[0] == 0xff && buffer[1] == 0x78);
	uint8_t up, down;
	assert(ops->map_service_to_pipe(base, 42, &up, &down) == 0 && up == 0 && down == 2);
	assert(ops->map_service_to_pipe(base, 99, &up, &down) == -ENOENT);
	int count;
	uint32_t data, first;
	assert(ops->get_user_msi_vector(base, "DP", &count, &data, &first) == 0 && count == 18 && data == 0x40e && first == 14);
	ops->get_ce_msi_idx(base, 2, &first);
	assert(first == 1);
	assert(ops->power_up(base) == 0 && ops->suspend(base) == 0 && ops->resume(base) == 0);
	assert(ops->start(base) == 0 && model.posts == 1 && model.enabled == 0x7f8);
	ops->irq_enable(base);
	assert(model.enabled == (0x7f8 | (1u << 16) | (1u << 24)));
	uint32_t work;
	bool more;
	assert(lx_qca_hif_service(model.hif, 4, 8, &work, &more) == 0 && model.last_ce == 2 && !more);
	model.dp_done = 8;
	assert(lx_qca_hif_service(model.hif, 16, 8, &work, &more) == 0 && work == 8 && more && model.last_group == 2);
	model.dp_done = 3;
	assert(lx_qca_hif_service(model.hif, 24, 8, &work, &more) == 0 && work == 3 && !more && model.last_group == 10);
	assert(lx_qca_hif_service(model.hif, 13, 8, &work, &more) == -EAGAIN);
	ops->irq_disable(base);
	ops->stop(base);
	assert(model.enabled == 0 && model.cleaned == 1);
	assert(lx_qca_hif_close(model.hif) == 0);
	model.closed = true;
	unsigned powers = model.powers;
	assert(ops->power_up(base) == -ESHUTDOWN && model.powers == powers);
	assert(lx_qca_hif_destroy(&model.hif) == 0 && model.hif == NULL);
}

static void
*power_thread(void *unused)
{
	(void)unused;
	assert(model.base.hif.ops->power_up(&model.base) == 0);
	return NULL;
}

static void
*close_thread(void *unused)
{
	(void)unused;
	atomic_store(&model.close_entered, true);
	assert(lx_qca_hif_close(model.hif) == 0);
	atomic_store(&model.close_done, true);
	return NULL;
}

static void
test_close_race(void)
{
	model_init();
	atomic_store(&model.block_power, true);
	pthread_t power_worker, closing_worker;
	assert(pthread_create(&power_worker, NULL, power_thread, NULL) == 0);
	while (!atomic_load(&model.entered_power)) { sched_yield(); }
	assert(pthread_create(&closing_worker, NULL, close_thread, NULL) == 0);
	while (!atomic_load(&model.close_entered)) { sched_yield(); }
	assert(!atomic_load(&model.close_done));
	atomic_store(&model.release_power, true);
	assert(pthread_join(power_worker, NULL) == 0 && pthread_join(closing_worker, NULL) == 0);
	assert(atomic_load(&model.close_done));
	assert(lx_qca_hif_destroy(&model.hif) == 0);
}

static void
test_bind_retry(void)
{
	model_init();
	assert(lx_qca_hif_destroy(&model.hif) == 0);
	assert(lx_qca_hif_create(&native_ops, &model, &model.hif) == 0);
	memset(model.base.ext_irq_grp, 0, sizeof(model.base.ext_irq_grp));
	model.base.hw_params.ce_count = 12;
	assert(lx_qca_hif_bind(model.hif, &model.base) == -EOVERFLOW);
	assert(lx_qca_hif_status(model.hif) == -ENOTCONN);
	assert(model.base.hif.ops->start(&model.base) == -ENOTCONN);
	assert(model.enabled == 0 && model.posts == 0);
	for (unsigned group = 0; group < ATH11K_EXT_IRQ_GRP_NUM_MAX; group++) {
		assert(model.base.ext_irq_grp[group].ab == NULL);
	}
	model.base.hw_params.ce_count = 9;
	assert(lx_qca_hif_bind(model.hif, &model.base) == 0);
	assert(model.base.hif.ops->start(&model.base) == 0 && model.enabled == 0x7f8);
	uint32_t work;
	bool more;
	assert(lx_qca_hif_service(model.hif, 11, 8, &work, &more) == -EAGAIN);
	assert(lx_qca_hif_service(model.hif, 12, 8, &work, &more) == -EAGAIN);
	assert(lx_qca_hif_destroy(&model.hif) == 0);
}

static void
test_errors(void)
{
	for (unsigned scenario = 0; scenario < 6; scenario++) {
		model_init();
		const struct ath11k_hif_ops *ops = model.base.hif.ops;
		if (scenario == 0) {
			model.fail_register = EIO;
			assert(ops->read32(&model.base, 0x224) == UINT32_MAX);
		} else if (scenario == 1) {
			model.fail_irq = EIO;
			assert(ops->start(&model.base) == -EIO && model.posts == 0);
		} else if (scenario == 2) {
			model.fail_power = -EINVAL;
			assert(ops->power_up(&model.base) == -EIO);
		} else if (scenario == 3) {
			model.fail_drain = EIO;
			assert(lx_qca_hif_destroy(&model.hif) == -EIO && model.hif != NULL);
		} else if (scenario == 4) {
			ops->irq_enable(&model.base);
			model.dp_done = -1;
			uint32_t work;
			bool more;
			assert(lx_qca_hif_service(model.hif, 16, 4, &work, &more) == -EIO);
		} else {
			model.dma_fault = EIO;
			assert(ops->start(&model.base) == -EIO);
		}
		assert(lx_qca_hif_status(model.hif) == -EIO && model.faults == 1);
		unsigned reads = model.registers;
		assert(ops->read32(&model.base, 0x224) == UINT32_MAX && model.registers == reads && model.faults == 1);
		assert(lx_qca_hif_destroy(&model.hif) == 0);
	}
}

int main(void)
{
	test_hif();
	test_bind_retry();
	test_errors();
	test_close_race();
	puts("qca-hif-test: PASS (native BAR batches, granted MSI layout, actual HIF/CE/DP dispatch with mocked driver)");
	return 0;
}

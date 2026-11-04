#if defined(CONFIG_ATH11K_PCI)

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include "core.h"
#include "ce.h"
#include "dp.h"
#include "hif.h"
#include "htc.h"
#include <lx_qca_hif.h>

#define mutex_destroy(_mutex) ((void)0)

struct lx_qca_hif {
	struct mutex gate;
	struct mutex service;
	struct mutex lifecycle;
	struct lx_qca_hif_operations operations;
	struct lx_qca_pci_resources resources;
	struct ath11k_base *driver;
	void *owner;
	uint32_t ce_mask, dp_mask, enabled;
	int ce[32], group[32];
	int fault;
	bool closed, drained;
};

static struct lx_qca_hif *
hif_for(struct ath11k_base *base)
{
	struct lx_qca_pci_resources resources;
	return lx_qca_pci_resources(base, &resources) == 0 ? resources.hif : NULL;
}

static int
hif_fail(struct lx_qca_hif *hif, int error)
{
	if (error == 0) {
		return 0;
	}
	if (error < 0) {
		error = EIO;
	}
	mutex_lock(&hif->gate);
	bool notify = hif->fault == 0;
	if (notify) {
		hif->fault = error;
	}
	mutex_unlock(&hif->gate);
	if (notify) {
		hif->operations.fault(hif->owner, error);
	}
	return -error;
}

int
lx_qca_hif_status(struct lx_qca_hif *hif)
{
	if (hif == NULL) {
		return -ENODEV;
	}
	mutex_lock(&hif->gate);
	int error = hif->fault != 0 ? -hif->fault : hif->closed ? -ESHUTDOWN : hif->driver == NULL ? -ENOTCONN : 0;
	mutex_unlock(&hif->gate);
	if (error == 0 && hif->resources.arena != NULL) {
		struct hakios_dma_arena_status status;
		int check = hakios_dma_arena_status(hif->resources.arena, &status);
		if (check == 0) { check = status.fault != 0 ? status.fault : status.closed ? ESHUTDOWN : 0; }
		if (check != 0) { error = hif_fail(hif, check); }
	}
	return error;
}

static int
hif_register(struct lx_qca_hif *hif, uint32_t offset, uint32_t *value, bool write)
{
	if (hif == NULL || value == NULL || (offset & 3) != 0 || offset >= (1u << 25)) {
		return -EINVAL;
	}
	struct hakios_pcie_bar_operation batch[3];
	uint32_t count = 0;
	if (offset >= 0x80000) {
		uint32_t selector = 0x40000000u | ((offset >> 19) & 63);
		batch[count++] = (struct hakios_pcie_bar_operation) {HAKIOS_PCIE_BAR_WRITE32, 0x310c, selector, 0};
		batch[count++] = (struct hakios_pcie_bar_operation) {HAKIOS_PCIE_BAR_VERIFY32, 0x310c, selector, 0x4000003f};
	}
	batch[count++] = (struct hakios_pcie_bar_operation) {
		.type = write ? HAKIOS_PCIE_BAR_WRITE32 : HAKIOS_PCIE_BAR_READ32,
		.offset = offset < 0x80000 ? offset : 0x80000 + (offset & 0x7ffff), .value = write ? *value : 0,
	};
	mutex_lock(&hif->gate);
	int error = hif->fault != 0 ? hif->fault : hif->closed ? ESHUTDOWN :
	    hakios_pcie_bar0_transfer(hif->resources.endpoint, batch, count, offset >= 0xfe0);
	mutex_unlock(&hif->gate);
	if (error == 0 && !write) {
		*value = batch[count - 1].value;
	}
	return hif_fail(hif, error);
}

static uint32_t
hif_read32(struct ath11k_base *base, uint32_t offset)
{
	uint32_t value = UINT32_MAX;
	struct lx_qca_hif *hif = hif_for(base);
	int error = hif_register(hif, offset, &value, false);
	if (hif != NULL && error != 0) {
		hif_fail(hif, -error);
	}
	return error == 0 ? value : UINT32_MAX;
}

static void
hif_write32(struct ath11k_base *base, uint32_t offset, uint32_t value)
{
	struct lx_qca_hif *hif = hif_for(base);
	int error = hif_register(hif, offset, &value, true);
	if (hif != NULL && error != 0) {
		hif_fail(hif, -error);
	}
}

static int
hif_read(struct ath11k_base *base, void *bytes, uint32_t start, uint32_t end)
{
	if (bytes == NULL || start > end || end >= (1u << 25) || ((start | end) & 3) != 0) {
		return -EINVAL;
	}
	struct lx_qca_hif *hif = hif_for(base);
	for (uint32_t offset = start; offset <= end; offset += 4) {
		uint32_t value;
		int error = hif_register(hif, offset, &value, false);
		if (error != 0) {
			memset(bytes, 0, end - start + 4);
			return error;
		}
		memcpy((uint8_t *)bytes + offset - start, &value, sizeof(value));
	}
	return 0;
}

static int
hif_irqs(struct lx_qca_hif *hif, uint32_t vectors, bool enable)
{
	if (hif == NULL) {
		return -ENODEV;
	}
	mutex_lock(&hif->gate);
	int error = enable && (hif->closed || hif->fault != 0 || hif->driver == NULL) ? ESHUTDOWN :
	    hif->operations.irqs(hif->owner, vectors, enable);
	if (error == 0) {
		hif->enabled = enable ? hif->enabled | vectors : hif->enabled & ~vectors;
	}
	mutex_unlock(&hif->gate);
	if (error == 0 && !enable) {
		error = hif->operations.drain(hif->owner, vectors);
		mutex_lock(&hif->service);
		mutex_unlock(&hif->service);
	}
	return hif_fail(hif, error);
}

static void hif_ce_enable(struct ath11k_base *base)
{ struct lx_qca_hif *hif = hif_for(base); if (hif != NULL) { (void)hif_irqs(hif, hif->ce_mask, true); } }
static void hif_ce_disable(struct ath11k_base *base)
{ struct lx_qca_hif *hif = hif_for(base); if (hif != NULL) { (void)hif_irqs(hif, hif->ce_mask, false); } }
static void hif_dp_enable(struct ath11k_base *base)
{ struct lx_qca_hif *hif = hif_for(base); if (hif != NULL) { (void)hif_irqs(hif, hif->dp_mask, true); } }
static void hif_dp_disable(struct ath11k_base *base)
{ struct lx_qca_hif *hif = hif_for(base); if (hif != NULL) { (void)hif_irqs(hif, hif->dp_mask, false); } }

static int
hif_start(struct ath11k_base *base)
{
	struct lx_qca_hif *hif = hif_for(base);
	if (hif == NULL) { return -ENODEV; }
	mutex_lock(&hif->lifecycle);
	int error = lx_qca_hif_status(hif);
	if (error != 0) {
		mutex_unlock(&hif->lifecycle);
		return error;
	}
	set_bit(ATH11K_FLAG_DEVICE_INIT_DONE, &base->dev_flags);
	error = hif_irqs(hif, hif->ce_mask, true);
	if (error == 0) {
		ath11k_ce_rx_post_buf(base);
		struct hakios_dma_endpoint_status dma;
		error = hakios_dma_endpoint_status(hif->resources.dma, &dma);
		if (error == 0) {
			error = dma.fault != 0 ? dma.fault : dma.closed ? ESHUTDOWN : 0;
		}
		error = hif_fail(hif, error);
	}
	mutex_unlock(&hif->lifecycle);
	return error;
}

static void
hif_stop(struct ath11k_base *base)
{
	struct lx_qca_hif *hif = hif_for(base);
	if (hif == NULL) { return; }
	mutex_lock(&hif->lifecycle);
	if (hif_irqs(hif, hif->ce_mask, false) == 0) {
		ath11k_ce_cleanup_pipes(base);
	}
	mutex_unlock(&hif->lifecycle);
}

static int
hif_power(struct ath11k_base *base, enum lx_qca_power operation)
{
	struct lx_qca_hif *hif = hif_for(base);
	if (hif == NULL) {
		return -ENODEV;
	}
	mutex_lock(&hif->lifecycle);
	int error = operation == LX_QCA_POWER_DOWN ? 0 : lx_qca_hif_status(hif);
	if (error == 0) {
		error = hif_fail(hif, hif->operations.power(hif->owner, operation));
	}
	mutex_unlock(&hif->lifecycle);
	return error;
}
static int hif_power_up(struct ath11k_base *base) { return hif_power(base, LX_QCA_POWER_UP); }
static int hif_suspend(struct ath11k_base *base) { return hif_power(base, LX_QCA_POWER_SUSPEND); }
static int hif_resume(struct ath11k_base *base) { return hif_power(base, LX_QCA_POWER_RESUME); }
static void hif_power_down(struct ath11k_base *base, bool suspend)
{ (void)hif_power(base, suspend ? LX_QCA_POWER_SUSPEND : LX_QCA_POWER_DOWN); }

static int
hif_map_service(struct ath11k_base *base, uint16_t service, uint8_t *uplink, uint8_t *downlink)
{
	if (uplink == NULL || downlink == NULL) {
		return -EINVAL;
	}
	bool up = false, down = false;
	uint32_t up_pipe = 0, down_pipe = 0;
	for (uint32_t index = 0; index < base->hw_params.svc_to_ce_map_len; index++) {
		const struct service_to_pipe *entry = &base->hw_params.svc_to_ce_map[index];
		if (__le32_to_cpu(entry->service_id) != service) {
			continue;
		}
		uint32_t direction = __le32_to_cpu(entry->pipedir), pipe = __le32_to_cpu(entry->pipenum);
		if (pipe >= base->hw_params.ce_count) {
			return -EPROTO;
		}
		if (direction == PIPEDIR_OUT || direction == PIPEDIR_INOUT) {
			if (up) { return -EPROTO; }
			up = true;
			up_pipe = pipe;
		}
		if (direction == PIPEDIR_IN || direction == PIPEDIR_INOUT) {
			if (down) { return -EPROTO; }
			down = true;
			down_pipe = pipe;
		}
	}
	if (!up || !down) {
		return -ENOENT;
	}
	*uplink = (uint8_t)up_pipe;
	*downlink = (uint8_t)down_pipe;
	return 0;
}

static int
hif_msi_vector(struct ath11k_base *base, char *name, int *count, uint32_t *data, uint32_t *first)
{
	static const char *names[] = {"MHI", "CE", "WAKE", "DP"};
	static const uint32_t starts[] = {0, 3, 13, 14}, counts[] = {3, 10, 1, 18};
	struct lx_qca_hif *hif = hif_for(base);
	if (name == NULL || count == NULL || data == NULL || first == NULL || hif == NULL) {
		return -EINVAL;
	}
	*count = 0;
	*data = *first = 0;
	for (unsigned user = 0; user < 4; user++) {
		if (strcmp(name, names[user]) != 0) { continue; }
		struct hakios_msi_message message;
		int error = hakios_msi_message(hif->resources.msi, (enum hakios_msi_user)user, 0, &message);
		if (error != 0) { return hif_fail(hif, error); }
		*count = (int)counts[user];
		*data = message.data;
		*first = starts[user];
		return 0;
	}
	return -ENOENT;
}

static void
hif_msi_address(struct ath11k_base *base, uint32_t *low, uint32_t *high)
{
	struct lx_qca_hif *hif = hif_for(base);
	*low = *high = 0;
	if (hif == NULL) { return; }
	struct hakios_msi_message message;
	int error = hakios_msi_message(hif->resources.msi, HAKIOS_MSI_MHI, 0, &message);
	if (hif_fail(hif, error) == 0) {
		*low = (uint32_t)message.address;
		*high = (uint32_t)(message.address >> 32);
	}
}

static void
hif_ce_index(struct ath11k_base *base, uint32_t ce_id, uint32_t *index)
{
	struct lx_qca_hif *hif = hif_for(base);
	*index = UINT32_MAX;
	if (hif == NULL) { return; }
	for (uint32_t vector = 3; vector < 13; vector++) {
		if (hif->ce[vector] == (int)ce_id) {
			*index = vector - 3;
			return;
		}
	}
	(void)hif_fail(hif, EINVAL);
}

static const struct ath11k_hif_ops qca_hif_ops = {
	.read32 = hif_read32, .write32 = hif_write32, .read = hif_read,
	.irq_enable = hif_dp_enable, .irq_disable = hif_dp_disable, .start = hif_start, .stop = hif_stop,
	.power_up = hif_power_up, .power_down = hif_power_down, .suspend = hif_suspend, .resume = hif_resume,
	.map_service_to_pipe = hif_map_service, .get_user_msi_vector = hif_msi_vector, .get_msi_address = hif_msi_address,
	.ce_irq_enable = hif_ce_enable, .ce_irq_disable = hif_ce_disable, .get_ce_msi_idx = hif_ce_index,
};

const struct ath11k_hif_ops *lx_qca_hif_operations(void) { return &qca_hif_ops; }

int
lx_qca_hif_create(const struct lx_qca_hif_operations *operations, void *owner, struct lx_qca_hif **result)
{
	if (result == NULL) { return -EINVAL; }
	*result = NULL;
	if (operations == NULL || owner == NULL || operations->version != LX_QCA_HIF_VERSION ||
	    operations->power == NULL || operations->irqs == NULL || operations->drain == NULL || operations->fault == NULL) {
		return -EINVAL;
	}
	struct lx_qca_hif *hif = kzalloc(sizeof(*hif), GFP_KERNEL);
	if (hif == NULL) { return -ENOMEM; }
	hif->operations = *operations;
	hif->owner = owner;
	mutex_init(&hif->gate);
	mutex_init(&hif->service);
	mutex_init(&hif->lifecycle);
	for (unsigned vector = 0; vector < 32; vector++) { hif->ce[vector] = hif->group[vector] = -1; }
	*result = hif;
	return 0;
}

int
lx_qca_hif_bind(struct lx_qca_hif *hif, struct ath11k_base *base)
{
	if (hif == NULL || base == NULL || hif->driver != NULL || hif->closed || base->hif.ops != &qca_hif_ops ||
	    base->hw_rev != ATH11K_HW_QCA6390_HW20 || base->hw_params.ce_count > CE_COUNT_MAX ||
	    base->hw_params.ring_mask == NULL || ATH11K_EXT_IRQ_GRP_NUM_MAX > 18) {
		return -EINVAL;
	}
	struct lx_qca_pci_resources resources;
	int error = lx_qca_pci_resources(base, &resources);
	if (error != 0 || resources.hif != hif || resources.endpoint == 0 || resources.msi == NULL ||
	    resources.dma == NULL) {
		return error != 0 ? error : -ENODEV;
	}
	int engines[10];
	unsigned count = 0;
	for (unsigned index = 0; index < base->hw_params.ce_count; index++) {
		if (ath11k_ce_get_attr_flags(base, index) & CE_ATTR_DIS_INTR) { continue; }
		if (count == 10) { return -EOVERFLOW; }
		engines[count++] = (int)index;
	}
	hif->resources = resources;
	for (unsigned index = 0; index < count; index++) {
		unsigned vector = 3 + index;
		hif->ce[vector] = engines[index];
		hif->ce_mask |= 1u << vector;
	}
	for (unsigned group = 0; group < ATH11K_EXT_IRQ_GRP_NUM_MAX; group++) {
		const struct ath11k_hw_ring_mask *mask = base->hw_params.ring_mask;
		if (!(mask->tx[group] || mask->rx[group] || mask->rx_err[group] || mask->rx_wbm_rel[group] ||
		    mask->reo_status[group] || mask->rxdma2host[group] || mask->host2rxdma[group] || mask->rx_mon_status[group])) {
			continue;
		}
		base->ext_irq_grp[group].ab = base;
		base->ext_irq_grp[group].grp_id = group;
		hif->group[14 + group] = (int)group;
		hif->dp_mask |= 1u << (14 + group);
	}
	hif->driver = base;
	return 0;
}

int
lx_qca_hif_service(struct lx_qca_hif *hif, uint32_t vector, uint32_t budget, uint32_t *work, bool *more)
{
	if (work == NULL || more == NULL) { return -EINVAL; }
	*work = 0;
	*more = false;
	if (hif == NULL || vector >= 32 || budget == 0 || budget > 4096) { return -EINVAL; }
	mutex_lock(&hif->service);
	mutex_lock(&hif->gate);
	int error = hif->closed ? -ESHUTDOWN : hif->fault != 0 ? -hif->fault :
	    (hif->enabled & (1u << vector)) == 0 ? -EAGAIN : 0;
	mutex_unlock(&hif->gate);
	if (error == 0 && hif->ce[vector] >= 0) {
		ath11k_ce_per_engine_service(hif->driver, hif->ce[vector]);
	} else if (error == 0 && hif->group[vector] >= 0) {
		int done = ath11k_dp_service_srng(hif->driver, &hif->driver->ext_irq_grp[hif->group[vector]], (int)budget);
		if (done < 0) {
			error = hif_fail(hif, EIO);
		} else {
			*work = (uint32_t)done > budget ? budget : (uint32_t)done;
			*more = (uint32_t)done >= budget;
		}
	} else if (error == 0) {
		error = -ENOENT;
	}
	if (error == 0) {
		struct hakios_dma_endpoint_status dma;
		int check = hakios_dma_endpoint_status(hif->resources.dma, &dma);
		if (check == 0) { check = dma.fault != 0 ? dma.fault : dma.closed ? ESHUTDOWN : 0; }
		error = hif_fail(hif, check);
		if (error == 0) { error = lx_qca_hif_status(hif); }
	}
	mutex_unlock(&hif->service);
	return error;
}

int
lx_qca_hif_close(struct lx_qca_hif *hif)
{
	if (hif == NULL) { return -EINVAL; }
	mutex_lock(&hif->lifecycle);
	mutex_lock(&hif->gate);
	hif->closed = true;
	mutex_unlock(&hif->gate);
	int error = hif->drained ? 0 : hif_irqs(hif, hif->ce_mask | hif->dp_mask, false);
	if (error == 0) { hif->drained = true; }
	mutex_unlock(&hif->lifecycle);
	return error;
}

int
lx_qca_hif_destroy(struct lx_qca_hif **hif)
{
	if (hif == NULL || *hif == NULL) { return -EINVAL; }
	int error = lx_qca_hif_close(*hif);
	if (error != 0) { return error; }
	mutex_destroy(&(*hif)->service);
	mutex_destroy(&(*hif)->gate);
	mutex_destroy(&(*hif)->lifecycle);
	kfree(*hif);
	*hif = NULL;
	return 0;
}

#endif

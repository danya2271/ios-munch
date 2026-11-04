#if defined(CONFIG_ATH11K_PCI)

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include "core.h"
#include "ce.h"
#include "debug.h"
#include "fw.h"
#include "hal.h"
#include "hif.h"
#include "qmi.h"
#include <lx_qca_pci.h>
#include <lx_qca_hif.h>

struct lx_qca_pci {
	struct lx_qca_pci_operations operations;
	struct lx_qca_pci_resources resources;
	struct ath11k_base *driver;
	void *owner;
	bool hal_ready;
	bool hal_attempted;
	bool ce_ready;
	bool bound;
	bool irqs_attempted;
	bool core_ready;
	bool workers_cancelled;
	bool hif_closed;
	bool closed;
	bool fenced;
};

static struct ath11k_msi_user qca_msi_users[] = {
	{.name = "MHI", .num_vectors = 3, .base_vector = 0},
	{.name = "CE", .num_vectors = 10, .base_vector = 3},
	{.name = "WAKE", .num_vectors = 1, .base_vector = 13},
	{.name = "DP", .num_vectors = 18, .base_vector = 14},
};

static const struct ath11k_msi_config qca_msi_config = {
	.total_vectors = 32, .total_users = 4, .users = qca_msi_users, .hw_rev = ATH11K_HW_QCA6390_HW20,
};

static int
qca_native_error(int error)
{
	return error < 0 ? -EIO : -error;
}

void *
lx_qca_pci_owner(struct ath11k_base *driver)
{
	if (driver == NULL || driver->hif.bus != ATH11K_BUS_PCI) {
		return NULL;
	}
	struct lx_qca_pci *state = *(struct lx_qca_pci **)driver->drv_priv;
	return state != NULL && state->driver == driver ? state->owner : NULL;
}

static int
qca_validate(struct lx_qca_pci *state)
{
	struct hakios_pcie_endpoint_resources endpoint;
	struct hakios_mhi_worker_status worker;
	struct hakios_ipcr_status ipcr;
	struct hakios_msi_message first, message;
	struct hakios_dma_endpoint_status dma;
	struct hakios_msi_config routes;
	uint32_t identity, revision, subsystem;
	int error;
	if (state->resources.endpoint == 0 || state->resources.msi == NULL || state->resources.ipcr == NULL ||
	    state->resources.worker == NULL || state->resources.dma == NULL || state->resources.routes == NULL) {
		return -ENXIO;
	}
	if ((error = hakios_pcie_endpoint_resources(state->resources.endpoint, &endpoint)) != 0 ||
	    (error = hakios_pcie_config_read(state->resources.endpoint, 0, 4, &identity)) != 0 ||
	    (error = hakios_pcie_config_read(state->resources.endpoint, 8, 4, &revision)) != 0 ||
	    (error = hakios_pcie_config_read(state->resources.endpoint, 0x2c, 4, &subsystem)) != 0) {
		return qca_native_error(error);
	}
	if (endpoint.domain != 0 || endpoint.requester_id != 0x100 || endpoint.stream_id != 0x1c01 ||
	    endpoint.bar0_address != 0x60300000 || endpoint.bar0_size != 0x100000 ||
	    identity != 0x110117cbu || revision != 0x02800001u) {
		return -ENODEV;
	}
	struct hakios_pcie_bar_operation version = {.type = HAKIOS_PCIE_BAR_READ32, .offset = 0x224};
	error = hakios_pcie_bar0_transfer(state->resources.endpoint, &version, 1, false);
	if (error != 0) {
		return qca_native_error(error);
	}
	if (((version.value >> 8) & 15) != 2) {
		return -EOPNOTSUPP;
	}
	if ((error = hakios_mhi_worker_status(state->resources.worker, &worker)) != 0 ||
	    (error = hakios_ipcr_status(state->resources.ipcr, &ipcr)) != 0) {
		return qca_native_error(error);
	}
	if (worker.epoch != ipcr.epoch || ipcr.epoch == 0 || worker.fault != 0 || ipcr.fault != 0 || ipcr.stopped) {
		return -ENOTCONN;
	}
	if ((error = hakios_dma_endpoint_status(state->resources.dma, &dma)) != 0 ||
	    (error = hakios_msi_allocation_config(state->resources.routes, state->resources.endpoint, 0, &routes)) != 0) {
		return qca_native_error(error);
	}
	if (dma.endpoint != state->resources.endpoint || dma.fault != 0 || dma.closed) {
		return -ENOTCONN;
	}
	if (state->resources.arena != NULL) {
		struct hakios_dma_arena_status status;
		struct hakios_dma_arena *bound = hakios_dma_arena_get(state->driver->dev);
		bool matching = bound == state->resources.arena;
		if (bound != NULL) { hakios_dma_arena_put(bound); }
		if (!matching || hakios_dma_arena_status(state->resources.arena, &status) != 0 ||
		    status.endpoint != state->resources.dma || status.closed || status.fault != 0) { return -ENOTCONN; }
	}
	if ((error = hakios_msi_message(state->resources.msi, HAKIOS_MSI_MHI, 0, &first)) != 0) {
		return qca_native_error(error);
	}
	uint32_t vector = 0;
	for (uint32_t user = 0; user < 4; user++) {
		for (uint32_t index = 0; index < (uint32_t)qca_msi_users[user].num_vectors; index++, vector++) {
			error = hakios_msi_message(state->resources.msi, (enum hakios_msi_user)user, index, &message);
			if (error != 0) {
				return qca_native_error(error);
			}
			if (message.address != first.address || message.data != first.data + vector ||
			    message.address != routes.doorbell_iova || message.data != routes.data + vector ||
			    message.intid != routes.intids[vector]) {
				return -EPROTO;
			}
		}
	}
	struct ath11k_base *driver = state->driver;
	driver->id.vendor = identity & 0xffff;
	driver->id.device = identity >> 16;
	driver->id.subsystem_vendor = subsystem & 0xffff;
	driver->id.subsystem_device = subsystem >> 16;
	driver->pci.msi.config = &qca_msi_config;
	driver->pci.msi.addr_lo = (uint32_t)first.address;
	driver->pci.msi.addr_hi = (uint32_t)(first.address >> 32);
	driver->pci.msi.ep_base_data = first.data;
	set_bit(ATH11K_FLAG_MULTI_MSI_VECTORS, &driver->dev_flags);
	return 0;
}

static int
qca_fence(struct lx_qca_pci *state)
{
	int error = state->operations.fence(state->owner);
	if (error != 0) {
		return qca_native_error(error);
	}
	if (state->resources.worker != NULL) {
		struct hakios_mhi_worker_status status;
		error = hakios_mhi_worker_status(state->resources.worker, &status);
		if (error != 0 || !status.quiet || !status.irq_quiesced) {
			return error != 0 ? qca_native_error(error) : -EBUSY;
		}
	}
	if (state->resources.ipcr != NULL) {
		struct hakios_ipcr_status status;
		error = hakios_ipcr_status(state->resources.ipcr, &status);
		if (error != 0 || !status.stopped || !status.quiesced) {
			return error != 0 ? qca_native_error(error) : -EBUSY;
		}
	}
	if (state->resources.dma != NULL) {
		struct hakios_dma_endpoint_status status;
		error = hakios_dma_endpoint_status(state->resources.dma, &status);
		if (error != 0 || !status.closed || !status.fenced) {
			return error != 0 ? qca_native_error(error) : -EBUSY;
		}
	}
	state->fenced = true;
	return 0;
}

int
lx_qca_pci_resources(struct ath11k_base *driver, struct lx_qca_pci_resources *resources)
{
	if (resources == NULL) {
		return -EINVAL;
	}
	memset(resources, 0, sizeof(*resources));
	if (driver == NULL || driver->hif.bus != ATH11K_BUS_PCI) {
		return -EINVAL;
	}
	struct lx_qca_pci *state = *(struct lx_qca_pci **)driver->drv_priv;
	if (state == NULL || state->driver != driver) {
		return -ENODEV;
	}
	*resources = state->resources;
	return 0;
}

int
lx_qca_pci_remove(struct lx_qca_pci **driver)
{
	if (driver == NULL || *driver == NULL) {
		return -EINVAL;
	}
	struct lx_qca_pci *state = *driver;
	struct ath11k_base *base = state->driver;
	int error;
	if (base != NULL) {
		set_bit(ATH11K_FLAG_UNREGISTERING, &base->dev_flags);
	}
	if (!state->closed) {
		error = state->operations.close_admission(state->owner);
		if (error != 0) {
			return qca_native_error(error);
		}
		state->closed = true;
	}
	if (base != NULL) {
		if (!state->workers_cancelled) {
			if (state->core_ready) {
				cancel_work_sync(&base->qmi.event_work);
			}
			cancel_work_sync(&base->reset_work);
			cancel_work_sync(&base->dump_work);
			cancel_work_sync(&base->restart_work);
			cancel_work_sync(&base->update_11d_work);
			del_timer_sync(&base->rx_replenish_retry);
			state->workers_cancelled = true;
		}
		if (state->core_ready) {
			if (test_bit(ATH11K_FLAG_QMI_FAIL, &base->dev_flags)) {
				base->hif.ops->power_down(base, false);
				ath11k_debugfs_soc_destroy(base);
				ath11k_qmi_deinit_service(base);
				ath11k_core_pm_notifier_unregister(base);
			} else {
				ath11k_core_deinit(base);
			}
			state->core_ready = false;
		} else if (base->qmi.handle.sock != NULL) {
			qmi_handle_release(&base->qmi.handle);
		}
	}
	if (state->bound) {
		error = hakios_ipcr_qmi_unbind(state->resources.ipcr, &base->qmi.handle);
		if (error != 0) {
			return qca_native_error(error);
		}
		state->bound = false;
	}
	if (state->resources.hif != NULL && !state->hif_closed) {
		error = lx_qca_hif_close(state->resources.hif);
		if (error != 0) {
			return error;
		}
		state->hif_closed = true;
	}
	if (!state->fenced && (error = qca_fence(state)) != 0) {
		return error;
	}
	if (state->irqs_attempted) {
		error = state->operations.detach_irqs(state->owner);
		if (error != 0) {
			return qca_native_error(error);
		}
		state->irqs_attempted = false;
	}
	if (base != NULL) {
		if (state->ce_ready) {
			ath11k_ce_free_pipes(base);
			state->ce_ready = false;
		}
		if (state->hal_ready) {
			ath11k_hal_srng_deinit(base);
			state->hal_ready = false;
		} else if (state->hal_attempted) {
			kfree(base->hal.srng_config);
			base->hal.srng_config = NULL;
		}
		state->hal_attempted = false;
		ath11k_fw_destroy(base);
		ath11k_core_free(base);
		state->driver = NULL;
	}
	error = state->operations.release(state->owner);
	if (error != 0) {
		return qca_native_error(error);
	}
	kfree(state);
	*driver = NULL;
	return 0;
}

int
lx_qca_pci_probe(struct device *device, const struct ath11k_hif_ops *hif,
    const struct lx_qca_pci_operations *operations, void *owner, struct lx_qca_pci **result)
{
	if (result == NULL) {
		return -EINVAL;
	}
	*result = NULL;
	if (device == NULL || hif == NULL || owner == NULL || operations == NULL ||
	    operations->version != LX_QCA_PCI_VERSION || operations->prepare == NULL ||
	    operations->attach_irqs == NULL || operations->close_admission == NULL || operations->fence == NULL ||
	    operations->detach_irqs == NULL ||
	    operations->release == NULL || hif->read32 == NULL || hif->write32 == NULL || hif->read == NULL ||
	    hif->irq_enable == NULL || hif->irq_disable == NULL || hif->start == NULL || hif->stop == NULL ||
	    hif->power_up == NULL || hif->power_down == NULL || hif->suspend == NULL || hif->resume == NULL ||
	    hif->map_service_to_pipe == NULL || hif->get_user_msi_vector == NULL || hif->get_msi_address == NULL ||
	    hif->ce_irq_enable == NULL || hif->ce_irq_disable == NULL || hif->get_ce_msi_idx == NULL) {
		return -EINVAL;
	}
	struct lx_qca_pci *state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state == NULL) {
		return -ENOMEM;
	}
	*result = state;
	state->operations = *operations;
	state->owner = owner;
	int error = qca_native_error(operations->prepare(owner, device, &state->resources));
	if (error != 0) {
		goto fail;
	}
	struct ath11k_base *base = ath11k_core_alloc(device, sizeof(struct lx_qca_pci *), ATH11K_BUS_PCI);
	if (base == NULL) {
		error = -ENOMEM;
		goto fail;
	}
	state->driver = base;
	*(struct lx_qca_pci **)base->drv_priv = state;
	base->dev = device;
	base->hif.ops = hif;
	base->hw_rev = ATH11K_HW_QCA6390_HW20;
	base->fw_mode = ATH11K_FIRMWARE_MODE_NORMAL;
	if ((error = qca_validate(state)) != 0 || (error = ath11k_core_pre_init(base)) != 0) {
		goto fail;
	}
	state->hal_attempted = true;
	if ((error = ath11k_hal_srng_init(base)) != 0) {
		goto fail;
	}
	state->hal_ready = true;
	if ((error = ath11k_ce_alloc_pipes(base)) != 0) {
		goto fail;
	}
	state->ce_ready = true;
	base->qmi.ce_cfg.tgt_ce_len = base->hw_params.target_ce_count;
	base->qmi.ce_cfg.tgt_ce = base->hw_params.target_ce_config;
	base->qmi.ce_cfg.svc_to_ce_map_len = base->hw_params.svc_to_ce_map_len;
	base->qmi.ce_cfg.svc_to_ce_map = base->hw_params.svc_to_ce_map;
	base->qmi.service_ins_id = base->hw_params.qmi_service_ins_id;
	if (base->qmi.service_ins_id != 1) {
		error = -EPROTO;
		goto fail;
	}
	error = qca_native_error(hakios_ipcr_qmi_bind(state->resources.ipcr, &base->qmi.handle));
	if (error != 0) {
		goto fail;
	}
	state->bound = true;
	state->irqs_attempted = true;
	error = qca_native_error(operations->attach_irqs(owner, base));
	if (error != 0 || (error = ath11k_core_init(base)) != 0) {
		goto fail;
	}
	state->core_ready = true;
	(void)ath11k_qmi_fwreset_from_cold_boot(base);
	return 0;
fail:
	{
		int cleanup = lx_qca_pci_remove(result);
		return cleanup != 0 ? cleanup : error;
	}
}

#endif

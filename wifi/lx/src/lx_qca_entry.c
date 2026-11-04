#if defined(CONFIG_ATH11K_PCI)

#include <linux/device.h>
#include <linux/string.h>
#include <lx_qca_runtime.h>
#include <lx_os.h>

#include "core.h"
#include "lx.h"

struct qca_entry {
	struct device device;
	struct hakios_lx_qca_platform platform;
	struct hakios_dma_arena_config arena;
	void *platform_owner;
	struct lx_qca_runtime *runtime;
	bool prepared;
};

static struct qca_entry qca;

static void
qca_platform_clear(struct qca_entry *entry)
{
	entry->prepared = false;
	bzero(&entry->platform, sizeof(entry->platform));
	entry->platform_owner = NULL;
	bzero(&entry->arena, sizeof(entry->arena));
}

static int
qca_prepare(void *owner, struct device *device, struct lx_qca_pci_resources *resources)
{
	struct qca_entry *entry = owner;
	return entry->platform.prepare(entry->platform_owner, device, resources);
}

static int
qca_attach_irqs(void *owner, struct ath11k_base *driver)
{
	struct qca_entry *entry = owner;
	return entry->platform.attach_irqs(entry->platform_owner, driver);
}

static int
qca_close_admission(void *owner)
{
	struct qca_entry *entry = owner;
	return entry->platform.close_admission(entry->platform_owner);
}

static int
qca_fence(void *owner)
{
	struct qca_entry *entry = owner;
	return entry->platform.fence(entry->platform_owner);
}

static int
qca_detach_irqs(void *owner)
{
	struct qca_entry *entry = owner;
	return entry->platform.detach_irqs(entry->platform_owner);
}

static int
qca_release(void *owner)
{
	struct qca_entry *entry = owner;
	return entry->platform.release(entry->platform_owner);
}

static int
qca_power(void *owner, enum lx_qca_power operation)
{
	struct qca_entry *entry = owner;
	return entry->platform.power(entry->platform_owner, operation);
}

static int
qca_irqs(void *owner, uint32_t vectors, bool enable)
{
	struct qca_entry *entry = owner;
	return entry->platform.irqs(entry->platform_owner, vectors, enable);
}

static int
qca_drain(void *owner, uint32_t vectors)
{
	struct qca_entry *entry = owner;
	return entry->platform.drain(entry->platform_owner, vectors);
}

static void
qca_fault(void *owner, int error)
{
	struct qca_entry *entry = owner;
	entry->platform.fault(entry->platform_owner, error);
}

static int
qca_platform_load(struct qca_entry *entry)
{
	struct hakios_lx_qca_platform platform;
	struct hakios_dma_arena_config arena;
	void *owner = NULL;
	bzero(&platform, sizeof(platform));
	bzero(&arena, sizeof(arena));
	int error = lx_os_qca_platform(&platform, &owner, &arena);
	if (error != 0) {
		return error;
	}
	if (owner == NULL || platform.version != HAKIOS_LX_QCA_PLATFORM_VERSION ||
	    platform.prepare == NULL || platform.attach_irqs == NULL || platform.close_admission == NULL ||
	    platform.fence == NULL || platform.detach_irqs == NULL || platform.release == NULL ||
	    platform.power == NULL || platform.irqs == NULL || platform.drain == NULL || platform.fault == NULL ||
	    arena.dma_bytes == 0 || arena.cpu_bytes == 0) {
		return EPROTO;
	}
	entry->platform = platform;
	entry->platform_owner = owner;
	entry->arena = arena;
	entry->prepared = true;
	return 0;
}

int
lx_backend_is_pci(void)
{
	return 1;
}

int
lx_prepare(void)
{
	if (qca.prepared) {
		return 0;
	}
	return qca_platform_load(&qca);
}

static const struct lx_qca_runtime_operations qca_runtime_operations = {
	.version = LX_QCA_RUNTIME_VERSION,
	.platform = {
		.version = LX_QCA_PCI_VERSION,
		.prepare = qca_prepare,
		.attach_irqs = qca_attach_irqs,
		.close_admission = qca_close_admission,
		.fence = qca_fence,
		.detach_irqs = qca_detach_irqs,
		.release = qca_release,
	},
	.hif = {
		.version = LX_QCA_HIF_VERSION,
		.power = qca_power,
		.irqs = qca_irqs,
		.drain = qca_drain,
		.fault = qca_fault,
	},
};

int
lx_start(void)
{
	int error;

	if (qca.runtime != NULL) {
		return EBUSY;
	}
	if ((error = lx_prepare()) != 0) {
		lx_tell("lx: QCA6390 platform owner is unavailable: %d\n", error);
		return error;
	}
	bzero(&qca.device, sizeof(qca.device));
	qca.device.kobj.name = "munch-qca6390";
	ath11k_frame_mode = ATH11K_HW_TXRX_ETHERNET;
	error = lx_qca_runtime_start(&qca.device, &qca_runtime_operations, &qca.arena, &qca, &qca.runtime);
	if (error != 0) {
		if (qca.runtime == NULL) {
			qca_platform_clear(&qca);
		}
		lx_tell("lx: QCA6390 native PCI runtime start failed: %d\n", error);
		return error;
	}
	if (lx_os_option("wifi_station")) {
		error = lx_station_offer();
	} else if (lx_os_option("wifi_join")) {
		error = lx_station_join();
		if (error == 0 && lx_os_option("wifi_hold")) {
			lx_pause_ms(1000 * lx_os_option("wifi_hold"));
			lx_station_say();
		}
	} else {
		error = lx_os_option("wifi_scan") ? lx_station_scan() : 0;
	}
	if (error != 0) {
		lx_stop();
	}
	return error;
}

void
lx_stop(void)
{
	int error;

	if (qca.runtime == NULL) {
		return;
	}
	lx_station_withdraw();
	lx_station_leave();
	error = lx_qca_runtime_stop(&qca.runtime);
	if (error != 0) {
		lx_tell("lx: QCA6390 native PCI runtime stop retained ownership: %d\n", error);
		return;
	}
	qca_platform_clear(&qca);
	lx_tell("lx: QCA6390 native PCI runtime is down\n");
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return qca.runtime != NULL ? lx_station_send(frame, length) : ENETDOWN;
}

#endif

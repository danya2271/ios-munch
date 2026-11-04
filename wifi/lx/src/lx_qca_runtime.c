#if defined(CONFIG_ATH11K_PCI)

#include <linux/slab.h>
#include <lx_qca_runtime.h>

extern int lx_base_init(void);

struct lx_qca_runtime {
	struct lx_qca_runtime_operations operations;
	struct hakios_dma_arena_config config;
	struct lx_qca_pci_resources resources;
	struct lx_qca_pci *driver;
	struct device *device;
	void *owner;
	int fault;
	bool prepared;
};

static int runtime_power(void *owner, enum lx_qca_power operation)
{ struct lx_qca_runtime *runtime = owner; return runtime->operations.hif.power(runtime->owner, operation); }
static int runtime_irqs(void *owner, uint32_t vectors, bool enable)
{ struct lx_qca_runtime *runtime = owner; return runtime->operations.hif.irqs(runtime->owner, vectors, enable); }
static int runtime_drain(void *owner, uint32_t vectors)
{ struct lx_qca_runtime *runtime = owner; return runtime->operations.hif.drain(runtime->owner, vectors); }
static void runtime_fault(void *owner, int error)
{
	struct lx_qca_runtime *runtime = owner;
	int expected = 0;
	if (__atomic_compare_exchange_n(&runtime->fault, &expected, error > 0 ? error : EIO, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
		hakios_dma_arena_fault(runtime->resources.arena, error);
		runtime->operations.hif.fault(runtime->owner, error);
	}
}

static int runtime_prepare(void *owner, struct device *device, struct lx_qca_pci_resources *resources)
{
	struct lx_qca_runtime *runtime = owner;
	runtime->prepared = true;
	int error = runtime->operations.platform.prepare(runtime->owner, device, &runtime->resources);
	if (runtime->resources.hif != NULL || runtime->resources.arena != NULL) {
		runtime->resources.hif = NULL;
		runtime->resources.arena = NULL;
		error = EINVAL;
	}
	if (error == 0) { error = hakios_dma_arena_create(runtime->resources.dma, &runtime->config, &runtime->resources.arena); }
	if (error == 0) { error = hakios_dma_arena_bind(runtime->resources.arena, device); }
	const struct lx_qca_hif_operations operations = {.version = LX_QCA_HIF_VERSION,
	    .power = runtime_power, .irqs = runtime_irqs, .drain = runtime_drain, .fault = runtime_fault};
	if (error == 0) { error = -lx_qca_hif_create(&operations, runtime, &runtime->resources.hif); }
	*resources = runtime->resources;
	return error;
}
static int runtime_attach(void *owner, struct ath11k_base *base)
{
	struct lx_qca_runtime *runtime = owner;
	int error = lx_qca_hif_bind(runtime->resources.hif, base);
	return error != 0 ? -error : runtime->operations.platform.attach_irqs(runtime->owner, base);
}
static int runtime_close(void *owner)
{
	struct lx_qca_runtime *runtime = owner;
	int error = runtime->operations.platform.close_admission(runtime->owner);
	if (error == 0 && runtime->resources.arena != NULL) { error = hakios_dma_arena_close(runtime->resources.arena); }
	return error;
}
static int runtime_fence(void *owner)
{
	struct lx_qca_runtime *runtime = owner;
	int error = runtime->operations.platform.fence(runtime->owner);
	if (error == 0 && runtime->resources.dma != NULL) { error = hakios_dma_endpoint_close(runtime->resources.dma); }
	return error;
}
static int runtime_detach(void *owner)
{ struct lx_qca_runtime *runtime = owner; return runtime->operations.platform.detach_irqs(runtime->owner); }
static int runtime_release(void *owner)
{
	struct lx_qca_runtime *runtime = owner;
	int error = 0;
	if (runtime->resources.hif != NULL) { error = -lx_qca_hif_destroy(&runtime->resources.hif); }
	if (error == 0 && runtime->resources.arena != NULL) { error = hakios_dma_arena_destroy(&runtime->resources.arena); }
	if (error == 0 && runtime->prepared) {
		error = runtime->operations.platform.release(runtime->owner);
		if (error == 0) { runtime->prepared = false; }
	}
	return error;
}
static const struct lx_qca_pci_operations runtime_pci = {.version = LX_QCA_PCI_VERSION,
	.prepare = runtime_prepare, .attach_irqs = runtime_attach, .close_admission = runtime_close,
	.fence = runtime_fence, .detach_irqs = runtime_detach, .release = runtime_release};

int lx_qca_runtime_status(struct lx_qca_runtime *runtime)
{
	if (runtime == NULL || runtime->driver == NULL) { return -ENODEV; }
	int error = __atomic_load_n(&runtime->fault, __ATOMIC_ACQUIRE);
	if (error != 0) { return -error; }
	struct hakios_dma_arena_status status;
	error = hakios_dma_arena_status(runtime->resources.arena, &status);
	if (error == 0) { error = status.fault != 0 ? status.fault : status.closed ? ESHUTDOWN : 0; }
	return error != 0 ? -error : lx_qca_hif_status(runtime->resources.hif);
}
int lx_qca_runtime_stop(struct lx_qca_runtime **runtime)
{
	if (runtime == NULL || *runtime == NULL) { return -EINVAL; }
	struct lx_qca_runtime *state = *runtime;
	int error = state->driver != NULL ? lx_qca_pci_remove(&state->driver) : 0;
	if (error != 0) { return error; }
	kfree(state);
	*runtime = NULL;
	return 0;
}
int lx_qca_runtime_start(struct device *device, const struct lx_qca_runtime_operations *operations,
    const struct hakios_dma_arena_config *config, void *owner, struct lx_qca_runtime **result)
{
	if (result == NULL) { return -EINVAL; }
	*result = NULL;
	if (device == NULL || owner == NULL || config == NULL || operations == NULL ||
	    operations->version != LX_QCA_RUNTIME_VERSION || operations->platform.version != LX_QCA_PCI_VERSION ||
	    operations->hif.version != LX_QCA_HIF_VERSION || operations->platform.prepare == NULL ||
	    operations->platform.attach_irqs == NULL || operations->platform.close_admission == NULL ||
	    operations->platform.fence == NULL || operations->platform.detach_irqs == NULL || operations->platform.release == NULL ||
	    operations->hif.power == NULL || operations->hif.irqs == NULL || operations->hif.drain == NULL || operations->hif.fault == NULL) {
		return -EINVAL;
	}
	int error = lx_base_init();
	if (error != 0) { return error > 0 ? -error : -EIO; }
	struct lx_qca_runtime *runtime = kzalloc(sizeof(*runtime), GFP_KERNEL);
	if (runtime == NULL) { return -ENOMEM; }
	runtime->operations = *operations;
	runtime->config = *config;
	runtime->device = device;
	runtime->owner = owner;
	*result = runtime;
	error = lx_qca_pci_probe(device, lx_qca_hif_operations(), &runtime_pci, runtime, &runtime->driver);
	if (error == 0) { error = lx_qca_runtime_status(runtime); }
	if (error != 0) {
		int cleanup = lx_qca_runtime_stop(result);
		if (cleanup != 0) { return cleanup; }
	}
	return error;
}
int lx_qca_runtime_recover(struct lx_qca_runtime **runtime)
{
	if (runtime == NULL || *runtime == NULL) { return -EINVAL; }
	struct lx_qca_runtime_operations operations = (*runtime)->operations;
	struct hakios_dma_arena_config config = (*runtime)->config;
	struct device *device = (*runtime)->device;
	void *owner = (*runtime)->owner;
	int error = lx_qca_runtime_stop(runtime);
	return error != 0 ? error : lx_qca_runtime_start(device, &operations, &config, owner, runtime);
}

#endif

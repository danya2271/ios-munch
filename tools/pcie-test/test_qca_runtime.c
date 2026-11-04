#define PCIE_TEST_RUNTIME 1
#define main pci_fixture_main
#include "test_ath11k_pci.c"
#undef main
#include <linux/dma-mapping.h>
#include <linux/skbuff.h>
#include <lx_qca_runtime.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls, pcie_test_fail_allocation_at;
static struct lx_qca_runtime *runtime;
static struct {
	void *memory;
	unsigned holds, epochs, faults, drains;
	int base_error, close_error, irq_error, drain_error;
	bool allocation_error, foreign, empty_prepare;
} platform;

int lx_base_init(void) { return platform.base_error; }
int hakios_dma_endpoint_hold(struct hakios_dma_endpoint *endpoint)
{ assert(endpoint == &model.dma); platform.holds++; return 0; }
int hakios_dma_endpoint_drop(struct hakios_dma_endpoint *endpoint)
{ assert(endpoint == &model.dma && platform.holds != 0 && model.fenced); platform.holds--; return 0; }
int hakios_dma_endpoint_close(struct hakios_dma_endpoint *endpoint)
{
	assert(endpoint == &model.dma && model.fenced);
	int error = platform.close_error;
	platform.close_error = 0;
	return error;
}
int hakios_dma_endpoint_alloc_aligned(struct hakios_dma_endpoint *endpoint, size_t size, uint64_t alignment,
    hakios_dma_lease_t *lease, void **cpu, uint64_t *dma)
{
	assert(endpoint == &model.dma && platform.memory == NULL && size == alignment);
	platform.memory = aligned_alloc(alignment, size);
	assert(platform.memory != NULL);
	if (platform.allocation_error) { return EIO; }
	*cpu = platform.memory; *dma = 0x20000000; *lease = 1;
	return 0;
}
unsigned ath11k_ce_get_attr_flags(struct ath11k_base *base, unsigned engine)
{ assert(base == model.base && engine < CE_COUNT_MAX); return 0; }
void ath11k_ce_per_engine_service(struct ath11k_base *base, uint16_t engine)
{ (void)base; (void)engine; abort(); }
void ath11k_ce_rx_post_buf(struct ath11k_base *base) { (void)base; abort(); }
void ath11k_ce_cleanup_pipes(struct ath11k_base *base) { (void)base; abort(); }
int ath11k_dp_service_srng(struct ath11k_base *base, struct ath11k_ext_irq_grp *group, int budget)
{ (void)base; (void)group; (void)budget; abort(); }

static int runtime_prepare(void *owner, struct device *device, struct lx_qca_pci_resources *resources)
{
	assert(owner == &platform && runtime != NULL);
	if (model.released) {
		assert(platform.memory == NULL && platform.holds == 0);
		model.prepared = model.closed = model.fenced = model.released = model.hif_closed = false;
		model.core_deinit = model.special_deinit = model.cancelled = 0;
		model.worker_status = (struct hakios_mhi_worker_status) {.epoch = 19 + platform.epochs};
		model.ipcr_status = (struct hakios_ipcr_status) {.epoch = 19 + platform.epochs};
	}
	model.pci_owner = runtime;
	platform.epochs++;
	int error = prepare(&model, device, resources);
	resources->hif = NULL;
	if (platform.empty_prepare) { memset(resources, 0, sizeof(*resources)); return EIO; }
	if (platform.foreign) { resources->hif = &model.hif; resources->arena = (void *)&model; }
	return error;
}
static int runtime_attach(void *owner, struct ath11k_base *base)
{
	assert(owner == &platform);
	struct lx_qca_pci_resources resources;
	assert(lx_qca_pci_resources(base, &resources) == 0 && resources.arena != NULL && resources.hif != NULL);
	assert(lx_qca_hif_status(resources.hif) == 0);
	model.arena = resources.arena;
	return attach(&model, base);
}
static int runtime_close(void *owner) { assert(owner == &platform); return close_admission(&model); }
static int runtime_fence(void *owner)
{
	assert(owner == &platform);
	struct hakios_dma_arena_status status;
	if (model.arena != NULL) {
		assert(hakios_dma_arena_status(model.arena, &status) == 0 && status.closed);
	}
	model.hif_closed = true;
	return fence(&model);
}
static int runtime_detach(void *owner) { assert(owner == &platform); return detach(&model); }
static int runtime_release(void *owner)
{
	assert(owner == &platform && platform.holds == 0);
	struct hakios_dma_arena *arena = hakios_dma_arena_get(&model.device);
	assert(arena == NULL);
	int error = release(&model);
	if (error == 0) { free(platform.memory); platform.memory = NULL; model.arena = NULL; }
	return error;
}
static int runtime_power(void *owner, enum lx_qca_power operation)
{ assert(owner == &platform && operation <= LX_QCA_POWER_RESUME); return 0; }
static int runtime_irqs(void *owner, uint32_t vectors, bool enable)
{
	assert(owner == &platform && !enable && vectors == 0);
	int error = platform.irq_error;
	platform.irq_error = 0;
	return error;
}
static int runtime_drain(void *owner, uint32_t vectors)
{
	assert(owner == &platform && vectors == 0);
	platform.drains++;
	int error = platform.drain_error;
	platform.drain_error = 0;
	return error;
}
static void runtime_fault(void *owner, int error)
{ assert(owner == &platform && error > 0); platform.faults++; }
static const struct lx_qca_runtime_operations runtime_operations = {
	.version = LX_QCA_RUNTIME_VERSION,
	.platform = {.version = LX_QCA_PCI_VERSION, .prepare = runtime_prepare, .attach_irqs = runtime_attach,
	    .close_admission = runtime_close, .fence = runtime_fence, .detach_irqs = runtime_detach, .release = runtime_release},
	.hif = {.version = LX_QCA_HIF_VERSION, .power = runtime_power, .irqs = runtime_irqs,
	    .drain = runtime_drain, .fault = runtime_fault},
};
static const struct hakios_dma_arena_config arena_config = {.dma_bytes = 65536, .cpu_bytes = 65536};

static void reset(void)
{
	assert(runtime == NULL && platform.memory == NULL && platform.holds == 0);
	memset(&platform, 0, sizeof(platform));
	model_init();
}
static int start(void)
{ return lx_qca_runtime_start(&model.device, &runtime_operations, &arena_config, &platform, &runtime); }
static void check_runtime_clean(void)
{
	assert(runtime == NULL && platform.holds == 0 && platform.memory == NULL);
	check_clean(NULL);
}
static void test_runtime_success(void)
{
	reset();
	assert(start() == 0 && lx_qca_runtime_status(runtime) == 0);
	assert(lx_qca_runtime_recover(&runtime) == 0 && platform.epochs == 2);
	assert(lx_qca_runtime_status(runtime) == 0 && platform.holds == 1);
	assert(lx_qca_runtime_stop(&runtime) == 0);
	check_runtime_clean();
}
static void test_runtime_failures(void)
{
	for (unsigned site = FAIL_PREPARE; site <= FAIL_CORE; site++) {
		reset();
		model.fail_site = site;
		assert(start() < 0 && model.fail_site == FAIL_NONE);
		check_runtime_clean();
	}
	for (unsigned site = FAIL_CLOSE; site <= FAIL_RELEASE; site++) {
		reset();
		assert(start() == 0);
		model.fail_site = site;
		assert(lx_qca_runtime_stop(&runtime) == -EIO && runtime != NULL);
		assert(lx_qca_runtime_stop(&runtime) == 0);
		check_runtime_clean();
	}
	for (unsigned failure = 1; failure <= 7; failure++) {
		reset();
		pcie_test_fail_allocation_at = pcie_test_allocation_calls + failure;
		assert(start() == -ENOMEM);
		pcie_test_fail_allocation_at = 0;
		check_runtime_clean();
	}
	for (unsigned scenario = 0; scenario < 5; scenario++) {
		reset();
		platform.allocation_error = scenario == 0;
		platform.foreign = scenario == 1;
		platform.empty_prepare = scenario == 2;
		if (scenario >= 3) { model.fail_site = scenario == 3 ? FAIL_PREPARE : FAIL_CORE; model.refuse_fence = true; }
		assert(start() < 0);
		if (scenario >= 3) {
			assert(runtime != NULL);
			if (scenario == 3) {
				assert(model.frees == 0 && model.cancelled == 0);
			} else {
				assert(model.frees == 1 && model.cancelled == 30 && model.base != NULL && model.ce && model.hal);
			}
			model.refuse_fence = false;
			assert(lx_qca_runtime_stop(&runtime) == 0);
		}
		check_runtime_clean();
	}
	for (unsigned scenario = 0; scenario < 3; scenario++) {
		reset();
		assert(start() == 0);
		if (scenario == 0) { platform.close_error = EIO; }
		if (scenario == 1) { platform.irq_error = EIO; }
		if (scenario == 2) { platform.drain_error = EIO; }
		assert(lx_qca_runtime_stop(&runtime) == -EIO && runtime != NULL);
		assert(lx_qca_runtime_stop(&runtime) == 0);
		check_runtime_clean();
	}
}
static void test_runtime_retention(void)
{
	reset();
	assert(start() == 0);
	struct hakios_dma_arena *arena = hakios_dma_arena_get(&model.device);
	assert(arena != NULL && lx_qca_runtime_stop(&runtime) == -EBUSY && !model.fenced);
	hakios_dma_arena_put(arena);
	assert(lx_qca_runtime_stop(&runtime) == 0);
	check_runtime_clean();
	reset();
	assert(start() == 0);
	struct sk_buff *skb = linuxkpi_alloc_skb(256, GFP_ATOMIC);
	dma_addr_t dma;
	void *coherent = dma_alloc_coherent(&model.device, 8192, &dma, GFP_ATOMIC);
	assert(skb != NULL && coherent != NULL);
	assert(lx_qca_runtime_recover(&runtime) == -EBUSY && platform.epochs == 1 && model.fenced);
	assert(model.base == NULL && runtime != NULL && !model.released);
	linuxkpi_kfree_skb(skb);
	dma_free_coherent(&model.device, 8192, coherent, dma);
	assert(lx_qca_runtime_recover(&runtime) == 0 && platform.epochs == 2);
	struct lx_qca_pci_resources resources;
	assert(lx_qca_pci_resources(model.base, &resources) == 0);
	dma_sync_single_for_cpu(&model.device, 0, 1, DMA_TO_DEVICE);
	assert(lx_qca_runtime_status(runtime) < 0);
	assert(lx_qca_hif_status(resources.hif) < 0 && platform.faults == 1);
	assert(lx_qca_hif_status(resources.hif) < 0 && platform.faults == 1);
	assert(lx_qca_runtime_recover(&runtime) == 0 && platform.epochs == 3);
	assert(lx_qca_runtime_stop(&runtime) == 0);
	check_runtime_clean();
}
static void test_runtime_rejections(void)
{
	reset();
	struct lx_qca_runtime_operations missing = runtime_operations;
	missing.platform.fence = NULL;
	assert(lx_qca_runtime_start(&model.device, &missing, &arena_config, &platform, &runtime) == -EINVAL);
	assert(runtime == NULL && platform.epochs == 0 && model.allocations == 0);
	platform.base_error = EIO;
	assert(start() == -EIO && runtime == NULL && !model.prepared);
	platform.base_error = 0;
	model.fail_state_alloc = true;
	assert(start() == -ENOMEM && runtime == NULL && !model.prepared);
}
int main(void)
{
	test_runtime_success(); test_runtime_failures(); test_runtime_retention(); test_runtime_rejections();
	puts("qca-runtime-test: PASS (actual runtime/PCI/HIF/arena/DMA/SKB, mocked platform/driver, rollback and recovery epochs)");
	return 0;
}

#ifndef _LX_QCA_RUNTIME_H_
#define _LX_QCA_RUNTIME_H_

#include <lx_qca_hif.h>

#define LX_QCA_RUNTIME_VERSION 1u
struct lx_qca_runtime;
struct lx_qca_runtime_operations {
	uint32_t version;
	struct lx_qca_pci_operations platform;
	struct lx_qca_hif_operations hif;
};

int lx_qca_runtime_start(struct device *device, const struct lx_qca_runtime_operations *operations,
    const struct hakios_dma_arena_config *config, void *owner, struct lx_qca_runtime **result);
int lx_qca_runtime_status(struct lx_qca_runtime *runtime);
int lx_qca_runtime_stop(struct lx_qca_runtime **runtime);
int lx_qca_runtime_recover(struct lx_qca_runtime **runtime);

#endif

#ifndef _LX_QCA_PCI_H_
#define _LX_QCA_PCI_H_

#include <sys/hakios.pcie.h>
#include <sys/hakios.msi.h>
#include <sys/hakios.mhi.worker.h>
#include <sys/hakios.dma.endpoint.h>
#include <sys/hakios.msi.pool.h>
#include <sys/hakios.dma.arena.h>

#define LX_QCA_PCI_VERSION 2u

struct device;
struct ath11k_base;
struct ath11k_hif_ops;
struct lx_qca_pci;
struct lx_qca_hif;

struct lx_qca_pci_resources {
	hakios_pcie_lease_t endpoint;
	struct hakios_msi *msi;
	struct hakios_ipcr *ipcr;
	struct hakios_mhi_worker *worker;
	struct hakios_dma_endpoint *dma;
	struct hakios_msi_allocation *routes;
	struct lx_qca_hif *hif;
	struct hakios_dma_arena *arena;
};

struct lx_qca_pci_operations {
	uint32_t version;
	int (*prepare)(void *owner, struct device *device, struct lx_qca_pci_resources *resources);
	int (*attach_irqs)(void *owner, struct ath11k_base *driver);
	int (*close_admission)(void *owner);
	int (*fence)(void *owner);
	int (*detach_irqs)(void *owner);
	int (*release)(void *owner);
};

int lx_qca_pci_probe(struct device *device, const struct ath11k_hif_ops *hif,
    const struct lx_qca_pci_operations *operations, void *owner, struct lx_qca_pci **result);
int lx_qca_pci_remove(struct lx_qca_pci **driver);
void *lx_qca_pci_owner(struct ath11k_base *driver);
int lx_qca_pci_resources(struct ath11k_base *driver, struct lx_qca_pci_resources *resources);

#endif

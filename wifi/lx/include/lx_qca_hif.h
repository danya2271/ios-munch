#ifndef _LX_QCA_HIF_H_
#define _LX_QCA_HIF_H_

#include <lx_qca_pci.h>

#define LX_QCA_HIF_VERSION 1u

enum lx_qca_power { LX_QCA_POWER_UP, LX_QCA_POWER_DOWN, LX_QCA_POWER_SUSPEND, LX_QCA_POWER_RESUME };

struct lx_qca_hif_operations {
	uint32_t version;
	int (*power)(void *owner, enum lx_qca_power operation);
	int (*irqs)(void *owner, uint32_t vectors, bool enable);
	int (*drain)(void *owner, uint32_t vectors);
	void (*fault)(void *owner, int error);
};

int lx_qca_hif_create(const struct lx_qca_hif_operations *operations, void *owner, struct lx_qca_hif **result);
const struct ath11k_hif_ops *lx_qca_hif_operations(void);
int lx_qca_hif_bind(struct lx_qca_hif *hif, struct ath11k_base *driver);
int lx_qca_hif_service(struct lx_qca_hif *hif, uint32_t vector, uint32_t budget, uint32_t *work, bool *more);
int lx_qca_hif_close(struct lx_qca_hif *hif);
int lx_qca_hif_destroy(struct lx_qca_hif **hif);
int lx_qca_hif_status(struct lx_qca_hif *hif);

#endif

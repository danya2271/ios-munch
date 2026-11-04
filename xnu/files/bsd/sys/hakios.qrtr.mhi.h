#ifndef _SYS_HAKIOS_QRTR_MHI_H_
#define _SYS_HAKIOS_QRTR_MHI_H_

#include <sys/hakios.qrtr.h>

#define HAKIOS_QRTR_MHI_VERSION 2u

struct hakios_qrtr_mhi_config {
	uint32_t local_node;
	uint32_t local_port;
};

struct hakios_qrtr_mhi_operations {
	uint32_t version;
	int (*transmit)(void *owner, const void *frame, size_t size);
	int (*quiesce)(void *owner);
	void (*schedule)(void *owner);
};

struct hakios_qrtr_mhi;

int hakios_qrtr_mhi_create(const struct hakios_qrtr_mhi_config *config,
    const struct hakios_qrtr_mhi_operations *operations, void *owner, struct hakios_qrtr_mhi **result);
int hakios_qrtr_mhi_endpoint(struct hakios_qrtr_mhi *router, struct hakios_qrtr_endpoint *endpoint);
int hakios_qrtr_mhi_start(struct hakios_qrtr_mhi *router);
int hakios_qrtr_mhi_receive(struct hakios_qrtr_mhi *router, const void *frame, size_t size);
int hakios_qrtr_mhi_poll(struct hakios_qrtr_mhi *router);
int hakios_qrtr_mhi_disconnect(struct hakios_qrtr_mhi *router);
int hakios_qrtr_mhi_status(struct hakios_qrtr_mhi *router);
int hakios_qrtr_mhi_idle(struct hakios_qrtr_mhi *router);
int hakios_qrtr_mhi_destroy(struct hakios_qrtr_mhi **router);

#endif

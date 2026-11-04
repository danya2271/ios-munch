#ifndef _SYS_HAKIOS_IPCR_H_
#define _SYS_HAKIOS_IPCR_H_

#include <sys/hakios.mhi.h>
#include <sys/hakios.qmi.h>
#include <sys/hakios.qrtr.mhi.h>

#define HAKIOS_IPCR_VERSION 1u
#define HAKIOS_IPCR_BUFFERS (HAKIOS_MHI_ELEMENTS - 1)

struct hakios_ipcr_buffer {
	uint8_t *bytes;
	uint64_t iova;
};

struct hakios_ipcr_config {
	uint64_t epoch;
	struct hakios_mhi_ring_config tx_ring;
	struct hakios_mhi_ring_config rx_ring;
	const struct hakios_ipcr_buffer *tx_buffers;
	const struct hakios_ipcr_buffer *rx_buffers;
	uint32_t tx_count;
	uint32_t rx_count;
	struct hakios_qrtr_mhi_config router;
};

struct hakios_ipcr_operations {
	uint32_t version;
	int (*sync_buffer)(void *owner, uint8_t channel, uint32_t index, bool for_device, uint32_t length);
	int (*sync_ring)(void *owner, uint8_t channel, uint32_t offset, uint32_t length);
	int (*publish)(void *owner, uint8_t channel, uint64_t next_wp);
	void (*schedule)(void *owner);
	int (*quiesce)(void *owner);
};

struct hakios_ipcr_status {
	uint64_t epoch;
	uint32_t tx_pending;
	uint32_t rx_ready;
	int fault;
	bool started;
	bool paused;
	bool stopped;
	bool quiesced;
};

struct hakios_ipcr;

int hakios_ipcr_create(const struct hakios_ipcr_config *config,
    const struct hakios_ipcr_operations *operations, void *owner, struct hakios_ipcr **result);
int hakios_ipcr_endpoint(struct hakios_ipcr *bridge, struct hakios_qrtr_endpoint *endpoint);
int hakios_ipcr_start(struct hakios_ipcr *bridge);
int hakios_ipcr_complete(struct hakios_ipcr *bridge, uint64_t epoch, const uint8_t event[HAKIOS_MHI_TRE_SIZE]);
int hakios_ipcr_work(struct hakios_ipcr *bridge, uint32_t budget, uint32_t *received);
int hakios_ipcr_pause(struct hakios_ipcr *bridge);
int hakios_ipcr_resume(struct hakios_ipcr *bridge);
int hakios_ipcr_stop(struct hakios_ipcr *bridge);
int hakios_ipcr_status(struct hakios_ipcr *bridge, struct hakios_ipcr_status *status);
int hakios_ipcr_qmi_bind(struct hakios_ipcr *bridge, struct qmi_handle *qmi);
int hakios_ipcr_qmi_unbind(struct hakios_ipcr *bridge, struct qmi_handle *qmi);
int hakios_ipcr_qmi_handle_init(struct qmi_handle *qmi, size_t largest, const struct qmi_ops *operations,
    const struct qmi_msg_handler *handlers);
int hakios_ipcr_destroy(struct hakios_ipcr **bridge);

#endif

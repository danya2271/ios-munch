#ifndef _SYS_HAKIOS_MHI_WORKER_H_
#define _SYS_HAKIOS_MHI_WORKER_H_

#include <sys/hakios.ipcr.h>

#define HAKIOS_MHI_WORKER_VERSION 1u
#define HAKIOS_MHI_WORK_STATE 1u
#define HAKIOS_MHI_WORK_CONTROL 2u
#define HAKIOS_MHI_WORK_DATA 4u
#define HAKIOS_MHI_WORK_IPCR 8u
#define HAKIOS_MHI_WORK_ALL 15u

struct hakios_mhi_irq_grant {
	void *token;
	uint32_t intid;
};

struct hakios_mhi_worker_config {
	uint64_t epoch;
	struct hakios_mhi_irq_grant vectors[3];
	struct hakios_mhi_event_ring *control;
	struct hakios_mhi_event_ring *data;
	struct hakios_ipcr *ipcr;
	uint32_t event_budget;
	uint32_t ipcr_budget;
	uint32_t retry_ms;
};

struct hakios_mhi_worker_operations {
	uint32_t version;
	int (*attach)(void *owner, const struct hakios_mhi_irq_grant *grant,
	    void (*handler)(void *context, uint32_t intid), void *context);
	int (*mask)(void *owner, const struct hakios_mhi_irq_grant *grant);
	int (*unmask)(void *owner, const struct hakios_mhi_irq_grant *grant);
	int (*synchronize)(void *owner, const struct hakios_mhi_irq_grant *grant);
	int (*detach)(void *owner, const struct hakios_mhi_irq_grant *grant);
	int (*control)(void *owner, bool *retry);
	void (*fault)(void *owner, int error);
};

struct hakios_mhi_worker_status {
	uint64_t epoch;
	uint64_t runs;
	uint32_t pending;
	uint32_t attached;
	int fault;
	bool started;
	bool quiet;
	bool irq_quiesced;
};

struct hakios_mhi_worker;

int hakios_mhi_worker_create(const struct hakios_mhi_worker_config *config,
    const struct hakios_mhi_worker_operations *operations, void *owner, struct hakios_mhi_worker **result);
int hakios_mhi_worker_start(struct hakios_mhi_worker *worker);
int hakios_mhi_worker_kick(struct hakios_mhi_worker *worker, uint64_t epoch, uint32_t sources);
int hakios_mhi_worker_quiesce(struct hakios_mhi_worker *worker);
int hakios_mhi_worker_resume(struct hakios_mhi_worker *worker);
int hakios_mhi_worker_status(struct hakios_mhi_worker *worker, struct hakios_mhi_worker_status *status);
int hakios_mhi_worker_destroy(struct hakios_mhi_worker **worker);

#endif

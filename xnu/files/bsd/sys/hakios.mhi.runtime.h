#ifndef _SYS_HAKIOS_MHI_RUNTIME_H_
#define _SYS_HAKIOS_MHI_RUNTIME_H_

#include <stdbool.h>
#include <sys/hakios.mhi.boot.h>

#define HAKIOS_MHI_RUNTIME_VERSION 1u

enum hakios_mhi_runtime_phase {
	HAKIOS_MHI_RUNTIME_NEW,
	HAKIOS_MHI_RUNTIME_ACTIVE,
	HAKIOS_MHI_RUNTIME_SUSPENDING,
	HAKIOS_MHI_RUNTIME_SUSPENDED,
	HAKIOS_MHI_RUNTIME_RESUMING,
	HAKIOS_MHI_RUNTIME_FAULT,
	HAKIOS_MHI_RUNTIME_RETIRED,
};

struct hakios_mhi_runtime_operations {
	uint32_t version;
	int (*read_state)(void *owner, uint32_t *state, uint32_t *environment);
	int (*request_state)(void *owner, uint32_t state);
	int (*wake)(void *owner, bool asserted);
	int (*drain)(void *owner);
	int (*rearm)(void *owner);
	int (*link_suspend)(void *owner);
	int (*link_resume)(void *owner);
	void (*fault)(void *owner, int error);
	int (*quiesce)(void *owner);
};

struct hakios_mhi_runtime_status {
	enum hakios_mhi_runtime_phase phase;
	uint32_t references;
	int fault;
};

struct hakios_mhi_runtime;

int hakios_mhi_runtime_create(struct hakios_mhi_boot *boot, uint32_t timeout_ms,
    const struct hakios_mhi_runtime_operations *operations, void *owner, struct hakios_mhi_runtime **result);
int hakios_mhi_runtime_start(struct hakios_mhi_runtime *runtime, uint64_t now_ms);
int hakios_mhi_runtime_get(struct hakios_mhi_runtime *runtime);
int hakios_mhi_runtime_put(struct hakios_mhi_runtime *runtime);
int hakios_mhi_runtime_suspend(struct hakios_mhi_runtime *runtime, uint64_t now_ms);
int hakios_mhi_runtime_resume(struct hakios_mhi_runtime *runtime, uint64_t now_ms);
int hakios_mhi_runtime_step(struct hakios_mhi_runtime *runtime, uint64_t now_ms);
int hakios_mhi_runtime_retire(struct hakios_mhi_runtime *runtime);
int hakios_mhi_runtime_status(struct hakios_mhi_runtime *runtime, struct hakios_mhi_runtime_status *status);
int hakios_mhi_runtime_destroy(struct hakios_mhi_runtime **runtime);

#endif

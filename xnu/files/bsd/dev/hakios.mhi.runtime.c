#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.mhi.runtime.h>

struct hakios_mhi_runtime {
	lck_mtx_t lock;
	struct hakios_mhi_boot *boot;
	struct hakios_mhi_runtime_operations operations;
	struct hakios_mhi_runtime_status status;
	void *owner;
	uint64_t last_time;
	uint64_t deadline;
	uint32_t timeout_ms;
	bool busy;
};

static LCK_GRP_DECLARE(mhi_runtime_group, "hakios-mhi-runtime");

static int
runtime_enter(struct hakios_mhi_runtime *runtime)
{
	if (runtime == NULL) {
		return EINVAL;
	}
	lck_mtx_lock(&runtime->lock);
	int error = runtime->busy ? EBUSY : 0;
	if (error == 0) {
		runtime->busy = true;
	}
	lck_mtx_unlock(&runtime->lock);
	return error;
}

static int
runtime_leave(struct hakios_mhi_runtime *runtime, int error)
{
	lck_mtx_lock(&runtime->lock);
	runtime->busy = false;
	lck_mtx_unlock(&runtime->lock);
	return error;
}

static int
runtime_fault(struct hakios_mhi_runtime *runtime, int error)
{
	if (error <= 0 || error == EAGAIN || error == ENOSPC || error == EBUSY) {
		error = EIO;
	}
	runtime->status.phase = HAKIOS_MHI_RUNTIME_FAULT;
	if (runtime->status.fault == 0) {
		runtime->status.fault = error;
		runtime->operations.fault(runtime->owner, error);
	}
	return error;
}

static int
runtime_time(struct hakios_mhi_runtime *runtime, uint64_t now_ms)
{
	if (now_ms < runtime->last_time || now_ms > UINT64_MAX - runtime->timeout_ms) {
		return runtime_fault(runtime, EINVAL);
	}
	runtime->last_time = now_ms;
	return 0;
}

static int
runtime_read(struct hakios_mhi_runtime *runtime, uint32_t *state)
{
	uint32_t environment = UINT32_MAX;
	*state = UINT32_MAX;
	int error = runtime->operations.read_state(runtime->owner, state, &environment);
	if (error == 0 && (environment != 2 || *state == 255)) {
		error = EIO;
	}
	return error == 0 ? 0 : runtime_fault(runtime, error);
}

int
hakios_mhi_runtime_create(struct hakios_mhi_boot *boot, uint32_t timeout_ms,
    const struct hakios_mhi_runtime_operations *operations, void *owner, struct hakios_mhi_runtime **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (boot == NULL || timeout_ms == 0 || operations == NULL || owner == NULL ||
	    operations->version != HAKIOS_MHI_RUNTIME_VERSION || operations->read_state == NULL ||
	    operations->request_state == NULL || operations->wake == NULL || operations->drain == NULL ||
	    operations->rearm == NULL || operations->link_suspend == NULL || operations->link_resume == NULL ||
	    operations->fault == NULL || operations->quiesce == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_runtime *runtime = kalloc_type(struct hakios_mhi_runtime, Z_WAITOK | Z_ZERO);
	if (runtime == NULL) {
		return ENOMEM;
	}
	int error = hakios_mhi_boot_take_runtime(boot);
	if (error != 0) {
		kfree_type(struct hakios_mhi_runtime, runtime);
		return error;
	}
	runtime->boot = boot;
	runtime->owner = owner;
	runtime->operations = *operations;
	runtime->timeout_ms = timeout_ms;
	lck_mtx_init(&runtime->lock, &mhi_runtime_group, LCK_ATTR_NULL);
	*result = runtime;
	return 0;
}

int
hakios_mhi_runtime_start(struct hakios_mhi_runtime *runtime, uint64_t now_ms)
{
	int error = runtime_enter(runtime);
	if (error != 0) {
		return error;
	}
	if (runtime->status.phase != HAKIOS_MHI_RUNTIME_NEW) {
		return runtime_leave(runtime, EALREADY);
	}
	uint32_t state;
	if ((error = runtime_time(runtime, now_ms)) == 0 && (error = runtime_read(runtime, &state)) == 0) {
		if (state != 2) {
			error = runtime_fault(runtime, ENOTSUP);
		} else if ((error = runtime->operations.wake(runtime->owner, true)) == 0 &&
		    (error = runtime->operations.rearm(runtime->owner)) == 0) {
			runtime->status.phase = HAKIOS_MHI_RUNTIME_ACTIVE;
		} else {
			error = runtime_fault(runtime, error);
		}
	}
	return runtime_leave(runtime, error);
}

int
hakios_mhi_runtime_get(struct hakios_mhi_runtime *runtime)
{
	int error = runtime_enter(runtime);
	if (error != 0) {
		return error;
	}
	if (runtime->status.phase == HAKIOS_MHI_RUNTIME_FAULT || runtime->status.phase == HAKIOS_MHI_RUNTIME_RETIRED) {
		error = ENOTCONN;
	} else if (runtime->status.phase != HAKIOS_MHI_RUNTIME_ACTIVE) {
		error = EAGAIN;
	} else if (runtime->status.references == UINT32_MAX) {
		error = EOVERFLOW;
	} else {
		runtime->status.references++;
	}
	return runtime_leave(runtime, error);
}

int
hakios_mhi_runtime_put(struct hakios_mhi_runtime *runtime)
{
	int error = runtime_enter(runtime);
	if (error != 0) {
		return error;
	}
	if (runtime->status.references == 0) {
		error = EINVAL;
	} else {
		runtime->status.references--;
	}
	return runtime_leave(runtime, error);
}

int
hakios_mhi_runtime_suspend(struct hakios_mhi_runtime *runtime, uint64_t now_ms)
{
	int error = runtime_enter(runtime);
	if (error != 0) {
		return error;
	}
	if (runtime->status.phase != HAKIOS_MHI_RUNTIME_ACTIVE || runtime->status.references != 0) {
		return runtime_leave(runtime, EBUSY);
	}
	if ((error = runtime_time(runtime, now_ms)) != 0) {
		return runtime_leave(runtime, error);
	}
	error = runtime->operations.drain(runtime->owner);
	if (error == EBUSY || error == EAGAIN) {
		return runtime_leave(runtime, error);
	}
	uint32_t state;
	if (error == 0 && (error = runtime_read(runtime, &state)) == 0) {
		error = state == 2 ? 0 : ENOTSUP;
	}
	if (error == 0 && (error = runtime->operations.request_state(runtime->owner, 5)) == 0) {
		error = runtime->operations.wake(runtime->owner, false);
	}
	if (error == 0) {
		runtime->status.phase = HAKIOS_MHI_RUNTIME_SUSPENDING;
		runtime->deadline = now_ms + runtime->timeout_ms;
		error = EAGAIN;
	} else {
		error = runtime_fault(runtime, error);
	}
	return runtime_leave(runtime, error);
}

int
hakios_mhi_runtime_resume(struct hakios_mhi_runtime *runtime, uint64_t now_ms)
{
	int error = runtime_enter(runtime);
	if (error != 0) {
		return error;
	}
	if (runtime->status.phase != HAKIOS_MHI_RUNTIME_SUSPENDED) {
		return runtime_leave(runtime, EBUSY);
	}
	if ((error = runtime_time(runtime, now_ms)) == 0 &&
	    (error = runtime->operations.link_resume(runtime->owner)) == 0 &&
	    (error = runtime->operations.wake(runtime->owner, true)) == 0) {
		error = runtime->operations.request_state(runtime->owner, 2);
	}
	if (error == 0) {
		runtime->status.phase = HAKIOS_MHI_RUNTIME_RESUMING;
		runtime->deadline = now_ms + runtime->timeout_ms;
		error = EAGAIN;
	} else {
		error = runtime_fault(runtime, error);
	}
	return runtime_leave(runtime, error);
}

int
hakios_mhi_runtime_step(struct hakios_mhi_runtime *runtime, uint64_t now_ms)
{
	int error = runtime_enter(runtime);
	if (error != 0) {
		return error;
	}
	enum hakios_mhi_runtime_phase phase = runtime->status.phase;
	if (phase == HAKIOS_MHI_RUNTIME_NEW || phase == HAKIOS_MHI_RUNTIME_FAULT || phase == HAKIOS_MHI_RUNTIME_RETIRED) {
		return runtime_leave(runtime, phase == HAKIOS_MHI_RUNTIME_NEW ? EAGAIN : ENOTCONN);
	}
	error = runtime_time(runtime, now_ms);
	if (error != 0 || phase == HAKIOS_MHI_RUNTIME_SUSPENDED) {
		return runtime_leave(runtime, error);
	}
	if (phase != HAKIOS_MHI_RUNTIME_ACTIVE && now_ms >= runtime->deadline) {
		return runtime_leave(runtime, runtime_fault(runtime, ETIMEDOUT));
	}
	uint32_t state;
	error = runtime_read(runtime, &state);
	if (error != 0) {
		return runtime_leave(runtime, error);
	}
	if (phase == HAKIOS_MHI_RUNTIME_ACTIVE) {
		error = state == 2 ? 0 : runtime_fault(runtime, ENOTSUP);
	} else if (phase == HAKIOS_MHI_RUNTIME_SUSPENDING && state == 5) {
		error = runtime->operations.link_suspend(runtime->owner);
		if (error == 0) {
			runtime->status.phase = HAKIOS_MHI_RUNTIME_SUSPENDED;
		}
	} else if (phase == HAKIOS_MHI_RUNTIME_RESUMING && state == 2) {
		error = runtime->operations.rearm(runtime->owner);
		if (error == 0) {
			runtime->status.phase = HAKIOS_MHI_RUNTIME_ACTIVE;
		}
	} else if ((phase == HAKIOS_MHI_RUNTIME_SUSPENDING && state == 2) ||
	    (phase == HAKIOS_MHI_RUNTIME_RESUMING && state == 5)) {
		return runtime_leave(runtime, EAGAIN);
	} else {
		error = ENOTSUP;
	}
	return runtime_leave(runtime, error == 0 ? 0 : runtime_fault(runtime, error));
}

int
hakios_mhi_runtime_retire(struct hakios_mhi_runtime *runtime)
{
	int error = runtime_enter(runtime);
	if (error != 0) {
		return error;
	}
	if (runtime->status.phase != HAKIOS_MHI_RUNTIME_RETIRED) {
		error = runtime->operations.quiesce(runtime->owner);
		if (error == 0) {
			runtime->status.phase = HAKIOS_MHI_RUNTIME_RETIRED;
		} else {
			error = runtime_fault(runtime, error);
		}
	}
	return runtime_leave(runtime, error);
}

int
hakios_mhi_runtime_status(struct hakios_mhi_runtime *runtime, struct hakios_mhi_runtime_status *status)
{
	if (status == NULL) {
		return EINVAL;
	}
	int error = runtime_enter(runtime);
	if (error == 0) {
		*status = runtime->status;
		runtime_leave(runtime, 0);
	}
	return error;
}

int
hakios_mhi_runtime_destroy(struct hakios_mhi_runtime **runtime)
{
	if (runtime == NULL || *runtime == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_runtime *state = *runtime;
	int error = runtime_enter(state);
	if (error != 0) {
		return error;
	}
	if (state->status.phase != HAKIOS_MHI_RUNTIME_RETIRED || state->status.references != 0) {
		return runtime_leave(state, EBUSY);
	}
	error = hakios_mhi_boot_retire_runtime(state->boot);
	if (error != 0) {
		return runtime_leave(state, error);
	}
	lck_mtx_destroy(&state->lock, &mhi_runtime_group);
	kfree_type(struct hakios_mhi_runtime, state);
	*runtime = NULL;
	return 0;
}

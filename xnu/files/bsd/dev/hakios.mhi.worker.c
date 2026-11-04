#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/hakios.mhi.worker.h>

struct mhi_irq_context {
	struct hakios_mhi_worker *worker;
	uint32_t index;
};

struct hakios_mhi_worker {
	lck_mtx_t lifecycle;
	lck_spin_t gate;
	struct hakios_mhi_worker_config config;
	struct hakios_mhi_worker_operations operations;
	struct hakios_mhi_worker_status status;
	struct mhi_irq_context irqs[3];
	thread_call_t call;
	thread_t callback_thread;
	void *owner;
	bool accepting;
	bool notified;
};

static LCK_GRP_DECLARE(mhi_worker_group, "hakios-mhi-worker");

static boolean_t
worker_lock(struct hakios_mhi_worker *worker)
{
	boolean_t interrupts = ml_set_interrupts_enabled(false);
	lck_spin_lock(&worker->gate);
	return interrupts;
}

static void
worker_unlock(struct hakios_mhi_worker *worker, boolean_t interrupts)
{
	lck_spin_unlock(&worker->gate);
	ml_set_interrupts_enabled(interrupts);
}

static int
worker_error(int error)
{
	return error <= 0 || error == EAGAIN || error == ENOSPC || error == EBUSY ? EIO : error;
}

static void
worker_fault_locked(struct hakios_mhi_worker *worker, int error)
{
	if (worker->status.fault == 0) {
		worker->status.fault = worker_error(error);
	}
}

static void
worker_irq(void *context, uint32_t intid)
{
	struct mhi_irq_context *irq = context;
	struct hakios_mhi_worker *worker = irq->worker;
	boolean_t interrupts = worker_lock(worker);
	if (worker->accepting) {
		int error = worker->operations.mask(worker->owner, &worker->config.vectors[irq->index]);
		if (error != 0 || intid != worker->config.vectors[irq->index].intid) {
			worker_fault_locked(worker, error != 0 ? error : EPROTO);
		}
		worker->status.pending |= 1u << irq->index;
		thread_call_enter(worker->call);
	}
	worker_unlock(worker, interrupts);
}

static int
worker_events(struct hakios_mhi_worker *worker, struct hakios_mhi_event_ring *ring, uint32_t source, uint32_t *retry)
{
	uint32_t dispatched;
	int error = hakios_mhi_event_poll(ring, worker->config.event_budget, &dispatched);
	if ((error == EAGAIN || error == ENOSPC) && hakios_mhi_event_status(ring) == 0) {
		*retry |= source;
		return 0;
	}
	if (error == 0 && dispatched == worker->config.event_budget) {
		*retry |= source;
	}
	return error;
}

static int
worker_ipcr(struct hakios_mhi_worker *worker, uint32_t *retry)
{
	struct hakios_ipcr_status status;
	int error = hakios_ipcr_status(worker->config.ipcr, &status);
	if (error != 0 || status.fault != 0 || status.stopped) {
		return error != 0 ? error : status.fault != 0 ? status.fault : ENOTCONN;
	}
	if (!status.started || status.paused) {
		return 0;
	}
	uint32_t received;
	error = hakios_ipcr_work(worker->config.ipcr, worker->config.ipcr_budget, &received);
	if (error == EAGAIN || error == ENOSPC || error == EBUSY) {
		int check = hakios_ipcr_status(worker->config.ipcr, &status);
		if (check != 0 || status.fault != 0 || status.stopped) {
			return check != 0 ? check : status.fault != 0 ? status.fault : ENOTCONN;
		}
		if (error == EBUSY) {
			*retry |= HAKIOS_MHI_WORK_IPCR;
		}
		return 0;
	}
	if (error == 0 && received == worker->config.ipcr_budget) {
		*retry |= HAKIOS_MHI_WORK_IPCR;
	}
	return error;
}

static void
worker_run(thread_call_param_t parameter, thread_call_param_t unused)
{
	(void)unused;
	struct hakios_mhi_worker *worker = parameter;
	boolean_t interrupts = worker_lock(worker);
	if (!worker->accepting) {
		worker_unlock(worker, interrupts);
		return;
	}
	worker->callback_thread = current_thread();
	worker->status.runs++;
	uint32_t sources = worker->status.pending;
	worker->status.pending = 0;
	int error = worker->status.fault;
	worker_unlock(worker, interrupts);
	uint32_t retry = 0;
	if (error == 0 && (sources & HAKIOS_MHI_WORK_CONTROL) != 0) {
		error = worker_events(worker, worker->config.control, HAKIOS_MHI_WORK_CONTROL, &retry);
	}
	if (error == 0 && (sources & HAKIOS_MHI_WORK_DATA) != 0) {
		error = worker_events(worker, worker->config.data, HAKIOS_MHI_WORK_DATA, &retry);
	}
	if (error == 0 && (sources & (HAKIOS_MHI_WORK_STATE | HAKIOS_MHI_WORK_CONTROL)) != 0) {
		bool again = false;
		error = worker->operations.control(worker->owner, &again);
		if (again) {
			retry |= HAKIOS_MHI_WORK_STATE;
		}
	}
	if (error == 0 && (sources & (HAKIOS_MHI_WORK_DATA | HAKIOS_MHI_WORK_IPCR)) != 0) {
		error = worker_ipcr(worker, &retry);
	}
	interrupts = worker_lock(worker);
	if (error != 0) {
		worker_fault_locked(worker, error);
	}
	if (worker->accepting && worker->status.fault == 0) {
		for (uint32_t index = 0; index < 3; index++) {
			uint32_t source = 1u << index;
			if ((sources & source) == 0 || ((retry | worker->status.pending) & source) != 0) {
				continue;
			}
			error = worker->operations.unmask(worker->owner, &worker->config.vectors[index]);
			if (error != 0) {
				worker_fault_locked(worker, error);
				break;
			}
		}
	}
	bool notify = worker->status.fault != 0 && !worker->notified;
	if (worker->status.fault != 0) {
		worker->accepting = false;
		worker->notified = true;
		for (uint32_t index = 0; index < 3; index++) {
			if ((worker->status.attached & (1u << index)) != 0) {
				(void)worker->operations.mask(worker->owner, &worker->config.vectors[index]);
			}
		}
	}
	if (worker->accepting) {
		bool immediate = worker->status.pending != 0;
		worker->status.pending |= retry;
		if (immediate) {
			thread_call_enter(worker->call);
		} else if (retry != 0) {
			uint64_t deadline;
			clock_interval_to_deadline(worker->config.retry_ms, 1000000u, &deadline);
			thread_call_enter_delayed(worker->call, deadline);
		}
	}
	error = worker->status.fault;
	worker_unlock(worker, interrupts);
	if (notify) {
		worker->operations.fault(worker->owner, error);
	}
	interrupts = worker_lock(worker);
	worker->callback_thread = THREAD_NULL;
	worker_unlock(worker, interrupts);
}

int
hakios_mhi_worker_create(const struct hakios_mhi_worker_config *config,
    const struct hakios_mhi_worker_operations *operations, void *owner, struct hakios_mhi_worker **result)
{
	if (result == NULL) {
		return EINVAL;
	}
	*result = NULL;
	if (config == NULL || config->epoch == 0 || config->control == NULL || config->data == NULL ||
	    config->control == config->data || config->ipcr == NULL || config->event_budget == 0 ||
	    config->event_budget > HAKIOS_MHI_ELEMENTS || config->ipcr_budget == 0 ||
	    config->ipcr_budget > HAKIOS_IPCR_BUFFERS || config->retry_ms == 0 || config->retry_ms > 1000 ||
	    operations == NULL || owner == NULL || operations->version != HAKIOS_MHI_WORKER_VERSION ||
	    operations->attach == NULL || operations->mask == NULL || operations->unmask == NULL ||
	    operations->synchronize == NULL || operations->detach == NULL || operations->control == NULL ||
	    operations->fault == NULL) {
		return EINVAL;
	}
	for (uint32_t index = 0; index < 3; index++) {
		if (config->vectors[index].token == NULL || config->vectors[index].intid < 32 || config->vectors[index].intid >= 1020) {
			return EINVAL;
		}
		for (uint32_t prior = 0; prior < index; prior++) {
			if (config->vectors[index].intid == config->vectors[prior].intid ||
			    config->vectors[index].token == config->vectors[prior].token) {
				return EINVAL;
			}
		}
	}
	struct hakios_ipcr_status ipcr_status;
	int error = hakios_ipcr_status(config->ipcr, &ipcr_status);
	if (error != 0 || ipcr_status.epoch != config->epoch || ipcr_status.stopped || ipcr_status.fault != 0) {
		return error != 0 ? error : EINVAL;
	}
	struct hakios_mhi_worker *worker = kalloc_type(struct hakios_mhi_worker, Z_WAITOK | Z_ZERO);
	if (worker == NULL) {
		return ENOMEM;
	}
	worker->config = *config;
	worker->operations = *operations;
	worker->owner = owner;
	worker->status.epoch = config->epoch;
	worker->status.quiet = true;
	worker->status.irq_quiesced = true;
	lck_mtx_init(&worker->lifecycle, &mhi_worker_group, LCK_ATTR_NULL);
	lck_spin_init(&worker->gate, &mhi_worker_group, LCK_ATTR_NULL);
	worker->call = thread_call_allocate_with_options(worker_run, worker, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (worker->call == NULL) {
		lck_spin_destroy(&worker->gate, &mhi_worker_group);
		lck_mtx_destroy(&worker->lifecycle, &mhi_worker_group);
		kfree_type(struct hakios_mhi_worker, worker);
		return ENOMEM;
	}
	for (uint32_t index = 0; index < 3; index++) {
		worker->irqs[index] = (struct mhi_irq_context) {.worker = worker, .index = index};
	}
	*result = worker;
	return 0;
}

static int
worker_lifecycle_enter(struct hakios_mhi_worker *worker)
{
	if (worker == NULL) {
		return EINVAL;
	}
	boolean_t interrupts = worker_lock(worker);
	bool self = worker->callback_thread == current_thread();
	worker_unlock(worker, interrupts);
	if (self) {
		return EDEADLK;
	}
	lck_mtx_lock(&worker->lifecycle);
	return 0;
}

int
hakios_mhi_worker_start(struct hakios_mhi_worker *worker)
{
	int error = worker_lifecycle_enter(worker);
	if (error != 0) {
		return error;
	}
	boolean_t interrupts = worker_lock(worker);
	error = worker->status.fault != 0 ? worker->status.fault : worker->status.started ? EALREADY : 0;
	worker_unlock(worker, interrupts);
	for (uint32_t index = 0; error == 0 && index < 3; index++) {
		interrupts = worker_lock(worker);
		worker->status.attached |= 1u << index;
		worker->status.irq_quiesced = false;
		worker_unlock(worker, interrupts);
		error = worker->operations.attach(worker->owner, &worker->config.vectors[index], worker_irq, &worker->irqs[index]);
	}
	interrupts = worker_lock(worker);
	if (error == 0) {
		worker->status.started = true;
		worker->status.quiet = false;
		worker->accepting = true;
		worker->status.pending = HAKIOS_MHI_WORK_ALL;
		thread_call_enter(worker->call);
	} else if (error != EALREADY) {
		worker_fault_locked(worker, error);
		error = worker->status.fault;
	}
	worker_unlock(worker, interrupts);
	lck_mtx_unlock(&worker->lifecycle);
	return error;
}

int
hakios_mhi_worker_kick(struct hakios_mhi_worker *worker, uint64_t epoch, uint32_t sources)
{
	if (worker == NULL || sources == 0 || (sources & ~HAKIOS_MHI_WORK_ALL) != 0) {
		return EINVAL;
	}
	boolean_t interrupts = worker_lock(worker);
	int error = epoch != worker->config.epoch ? ESTALE : worker->status.fault != 0 ? worker->status.fault :
	    !worker->accepting ? EAGAIN : 0;
	if (error == 0) {
		worker->status.pending |= sources;
		thread_call_enter(worker->call);
	}
	worker_unlock(worker, interrupts);
	return error;
}

static int
worker_quiet(struct hakios_mhi_worker *worker)
{
	boolean_t interrupts = worker_lock(worker);
	worker->accepting = false;
	worker->status.quiet = true;
	worker_unlock(worker, interrupts);
	thread_call_cancel_wait(worker->call);
	int error = 0;
	for (uint32_t index = 0; index < 3; index++) {
		if ((worker->status.attached & (1u << index)) == 0) {
			continue;
		}
		int result = worker->operations.mask(worker->owner, &worker->config.vectors[index]);
		if (result == 0) {
			result = worker->operations.synchronize(worker->owner, &worker->config.vectors[index]);
		}
		if (result != 0 && error == 0) {
			error = worker_error(result);
		}
	}
	interrupts = worker_lock(worker);
	worker->status.pending = 0;
	worker->status.irq_quiesced = error == 0;
	if (error != 0) {
		worker_fault_locked(worker, error);
	}
	worker_unlock(worker, interrupts);
	return error;
}

int
hakios_mhi_worker_quiesce(struct hakios_mhi_worker *worker)
{
	int error = worker_lifecycle_enter(worker);
	if (error == 0) {
		error = worker_quiet(worker);
		lck_mtx_unlock(&worker->lifecycle);
	}
	return error;
}

int
hakios_mhi_worker_resume(struct hakios_mhi_worker *worker)
{
	int error = worker_lifecycle_enter(worker);
	if (error != 0) {
		return error;
	}
	boolean_t interrupts = worker_lock(worker);
	error = worker->status.fault != 0 ? worker->status.fault :
	    !worker->status.started || worker->status.attached != 7 || !worker->status.quiet ||
	    !worker->status.irq_quiesced ? EINVAL : 0;
	if (error == 0) {
		worker->status.quiet = false;
		worker->status.irq_quiesced = false;
		worker->accepting = true;
		worker->status.pending = HAKIOS_MHI_WORK_ALL;
		thread_call_enter(worker->call);
	}
	worker_unlock(worker, interrupts);
	lck_mtx_unlock(&worker->lifecycle);
	return error;
}

int
hakios_mhi_worker_status(struct hakios_mhi_worker *worker, struct hakios_mhi_worker_status *status)
{
	if (worker == NULL || status == NULL) {
		return EINVAL;
	}
	boolean_t interrupts = worker_lock(worker);
	*status = worker->status;
	worker_unlock(worker, interrupts);
	return 0;
}

int
hakios_mhi_worker_destroy(struct hakios_mhi_worker **worker)
{
	if (worker == NULL || *worker == NULL) {
		return EINVAL;
	}
	struct hakios_mhi_worker *state = *worker;
	int error = worker_lifecycle_enter(state);
	if (error != 0) {
		return error;
	}
	error = worker_quiet(state);
	for (uint32_t index = 0; error == 0 && index < 3; index++) {
		if ((state->status.attached & (1u << index)) == 0) {
			continue;
		}
		error = state->operations.detach(state->owner, &state->config.vectors[index]);
		boolean_t interrupts = worker_lock(state);
		if (error == 0) {
			state->status.attached &= ~(1u << index);
		} else {
			worker_fault_locked(state, error);
			error = worker_error(error);
		}
		worker_unlock(state, interrupts);
	}
	if (error == 0 && !thread_call_free(state->call)) {
		error = EBUSY;
	}
	lck_mtx_unlock(&state->lifecycle);
	if (error != 0) {
		return error;
	}
	lck_spin_destroy(&state->gate, &mhi_worker_group);
	lck_mtx_destroy(&state->lifecycle, &mhi_worker_group);
	kfree_type(struct hakios_mhi_worker, state);
	*worker = NULL;
	return 0;
}

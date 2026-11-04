#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <sys/hakios.mhi.worker.h>
#include <sys/hakios.qrtr.wire.h>
#include <sys/hakios.wire.h>

bool pcie_test_fail_allocation;
unsigned pcie_test_allocation_calls;
unsigned pcie_test_fail_allocation_at;

struct worker_model;

struct irq_model {
	struct worker_model *model;
	void (*handler)(void *, uint32_t);
	void *context;
	uint32_t intid;
	atomic_bool masked;
	atomic_uint unmasks;
};

struct event_model {
	struct worker_model *model;
	struct hakios_mhi_event_ring *ring;
	_Alignas(16) uint8_t bytes[4096];
	uint64_t iova;
	atomic_uint producer;
	atomic_uint delivered;
	uint8_t index;
};

enum failure_site { FAIL_NONE, FAIL_ATTACH, FAIL_MASK, FAIL_UNMASK, FAIL_SYNC, FAIL_DETACH, FAIL_CONTROL, FAIL_EVENT };

struct worker_model {
	struct hakios_mhi_worker *worker;
	struct hakios_mhi_worker_config config;
	struct hakios_ipcr *ipcr;
	struct hakios_qrtr_endpoint endpoint;
	struct hakios_qrtr_port *port;
	struct irq_model irqs[3];
	struct event_model events[2];
	_Alignas(16) uint8_t transfers[2][1024];
	uint8_t buffers[2][8192];
	struct hakios_ipcr_buffer dma[2];
	atomic_uint transfer_wp[2];
	atomic_uint control_calls;
	atomic_uint rx_calls;
	atomic_uint fault_calls;
	atomic_uint fail_site;
	atomic_int failure;
	atomic_bool after;
	atomic_bool block;
	atomic_bool entered;
	atomic_bool release;
	atomic_bool retry;
	atomic_bool quiescing;
	atomic_bool quiesced;
	bool stopped;
};

static int
fail(struct worker_model *model, unsigned site)
{
	unsigned expected = site;
	return atomic_compare_exchange_strong(&model->fail_site, &expected, FAIL_NONE) ? atomic_load(&model->failure) : 0;
}

static void
wait_count(atomic_uint *counter, unsigned target)
{
	uint64_t deadline = pcie_test_clock() + 3000000000ULL;
	while (atomic_load(counter) < target && pcie_test_clock() < deadline) {
		struct timespec delay = {.tv_nsec = 1000000};
		nanosleep(&delay, NULL);
	}
	assert(atomic_load(counter) >= target);
}

static int
event_sync(void *owner, uint32_t offset, uint32_t length, bool for_device)
{
	struct event_model *event = owner;
	assert(!pcie_test_interrupts_disabled && offset + length <= sizeof(event->bytes));
	(void)for_device;
	return 0;
}

static int
event_pointer(void *owner, uint64_t *pointer)
{
	struct event_model *event = owner;
	assert(!pcie_test_interrupts_disabled);
	*pointer = event->iova + atomic_load(&event->producer) * 16;
	return 0;
}

static int
event_publish(void *owner, uint64_t pointer)
{
	struct event_model *event = owner;
	assert(!pcie_test_interrupts_disabled && pointer >= event->iova);
	return 0;
}

static int
event_dispatch(void *owner, const uint8_t *bytes)
{
	struct event_model *event = owner;
	assert(!pcie_test_interrupts_disabled);
	int error = fail(event->model, FAIL_EVENT);
	if (error != 0) {
		return error;
	}
	if (event->index == 1) {
		error = hakios_ipcr_complete(event->model->ipcr, 23, bytes);
	}
	if (error == 0) {
		atomic_fetch_add(&event->delivered, 1);
	}
	return error;
}

static int
event_quiesce(void *owner)
{
	struct event_model *event = owner;
	assert(event->model->worker == NULL && event->model->stopped);
	return 0;
}

static int
buffer_sync(void *owner, uint8_t channel, uint32_t index, bool for_device, uint32_t length)
{
	struct worker_model *model = owner;
	assert(!pcie_test_interrupts_disabled && !model->stopped && channel >= 20 && channel <= 21 && index == 0 && length <= 8192);
	(void)for_device;
	return 0;
}

static int
transfer_sync(void *owner, uint8_t channel, uint32_t offset, uint32_t length)
{
	struct worker_model *model = owner;
	assert(!pcie_test_interrupts_disabled && !model->stopped && channel >= 20 && channel <= 21 && offset < 1024 && length == 16);
	return 0;
}

static int
transfer_publish(void *owner, uint8_t channel, uint64_t pointer)
{
	struct worker_model *model = owner;
	assert(!pcie_test_interrupts_disabled && !model->stopped);
	atomic_store(&model->transfer_wp[channel - 20], (unsigned)((pointer - (0x400000 + (channel - 20) * 4096)) / 16));
	return 0;
}

static void
schedule_ipcr(void *owner)
{
	struct worker_model *model = owner;
	if (model->worker != NULL) {
		int error = hakios_mhi_worker_kick(model->worker, 23, HAKIOS_MHI_WORK_IPCR);
		assert(error == 0 || error == EAGAIN || error == EIO || error == ENOTCONN);
	}
}

static int
ipcr_quiesce(void *owner)
{
	struct worker_model *model = owner;
	assert(model->worker == NULL);
	model->stopped = true;
	return 0;
}

static struct irq_model *
grant_irq(void *owner, const struct hakios_mhi_irq_grant *grant)
{
	struct worker_model *model = owner;
	for (unsigned index = 0; index < 3; index++) {
		if (grant->token == &model->irqs[index]) {
			assert(grant->intid == model->irqs[index].intid);
			return &model->irqs[index];
		}
	}
	abort();
}

static int
irq_attach(void *owner, const struct hakios_mhi_irq_grant *grant, void (*handler)(void *, uint32_t), void *context)
{
	struct irq_model *irq = grant_irq(owner, grant);
	assert(!pcie_test_interrupts_disabled && atomic_load(&irq->masked));
	int error = fail(owner, FAIL_ATTACH);
	if (error == 0 || atomic_load(&irq->model->after)) {
		irq->handler = handler;
		irq->context = context;
	}
	return error;
}

static int
irq_mask(void *owner, const struct hakios_mhi_irq_grant *grant)
{
	struct irq_model *irq = grant_irq(owner, grant);
	int error = fail(owner, FAIL_MASK);
	if (error == 0 || atomic_load(&irq->model->after)) {
		atomic_store(&irq->masked, true);
	}
	return error;
}

static int
irq_unmask(void *owner, const struct hakios_mhi_irq_grant *grant)
{
	struct irq_model *irq = grant_irq(owner, grant);
	assert(pcie_test_interrupts_disabled && irq->handler != NULL);
	int error = fail(owner, FAIL_UNMASK);
	if (error == 0 || atomic_load(&irq->model->after)) {
		atomic_store(&irq->masked, false);
		atomic_fetch_add(&irq->unmasks, 1);
	}
	return error;
}

static int
irq_sync(void *owner, const struct hakios_mhi_irq_grant *grant)
{
	struct irq_model *irq = grant_irq(owner, grant);
	assert(!pcie_test_interrupts_disabled && atomic_load(&irq->masked));
	return fail(owner, FAIL_SYNC);
}

static int
irq_detach(void *owner, const struct hakios_mhi_irq_grant *grant)
{
	struct irq_model *irq = grant_irq(owner, grant);
	assert(!pcie_test_interrupts_disabled && atomic_load(&irq->masked));
	int error = fail(owner, FAIL_DETACH);
	if (error == 0 || atomic_load(&irq->model->after)) {
		irq->handler = NULL;
		irq->context = NULL;
	}
	return error;
}

static int
control(void *owner, bool *retry)
{
	struct worker_model *model = owner;
	assert(!pcie_test_interrupts_disabled);
	assert(hakios_mhi_worker_quiesce(model->worker) == EDEADLK);
	struct hakios_mhi_worker *copy = model->worker;
	assert(hakios_mhi_worker_destroy(&copy) == EDEADLK && copy == model->worker);
	atomic_fetch_add(&model->control_calls, 1);
	if (atomic_load(&model->block)) {
		atomic_store(&model->entered, true);
		while (!atomic_load(&model->release)) {
			sched_yield();
		}
	}
	*retry = atomic_load(&model->retry);
	return fail(model, FAIL_CONTROL);
}

static void
worker_fault(void *owner, int error)
{
	struct worker_model *model = owner;
	assert(!pcie_test_interrupts_disabled && error != 0);
	assert(hakios_mhi_worker_quiesce(model->worker) == EDEADLK);
	for (unsigned index = 0; index < 3; index++) {
		assert(atomic_load(&model->irqs[index].masked));
	}
	atomic_fetch_add(&model->fault_calls, 1);
}

static const struct hakios_mhi_worker_operations worker_operations = {
	.version = HAKIOS_MHI_WORKER_VERSION, .attach = irq_attach, .mask = irq_mask, .unmask = irq_unmask,
	.synchronize = irq_sync, .detach = irq_detach, .control = control, .fault = worker_fault,
};

static struct worker_model *
model_new(void)
{
	struct worker_model *model = calloc(1, sizeof(*model));
	assert(model != NULL);
	for (unsigned index = 0; index < 3; index++) {
		model->irqs[index].model = model;
		model->irqs[index].intid = 120 + index;
		atomic_init(&model->irqs[index].masked, true);
		model->config.vectors[index] = (struct hakios_mhi_irq_grant) {.token = &model->irqs[index], .intid = 120 + index};
	}
	const struct hakios_mhi_event_operations event_operations = {
		.version = HAKIOS_MHI_VERSION, .sync_ring = event_sync, .read_pointer = event_pointer,
		.publish = event_publish, .dispatch = event_dispatch, .quiesce = event_quiesce,
	};
	for (unsigned index = 0; index < 2; index++) {
		struct event_model *event = &model->events[index];
		event->model = model;
		event->index = index;
		event->iova = 0x100000 + index * 0x10000;
		struct hakios_mhi_event_config config = {.elements = event->bytes, .iova = event->iova, .index = index};
		assert(hakios_mhi_event_create(&config, &event_operations, event, &event->ring) == 0);
		assert(hakios_mhi_event_start(event->ring) == 0);
		model->dma[index] = (struct hakios_ipcr_buffer) {.bytes = model->buffers[index], .iova = 0x200000 + index * 0x10000};
	}
	struct hakios_ipcr_config config = {
		.epoch = 23, .tx_ring = {.elements = model->transfers[0], .iova = 0x400000, .channel = 20},
		.rx_ring = {.elements = model->transfers[1], .iova = 0x401000, .channel = 21},
		.tx_buffers = &model->dma[0], .rx_buffers = &model->dma[1], .tx_count = 1, .rx_count = 1,
		.router = {.local_node = 1, .local_port = 0x4000},
	};
	const struct hakios_ipcr_operations ipcr_operations = {
		.version = HAKIOS_IPCR_VERSION, .sync_buffer = buffer_sync, .sync_ring = transfer_sync,
		.publish = transfer_publish, .schedule = schedule_ipcr, .quiesce = ipcr_quiesce,
	};
	assert(hakios_ipcr_create(&config, &ipcr_operations, model, &model->ipcr) == 0);
	model->config.epoch = 23;
	model->config.control = model->events[0].ring;
	model->config.data = model->events[1].ring;
	model->config.ipcr = model->ipcr;
	model->config.event_budget = 2;
	model->config.ipcr_budget = 2;
	model->config.retry_ms = 2;
	assert(hakios_mhi_worker_create(&model->config, &worker_operations, model, &model->worker) == 0);
	return model;
}

static void
model_free(struct worker_model *model)
{
	atomic_store(&model->fail_site, FAIL_NONE);
	assert(hakios_mhi_worker_destroy(&model->worker) == 0 && model->worker == NULL);
	assert(hakios_ipcr_destroy(&model->ipcr) == 0);
	model->stopped = true;
	assert(hakios_mhi_event_destroy(&model->events[0].ring) == 0);
	assert(hakios_mhi_event_destroy(&model->events[1].ring) == 0);
	free(model);
}

static void
raise_irq(struct worker_model *model, unsigned index)
{
	struct irq_model *irq = &model->irqs[index];
	assert(irq->handler != NULL);
	bool old = ml_set_interrupts_enabled(false);
	irq->handler(irq->context, irq->intid);
	ml_set_interrupts_enabled(old);
}

static void
test_dispatch(void)
{
	struct worker_model *model = model_new();
	assert(hakios_mhi_worker_kick(model->worker, 22, 1) == ESTALE);
	assert(hakios_mhi_worker_kick(model->worker, 23, 1) == EAGAIN);
	assert(hakios_mhi_worker_start(model->worker) == 0);
	for (unsigned index = 0; index < 3; index++) {
		wait_count(&model->irqs[index].unmasks, 1);
	}
	assert(hakios_mhi_worker_start(model->worker) == EALREADY);
	for (unsigned index = 0; index < 10; index++) {
		hakios_wire_put32(model->events[0].bytes + index * 16 + 8, 2u << 24);
		hakios_wire_put32(model->events[0].bytes + index * 16 + 12, 0x20u << 16);
	}
	atomic_store(&model->events[0].producer, 10);
	raise_irq(model, 1);
	wait_count(&model->events[0].delivered, 10);
	wait_count(&model->irqs[1].unmasks, 2);
	assert(hakios_ipcr_start(model->ipcr) == 0);
	wait_count(&model->transfer_wp[0], 1);
	wait_count(&model->transfer_wp[1], 1);
	uint8_t *event = model->events[1].bytes;
	hakios_wire_put64(event, 0x400000);
	hakios_wire_put32(event + 8, 2u << 24);
	hakios_wire_put32(event + 12, 20u << 24 | 0x22u << 16);
	atomic_store(&model->events[1].producer, 1);
	raise_irq(model, 2);
	wait_count(&model->events[1].delivered, 1);
	assert(hakios_mhi_worker_quiesce(model->worker) == 0);
	struct hakios_ipcr_status ipcr;
	assert(hakios_ipcr_status(model->ipcr, &ipcr) == 0 && ipcr.tx_pending == 0);
	struct hakios_mhi_worker_status status;
	assert(hakios_mhi_worker_status(model->worker, &status) == 0 && status.quiet && status.irq_quiesced);
	uint64_t runs = status.runs;
	for (unsigned index = 0; index < 3; index++) {
		assert(atomic_load(&model->irqs[index].masked));
		raise_irq(model, index);
	}
	assert(hakios_mhi_worker_status(model->worker, &status) == 0 && status.runs == runs && status.pending == 0);
	assert(hakios_mhi_worker_resume(model->worker) == 0);
	wait_count(&model->irqs[0].unmasks, 2);
	model_free(model);
}

static void *
quiesce_thread(void *owner)
{
	struct worker_model *model = owner;
	atomic_store(&model->quiescing, true);
	assert(hakios_mhi_worker_quiesce(model->worker) == 0);
	atomic_store(&model->quiesced, true);
	return NULL;
}

static void
test_cancel_and_coalesce(void)
{
	struct worker_model *model = model_new();
	atomic_store(&model->block, true);
	assert(hakios_mhi_worker_start(model->worker) == 0);
	while (!atomic_load(&model->entered)) {
		sched_yield();
	}
	for (unsigned count = 0; count < 10000; count++) {
		assert(hakios_mhi_worker_kick(model->worker, 23, HAKIOS_MHI_WORK_ALL) == 0);
	}
	pthread_t stopping;
	assert(pthread_create(&stopping, NULL, quiesce_thread, model) == 0);
	while (!atomic_load(&model->quiescing)) {
		sched_yield();
	}
	assert(!atomic_load(&model->quiesced));
	atomic_store(&model->release, true);
	assert(pthread_join(stopping, NULL) == 0 && atomic_load(&model->quiesced));
	assert(atomic_load(&model->control_calls) <= 2);
	model_free(model);
	model = model_new();
	atomic_store(&model->retry, true);
	assert(hakios_mhi_worker_start(model->worker) == 0);
	wait_count(&model->control_calls, 5);
	assert(hakios_mhi_worker_quiesce(model->worker) == 0);
	unsigned calls = atomic_load(&model->control_calls);
	struct timespec delay = {.tv_nsec = 10000000};
	nanosleep(&delay, NULL);
	assert(atomic_load(&model->control_calls) == calls);
	model_free(model);
}

static void
test_rejections(void)
{
	struct worker_model *model = model_new();
	for (unsigned scenario = 0; scenario < 6; scenario++) {
		struct hakios_mhi_worker_config config = model->config;
		switch (scenario) {
		case 0: config.vectors[1].token = config.vectors[0].token; break;
		case 1: config.vectors[1].intid = config.vectors[0].intid; break;
		case 2: config.control = config.data; break;
		case 3: config.epoch++; break;
		case 4: config.event_budget = 0; break;
		case 5: config.retry_ms = 1001; break;
		}
		struct hakios_mhi_worker *rejected = NULL;
		assert(hakios_mhi_worker_create(&config, &worker_operations, model, &rejected) == EINVAL && rejected == NULL);
	}
	for (unsigned allocation = 1; allocation <= 2; allocation++) {
		pcie_test_fail_allocation_at = pcie_test_allocation_calls + allocation;
		struct hakios_mhi_worker *rejected = NULL;
		assert(hakios_mhi_worker_create(&model->config, &worker_operations, model, &rejected) == ENOMEM && rejected == NULL);
		pcie_test_fail_allocation_at = 0;
	}
	assert(hakios_mhi_worker_start(model->worker) == 0);
	wait_count(&model->irqs[2].unmasks, 1);
	assert(hakios_mhi_worker_quiesce(model->worker) == 0);
	pcie_test_thread_call_free_fail = true;
	assert(hakios_mhi_worker_destroy(&model->worker) == EBUSY && model->worker != NULL);
	assert(hakios_mhi_worker_resume(model->worker) == EINVAL);
	model_free(model);
}

static void
test_failures(void)
{
	const int failures[] = {EIO, EAGAIN, ENOSPC, -EIO};
	unsigned injected = 0;
	for (unsigned after = 0; after < 2; after++) {
		for (unsigned failure = 0; failure < sizeof(failures) / sizeof(failures[0]); failure++) {
			for (unsigned site = FAIL_ATTACH; site <= FAIL_EVENT; site++) {
				struct worker_model *model = model_new();
				atomic_store(&model->after, after != 0);
				atomic_store(&model->failure, failures[failure]);
				if (site == FAIL_ATTACH || site == FAIL_UNMASK || site == FAIL_CONTROL) {
					atomic_store(&model->fail_site, site);
				}
				int error = hakios_mhi_worker_start(model->worker);
				if (site == FAIL_ATTACH) {
					assert(error == EIO);
				} else {
					assert(error == 0);
					if (site != FAIL_UNMASK && site != FAIL_CONTROL) {
						wait_count(&model->irqs[2].unmasks, 1);
						atomic_store(&model->fail_site, site);
					}
					if (site == FAIL_MASK) {
						raise_irq(model, 0);
					} else if (site == FAIL_EVENT) {
						hakios_wire_put32(model->events[0].bytes + 8, 2u << 24);
						hakios_wire_put32(model->events[0].bytes + 12, 0x20u << 16);
						atomic_store(&model->events[0].producer, 1);
						raise_irq(model, 1);
						if (failures[failure] == EAGAIN || failures[failure] == ENOSPC) {
							wait_count(&model->events[0].delivered, 1);
							assert(atomic_load(&model->fault_calls) == 0);
							model_free(model);
							injected++;
							continue;
						}
					} else if (site == FAIL_SYNC || site == FAIL_DETACH) {
						assert(hakios_mhi_worker_destroy(&model->worker) == EIO && model->worker != NULL);
					}
					if (site != FAIL_SYNC && site != FAIL_DETACH) {
						wait_count(&model->fault_calls, 1);
					}
				}
				struct hakios_mhi_worker_status status;
				assert(hakios_mhi_worker_status(model->worker, &status) == 0 && status.fault == EIO);
				assert(hakios_mhi_worker_kick(model->worker, 23, 1) == EIO);
				model_free(model);
				injected++;
			}
		}
	}
	printf("mhi-worker failures: %u\n", injected);
}

int
main(void)
{
	test_dispatch();
	test_cancel_and_coalesce();
	test_rejections();
	test_failures();
	puts("mhi-worker-test: PASS (native thread-call engine, IRQ deferral, real event/IPCR dispatch, delayed retries, drain)");
	return 0;
}
